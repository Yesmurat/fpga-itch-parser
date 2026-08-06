"""
cocotb testbench for udp_rx_top: Ethernet -> IPv4 -> UDP RX chain sanity test.

Injects full Ethernet frames (built with scapy) at the s_axis_* input and
checks that the UDP header fields and payload bytes come out the other end
byte-exact. Does NOT parse the UDP payload (MoldUDP64/ITCH is out of scope).
"""
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, ClockCycles, with_timeout

from cocotbext.axi import AxiStreamBus, AxiStreamSource, AxiStreamSink

from scapy.all import Ether, IP, UDP, Raw

# Generous bound so a regressed (broken-handshake) DUT fails with a clean
# timeout instead of hanging the simulation forever.
RECV_TIMEOUT_NS = 5000

DST_MAC = "02:00:00:00:00:02"
SRC_MAC = "02:00:00:00:00:01"
SRC_IP = "192.168.1.10"
DST_IP = "192.168.1.20"
SRC_PORT = 5555
FEED_PORT = 12345


def build_udp_frame(payload, sport=SRC_PORT, dport=FEED_PORT, bad_ip_checksum=False):
    """Build Ether/IP/UDP/Raw frame bytes with scapy. Lets scapy compute
    IP/UDP checksums and lengths unless bad_ip_checksum forces a fixed
    (wrong) IP header checksum so it is NOT recomputed."""
    if bad_ip_checksum:
        ip_layer = IP(src=SRC_IP, dst=DST_IP, chksum=0xdead)
    else:
        ip_layer = IP(src=SRC_IP, dst=DST_IP)
    pkt = Ether(dst=DST_MAC, src=SRC_MAC) / ip_layer / UDP(sport=sport, dport=dport) / Raw(load=payload)
    return bytes(pkt)


class UdpRxTB:
    def __init__(self, dut):
        self.dut = dut

        self.clock = Clock(dut.clk, 10, unit="ns")

        self.eth_source = AxiStreamSource(
            AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst, reset_active_level=True)
        self.udp_payload_sink = AxiStreamSink(
            AxiStreamBus.from_prefix(dut, "m_udp_payload_axis"), dut.clk, dut.rst, reset_active_level=True)

        self.hdr_events = []
        self.error_invalid_header_seen = False
        self.error_invalid_checksum_seen = False
        self.error_early_term_seen = False

    async def start(self):
        self.clock.start()
        cocotb.start_soon(self._monitor())
        await self.reset()

    async def reset(self):
        self.dut.rst.value = 1
        self.dut.s_axis_tuser.value = 0
        # m_udp_hdr_ready is the one external header ready the TB owns; the
        # two internal hdr_ready handshakes (eth->ip, ip->udp) are wired
        # stage-to-stage inside udp_rx_top and are not touched here.
        self.dut.m_udp_hdr_ready.value = 1
        await ClockCycles(self.dut.clk, 5)
        self.dut.rst.value = 0
        await ClockCycles(self.dut.clk, 5)

    async def _monitor(self):
        while True:
            await RisingEdge(self.dut.clk)

            if self.dut.m_udp_hdr_valid.value and self.dut.m_udp_hdr_ready.value:
                self.hdr_events.append({
                    "source_port": int(self.dut.m_udp_source_port.value),
                    "dest_port": int(self.dut.m_udp_dest_port.value),
                    "length": int(self.dut.m_udp_length.value),
                    "checksum": int(self.dut.m_udp_checksum.value),
                })

            if self.dut.error_invalid_header.value:
                self.error_invalid_header_seen = True

            if self.dut.error_invalid_checksum.value:
                self.error_invalid_checksum_seen = True

            if (self.dut.error_eth_header_early_termination.value or
                    self.dut.error_ip_header_early_termination.value or
                    self.dut.error_ip_payload_early_termination.value or
                    self.dut.error_udp_header_early_termination.value or
                    self.dut.error_udp_payload_early_termination.value):
                self.error_early_term_seen = True

    async def send_frame(self, frame_bytes):
        # s_axis_tuser is the bad-frame marker; a clean injected frame holds it low.
        await self.eth_source.send(frame_bytes)

    async def recv_payload(self):
        frame = await self.udp_payload_sink.recv(compact=True)
        return bytes(frame)


def make_payload(n, seed):
    return random.Random(seed).randbytes(n)


