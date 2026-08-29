"""
cocotb testbench for moldudp64_deframer: header parse, sequence/gap tracking,
the gearbox realignment buffer, and message-body streaming.

Covers: session/seq_num/count byte-order latching, gap detection, duplicate
drop (both zero-body and with leftover message bytes needing WAIT_NEXT to
drain), heartbeat/end-of-session special counts, back-to-back packets,
single/multiple/zero-length message bodies, mid-body and header-handshake
backpressure, and truncation detection at both the header and message level.
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, ClockCycles, with_timeout

from cocotbext.axi import AxiStreamBus, AxiStreamSource, AxiStreamSink

STATE_IDLE, STATE_READ_HEADER, STATE_READ_MESSAGE, STATE_WAIT_NEXT = 0, 1, 2, 3
RECV_TIMEOUT_NS = 5000


def build_header(seq_num, count, session=b"SESSION001"):
    assert len(session) == 10
    return session + seq_num.to_bytes(8, "big") + count.to_bytes(2, "big")


def build_message_block(data):
    return len(data).to_bytes(2, "big") + data


class TB:
    def __init__(self, dut):
        self.dut = dut
        self.clock = Clock(dut.clk, 10, unit="ns")
        self.source = AxiStreamSource(
            AxiStreamBus.from_prefix(dut, "s_udp_payload_axis"), dut.clk, dut.rst, reset_active_level=True)
        self.body_sink = AxiStreamSink(
            AxiStreamBus.from_prefix(dut, "m_msg_payload_axis"), dut.clk, dut.rst, reset_active_level=True)
        self.hdr_events = []

    async def start(self):
        self.clock.start()
        self.dut.rst.value = 1
        self.dut.m_msg_hdr_ready.value = 1
        await ClockCycles(self.dut.clk, 3)
        self.dut.rst.value = 0
        cocotb.start_soon(self._monitor_hdr())
        await RisingEdge(self.dut.clk)

    async def _monitor_hdr(self):
        while True:
            await RisingEdge(self.dut.clk)
            if int(self.dut.m_msg_hdr_valid.value) and int(self.dut.m_msg_hdr_ready.value):
                self.hdr_events.append({
                    "seq_num": int(self.dut.m_msg_seq_num.value),
                    "length": int(self.dut.m_msg_length.value),
                })

    async def send(self, payload):
        await self.source.send(payload)

    async def wait_state(self, target, timeout_cycles=300):
        for _ in range(timeout_cycles):
            await RisingEdge(self.dut.clk)
            if int(self.dut.state.value) == target:
                return
        raise TimeoutError(f"never reached state {target}, stuck at {int(self.dut.state.value)}")

    async def wait_left_idle(self, timeout_cycles=300):
        """state==IDLE trivially holds the instant a send is merely queued
        (AxiStreamSource.send() returns as soon as it's enqueued, before any
        byte has actually moved) -- so every round-trip check needs to first
        confirm we actually left IDLE before waiting to come back to it."""
        for _ in range(timeout_cycles):
            await RisingEdge(self.dut.clk)
            if int(self.dut.state.value) != STATE_IDLE:
                return
        raise TimeoutError("never left IDLE after send")

    async def send_and_wait_round_trip(self, payload, timeout_cycles=300):
        await self.send(payload)
        await self.wait_left_idle(timeout_cycles)
        await self.wait_state(STATE_IDLE, timeout_cycles=timeout_cycles)
        await RisingEdge(self.dut.clk)  # let the completing cycle's writes settle

    async def recv_body(self):
        frame = await with_timeout(self.body_sink.recv(compact=True), RECV_TIMEOUT_NS, "ns")
        return bytes(frame)


# ---------------------------------------------------------------------------
# Header parse, sequencing, gaps, special counts, WAIT_NEXT drain
# ---------------------------------------------------------------------------

@cocotb.test()
async def test_normal_packet_reaches_read_message(dut):
    tb = TB(dut)
    await tb.start()

    # seq_num=0 matches the post-reset expected_seq=0 -- a genuinely
    # gap-free first packet. (100 would ALSO legitimately set gap_valid,
    # since expected_seq starts at 0 -- that's correct RTL behavior, not
    # something to test around here.)
    pkt = build_header(seq_num=0, count=2) + build_message_block(b"AB") + build_message_block(b"CD")
    await tb.send(pkt)
    await tb.wait_state(STATE_READ_MESSAGE)

    assert int(dut.seq_num.value) == 0, f"seq_num={int(dut.seq_num.value)}"
    assert int(dut.count.value) == 2, f"count={int(dut.count.value)}"
    assert int(dut.msgs_remaining.value) == 2, f"msgs_remaining={int(dut.msgs_remaining.value)}"
    assert int(dut.gap_valid.value) == 0
    assert int(dut.session.value).to_bytes(10, "big") == b"SESSION001"
    dut._log.info("normal packet test passed")


@cocotb.test()
async def test_heartbeat(dut):
    tb = TB(dut)
    await tb.start()

    await tb.send_and_wait_round_trip(build_header(seq_num=50, count=0))

    assert int(dut.expected_seq.value) == 50, f"expected_seq={int(dut.expected_seq.value)}"
    assert int(dut.end_of_session.value) == 0
    dut._log.info("heartbeat test passed")


@cocotb.test()
async def test_end_of_session(dut):
    tb = TB(dut)
    await tb.start()

    await tb.send_and_wait_round_trip(build_header(seq_num=77, count=0xFFFF))

    assert int(dut.expected_seq.value) == 77
    assert int(dut.end_of_session.value) == 1
    dut._log.info("end-of-session test passed")


@cocotb.test()
async def test_gap_detection(dut):
    tb = TB(dut)
    await tb.start()

    # expected_seq starts at 0; send seq_num=10 (a gap), count=0 (heartbeat,
    # simplest way to exercise just the gap check in isolation)
    await tb.send_and_wait_round_trip(build_header(seq_num=10, count=0))

    assert int(dut.gap_valid.value) == 1
    assert int(dut.gap_expected_seq.value) == 0
    assert int(dut.expected_seq.value) == 10, "heartbeat still resyncs after flagging the gap"
    dut._log.info("gap detection test passed")


@cocotb.test()
async def test_duplicate_zero_body(dut):
    tb = TB(dut)
    await tb.start()

    # establish expected_seq = 20
    await tb.send_and_wait_round_trip(build_header(seq_num=20, count=0))
    assert int(dut.expected_seq.value) == 20

    # duplicate heartbeat, seq_num < expected_seq, no leftover bytes
    await tb.send_and_wait_round_trip(build_header(seq_num=15, count=0))

    assert int(dut.expected_seq.value) == 20, "duplicate must not change expected_seq"
    assert int(dut.gap_valid.value) == 0, "a duplicate is not a gap"
    dut._log.info("duplicate (zero-body) test passed")


@cocotb.test()
async def test_duplicate_with_leftover_drains_via_wait_next(dut):
    tb = TB(dut)
    await tb.start()

    # establish expected_seq = 100
    await tb.send_and_wait_round_trip(build_header(seq_num=100, count=0))
    assert int(dut.expected_seq.value) == 100

    # duplicate WITH a real message body -- must route through WAIT_NEXT,
    # not straight to IDLE, or the leftover bytes would be misread as the
    # next packet's header.
    dup_pkt = build_header(seq_num=50, count=1) + build_message_block(b"HELLOWORLD")
    await tb.send(dup_pkt)
    await tb.wait_state(STATE_WAIT_NEXT, timeout_cycles=50)
    await tb.wait_state(STATE_IDLE, timeout_cycles=50)
    await RisingEdge(dut.clk)

    assert int(dut.expected_seq.value) == 100, "duplicate must not change expected_seq"

    # and the deframer must be able to correctly parse a fresh packet
    # immediately afterward -- proves WAIT_NEXT fully consumed the dup's
    # bytes and didn't leave anything behind to corrupt the next header.
    await tb.send_and_wait_round_trip(build_header(seq_num=100, count=0))
    assert int(dut.expected_seq.value) == 100

    dut._log.info("duplicate-with-leftover / WAIT_NEXT drain test passed")


@cocotb.test()
async def test_back_to_back_packets(dut):
    tb = TB(dut)
    await tb.start()

    pkt_a = build_header(seq_num=0, count=0)   # heartbeat, expected_seq: 0->0
    pkt_b = build_header(seq_num=5, count=0)   # gap 0->5, then resync to 5

    # Enqueue both before the sim advances -- no idle gap between frames on
    # the wire, same as the RX-pipeline back-to-back test.
    await tb.source.send(pkt_a)
    await tb.source.send(pkt_b)

    await tb.wait_left_idle()
    await tb.wait_state(STATE_IDLE)
    await RisingEdge(dut.clk)
    first_expected = int(dut.expected_seq.value)

    await tb.wait_left_idle()
    await tb.wait_state(STATE_IDLE)
    await RisingEdge(dut.clk)
    second_expected = int(dut.expected_seq.value)

    assert first_expected == 0, f"after packet A: expected_seq={first_expected}"
    assert second_expected == 5, f"after packet B: expected_seq={second_expected}"
    dut._log.info("back-to-back packets test passed")


# ---------------------------------------------------------------------------
# Message-body streaming, sideband descriptor, backpressure, truncation
# ---------------------------------------------------------------------------

@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_single_message(dut):
    tb = TB(dut)
    await tb.start()

    payload = bytes([0x41 + i for i in range(20)])  # 20-byte ITCH-ish body, type byte = 0x41 'A'
    pkt = build_header(seq_num=1, count=1) + build_message_block(payload)
    await tb.send(pkt)

    body = await tb.recv_body()
    await tb.wait_state(STATE_IDLE)
    await RisingEdge(dut.clk)

    assert body == payload, f"body mismatch: got {body!r} expected {payload!r}"
    assert int(dut.m_msg_type.value) == payload[0], f"type={int(dut.m_msg_type.value):#x} expected {payload[0]:#x}"
    assert int(dut.m_msg_length.value) == len(payload)
    assert len(tb.hdr_events) == 1
    assert tb.hdr_events[0]["seq_num"] == 1
    assert tb.hdr_events[0]["length"] == len(payload)
    assert int(dut.error_truncated.value) == 0
    dut._log.info("single message test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_multiple_messages(dut):
    tb = TB(dut)
    await tb.start()

    bodies = [b"AAAA", b"BB", b"CCCCCCCCCC", b"D"]
    pkt = build_header(seq_num=10, count=len(bodies))
    for b in bodies:
        pkt += build_message_block(b)
    await tb.send(pkt)

    received = []
    for _ in bodies:
        received.append(await tb.recv_body())

    await tb.wait_state(STATE_IDLE)
    await RisingEdge(dut.clk)

    assert received == bodies, f"got {received!r} expected {bodies!r}"
    assert [e["seq_num"] for e in tb.hdr_events] == [10, 11, 12, 13]
    assert [e["length"] for e in tb.hdr_events] == [len(b) for b in bodies]
    assert int(dut.expected_seq.value) == 14  # seq_num(10) + count(4)
    dut._log.info("multiple messages test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_zero_length_message(dut):
    tb = TB(dut)
    await tb.start()

    # message 0: zero-length, message 1: real body -- proves the zero-length
    # path correctly skips MSG_BODY and doesn't desync the loop
    pkt = build_header(seq_num=5, count=2) + build_message_block(b"") + build_message_block(b"XYZ")
    await tb.send(pkt)

    body = await tb.recv_body()  # only message 1 has a body to receive
    await tb.wait_state(STATE_IDLE)
    await RisingEdge(dut.clk)

    assert body == b"XYZ"
    assert [e["seq_num"] for e in tb.hdr_events] == [5, 6]
    assert [e["length"] for e in tb.hdr_events] == [0, 3]
    assert tb.body_sink.empty()
    dut._log.info("zero-length message test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_mid_body_backpressure(dut):
    tb = TB(dut)
    await tb.start()

    payload = bytes(range(64))  # 64 bytes, several beats
    pkt = build_header(seq_num=1, count=1) + build_message_block(payload)

    async def backpressure_controller():
        while not (int(dut.m_msg_payload_axis_tvalid.value) and int(dut.m_msg_payload_axis_tready.value)):
            await RisingEdge(dut.clk)
        await ClockCycles(dut.clk, 2)
        tb.body_sink.pause = True
        await ClockCycles(dut.clk, 6)
        tb.body_sink.pause = False

    cocotb.start_soon(backpressure_controller())

    await tb.send(pkt)
    body = await tb.recv_body()
    await tb.wait_state(STATE_IDLE)
    await RisingEdge(dut.clk)

    assert body == payload, f"body corrupted under backpressure: got {len(body)} bytes, expected {len(payload)}"
    dut._log.info("mid-body backpressure test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_hdr_ready_backpressure(dut):
    tb = TB(dut)
    dut.m_msg_hdr_ready.value = 0  # override TB.start()'s default; withhold from the start
    tb.clock.start()
    dut.rst.value = 1
    await ClockCycles(dut.clk, 3)
    dut.rst.value = 0
    cocotb.start_soon(tb._monitor_hdr())
    await RisingEdge(dut.clk)

    payload = b"HELLO"
    pkt = build_header(seq_num=1, count=1) + build_message_block(payload)
    await tb.send(pkt)

    # let the header land and sit held (m_msg_hdr_valid asserted, not yet accepted)
    for _ in range(60):
        await RisingEdge(dut.clk)
    assert int(dut.m_msg_hdr_valid.value) == 1, "hdr should be held asserted while hdr_ready is low"
    assert int(dut.state.value) == STATE_READ_MESSAGE
    assert tb.body_sink.empty(), "body must not stream until the header handshake completes"

    dut.m_msg_hdr_ready.value = 1
    body = await tb.recv_body()
    await tb.wait_state(STATE_IDLE)
    await RisingEdge(dut.clk)

    assert body == payload
    assert len(tb.hdr_events) == 1
    dut._log.info("hdr_ready backpressure test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_message_truncation(dut):
    tb = TB(dut)
    await tb.start()

    # header claims count=1, message declares length=50, but only 10 bytes
    # of body actually follow before tlast -- must NOT hang, must NOT emit
    # a body frame, and must flag error_truncated.
    short_body = bytes(range(10))
    pkt_bytes = bytearray(build_header(seq_num=1, count=1))
    pkt_bytes += (50).to_bytes(2, "big")  # declared length: 50
    pkt_bytes += short_body               # actual bytes: only 10

    await tb.send(bytes(pkt_bytes))
    await tb.wait_left_idle()
    await tb.wait_state(STATE_IDLE, timeout_cycles=300)
    await RisingEdge(dut.clk)

    assert int(dut.error_truncated.value) == 1
    assert tb.body_sink.empty(), "no body frame should be emitted for a truncated message"
    dut._log.info("message truncation test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_header_truncation(dut):
    tb = TB(dut)
    await tb.start()

    # tlast fires after only 12 bytes -- packet ends mid-header (seq_num
    # field never completes). Must not hang forever.
    short_header = build_header(seq_num=1, count=1)[:12]
    await tb.send(short_header)
    await tb.wait_left_idle()
    await tb.wait_state(STATE_IDLE, timeout_cycles=300)
    await RisingEdge(dut.clk)

    assert int(dut.error_truncated.value) == 1
    dut._log.info("header truncation test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_back_to_back_with_messages(dut):
    tb = TB(dut)
    await tb.start()

    pkt_a = build_header(seq_num=0, count=1) + build_message_block(b"FIRST")
    pkt_b = build_header(seq_num=1, count=1) + build_message_block(b"SECOND")

    await tb.source.send(pkt_a)
    await tb.source.send(pkt_b)

    body_a = await tb.recv_body()
    body_b = await tb.recv_body()

    await tb.wait_state(STATE_IDLE)
    await RisingEdge(dut.clk)

    assert body_a == b"FIRST"
    assert body_b == b"SECOND"
    assert [e["seq_num"] for e in tb.hdr_events] == [0, 1]
    assert int(dut.expected_seq.value) == 2
    dut._log.info("back-to-back with messages test passed")