@cocotb.test()
async def test_udp_rx_positive(dut):
    """Well-formed UDP packet: header fields + byte-exact payload round-trip."""
    tb = UdpRxTB(dut)
    await tb.start()

    payload = make_payload(64, seed=1)
    frame_bytes = build_udp_frame(payload)

    await tb.send_frame(frame_bytes)
    rx_payload = await tb.recv_payload()

    await ClockCycles(dut.clk, 10)

    assert tb.hdr_events, "no UDP header event (m_udp_hdr_valid) was observed"
    hdr = tb.hdr_events[-1]

    assert hdr["dest_port"] == FEED_PORT, \
        f"m_udp_dest_port mismatch: got {hdr['dest_port']}, expected {FEED_PORT}"
    assert hdr["length"] == 8 + len(payload), \
        f"m_udp_length mismatch: got {hdr['length']}, expected {8 + len(payload)}"

    assert not tb.error_invalid_checksum_seen, "error_invalid_checksum asserted on a good packet"
    assert not tb.error_invalid_header_seen, "error_invalid_header asserted on a good packet"
    assert not tb.error_early_term_seen, "an early-termination error asserted on a good packet"

    assert rx_payload == payload, \
        f"payload mismatch: got {len(rx_payload)} bytes, expected {len(payload)} bytes"


@cocotb.test()
async def test_udp_rx_partial_last_beat(dut):
    """Odd payload length so the final AXIS beat has a partial tkeep."""
    tb = UdpRxTB(dut)
    await tb.start()

    payload = make_payload(37, seed=2)
    frame_bytes = build_udp_frame(payload)

    await tb.send_frame(frame_bytes)
    rx_payload = await tb.recv_payload()

    await ClockCycles(dut.clk, 10)

    assert tb.hdr_events, "no UDP header event (m_udp_hdr_valid) was observed"
    hdr = tb.hdr_events[-1]

    assert hdr["dest_port"] == FEED_PORT
    assert hdr["length"] == 8 + len(payload)

    assert not tb.error_invalid_checksum_seen
    assert not tb.error_invalid_header_seen
    assert not tb.error_early_term_seen

    assert rx_payload == payload, \
        f"payload mismatch on partial-beat case: got {len(rx_payload)} bytes, expected {len(payload)} bytes"


@cocotb.test()
async def test_udp_rx_bad_ip_checksum(dut):
    """Negative control: corrupted IP header checksum must trip error_invalid_checksum,
    and the frame must NOT be forwarded as a UDP header/payload.

    Observed behavior (confirmed by probing ip_eth_rx_64_inst internals directly,
    not assumed): on a bad checksum, ip_eth_rx_64's FSM takes state_reg through
    IDLE -> READ_HEADER -> READ_PAYLOAD -> WAIT_LAST (the discard path).
    m_ip_payload_axis_tvalid and m_ip_hdr_valid never assert, so udp_ip_rx_64
    never even sees a header and its own m_udp_hdr_valid never fires. This is a
    full DROP, not a pass-through-with-flag-set: nothing reaches the UDP payload
    boundary for a packet with a bad IP header checksum."""
    tb = UdpRxTB(dut)
    await tb.start()

    payload = make_payload(48, seed=3)
    frame_bytes = build_udp_frame(payload, bad_ip_checksum=True)

    await tb.send_frame(frame_bytes)
    await tb.eth_source.wait()

    # allow the bad frame to propagate through and be dropped
    await ClockCycles(dut.clk, 100)

    assert tb.error_invalid_checksum_seen, "error_invalid_checksum did not fire on a corrupted IP header checksum"
    assert not tb.hdr_events, "a UDP header was emitted despite a bad IP header checksum"
    assert tb.udp_payload_sink.empty(), "UDP payload was forwarded despite a bad IP header checksum"


@cocotb.test()
async def test_udp_rx_back_to_back(dut):
    """Regression guard for the hdr_ready fix: two UDP packets with NO idle gap
    between frames (both queued into the AXIS source before the sim advances,
    so tvalid never drops between frame A's last beat and frame B's first beat),
    sent while the TB withholds m_udp_hdr_ready so packet A's header sits
    unconsumed for a while.

    Plain back-to-back streaming alone does NOT expose a tied-high internal
    hdr_ready for this particular 3-module chain: measured handshake margins
    (cycles between a downstream stage becoming ready and the upstream stage
    actually presenting the next header) were 0-8 cycles and never negative
    across dozens of payload-length combinations and chains up to 10 packets
    long, so a bare two-packet stream passes whether or not m_eth_hdr_ready /
    m_ip_hdr_ready are wired correctly. What DOES expose it is a downstream
    consumer that isn't instantly ready for a header -- exactly what
    m_udp_hdr_ready models -- combined with a second packet already in flight.

    Empirically confirmed (by probing ip_eth_rx_64_inst/udp_ip_rx_64_inst
    .state_reg directly) that with m_ip_hdr_ready tied high instead of wired
    to udp_ip_rx_64's real s_ip_hdr_ready: ip_eth_rx_64 does not correctly
    hold its own m_ip_hdr_valid until udp_ip_rx_64 is truly ready; it "hands
    off" packet B's header and advances into STATE_READ_PAYLOAD believing the
    transfer succeeded, while udp_ip_rx_64 (still waiting on packet A) never
    actually captures it. udp_ip_rx_64 then never leaves STATE_IDLE, its
    s_ip_payload_axis_tready never asserts again, and ip_eth_rx_64 hangs
    forever in STATE_READ_PAYLOAD waiting for a tready that will never come --
    a full pipeline deadlock, not just a dropped header. This test must fail
    (via timeout, since the DUT genuinely deadlocks) if that regresses."""
    tb = UdpRxTB(dut)
    await tb.start()
    dut.m_udp_hdr_ready.value = 0  # withhold: tb.start()/reset() ties it high by default

    payload_a = make_payload(64, seed=10)
    payload_b = make_payload(96, seed=11)
    dport_a = 11111
    dport_b = 22222

    frame_a = build_udp_frame(payload_a, dport=dport_a)
    frame_b = build_udp_frame(payload_b, dport=dport_b)

    # Enqueue both frames before any clock edge is awaited so the source
    # driver transitions straight from frame A's last beat into frame B's
    # first beat with tvalid held high throughout (no idle gap).
    await tb.eth_source.send(frame_a)
    await tb.eth_source.send(frame_b)

    # Let packet A's header arrive and sit unconsumed (m_udp_hdr_valid held),
    # long enough for packet B to fully enter the pipeline too (or, under a
    # broken hdr_ready chain, for the pipeline to deadlock instead).
    await ClockCycles(dut.clk, 300)

    dut.m_udp_hdr_ready.value = 1  # release: both packets must now drain cleanly

    rx_payload_1 = await with_timeout(tb.recv_payload(), RECV_TIMEOUT_NS, "ns")
    rx_payload_2 = await with_timeout(tb.recv_payload(), RECV_TIMEOUT_NS, "ns")

    await ClockCycles(dut.clk, 10)

    assert len(tb.hdr_events) >= 2, \
        f"expected 2 UDP header events, got {len(tb.hdr_events)}: {tb.hdr_events}"
    hdr1, hdr2 = tb.hdr_events[0], tb.hdr_events[1]

    assert hdr1["dest_port"] == dport_a, f"packet 1 dest_port: got {hdr1['dest_port']}, expected {dport_a}"
    assert hdr1["length"] == 8 + len(payload_a)
    assert hdr2["dest_port"] == dport_b, f"packet 2 dest_port: got {hdr2['dest_port']}, expected {dport_b}"
    assert hdr2["length"] == 8 + len(payload_b)

    assert rx_payload_1 == payload_a, "packet 1 payload mismatch in back-to-back run"
    assert rx_payload_2 == payload_b, "packet 2 payload mismatch in back-to-back run"

    assert not tb.error_invalid_checksum_seen
    assert not tb.error_invalid_header_seen
    assert not tb.error_early_term_seen


@cocotb.test()
async def test_udp_rx_mid_stream_backpressure(dut):
    """Deassert m_udp_payload_axis_tready for several cycles in the middle of a
    single packet's payload, then resume. Asserts the reassembled payload is
    still byte-exact, which exercises payload-tready propagation all the way
    up the chain (udp_ip_rx_64 -> ip_eth_rx_64 -> eth_axis_rx -> s_axis_tready).
    If m_eth_payload_axis_tready/m_ip_payload_axis_tready were tied high instead
    of chained, an upstream stage would advance its own FSM/word-count as if a
    transfer succeeded while the real downstream tready was low, corrupting or
    dropping bytes under this exact scenario."""
    tb = UdpRxTB(dut)
    await tb.start()

    # Long enough that the pause window (triggered off real activity, not a
    # blind cycle count) lands strictly inside the payload transfer.
    payload = make_payload(256, seed=20)
    frame_bytes = build_udp_frame(payload)

    async def backpressure_controller():
        # Wait for the first real payload beat to be accepted...
        while not (dut.m_udp_payload_axis_tvalid.value and dut.m_udp_payload_axis_tready.value):
            await RisingEdge(dut.clk)
        # ...let a few more beats flow, then apply backpressure mid-stream.
        await ClockCycles(dut.clk, 3)
        tb.udp_payload_sink.pause = True
        await ClockCycles(dut.clk, 8)
        tb.udp_payload_sink.pause = False

    cocotb.start_soon(backpressure_controller())

    await tb.send_frame(frame_bytes)
    rx_payload = await with_timeout(tb.recv_payload(), RECV_TIMEOUT_NS, "ns")

    await ClockCycles(dut.clk, 10)

    assert tb.hdr_events, "no UDP header event observed under mid-stream backpressure"
    hdr = tb.hdr_events[-1]
    assert hdr["dest_port"] == FEED_PORT
    assert hdr["length"] == 8 + len(payload)

    assert not tb.error_invalid_checksum_seen
    assert not tb.error_invalid_header_seen
    assert not tb.error_early_term_seen

    assert rx_payload == payload, \
        f"payload corrupted under mid-stream backpressure: got {len(rx_payload)} bytes, expected {len(payload)} bytes"
