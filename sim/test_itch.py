"""
cocotb testbench for itch_decoder: table-driven field extraction for the 9
in-scope ITCH 5.0 message types, sideband descriptor handshake, downstream
backpressure, and error signaling (unknown type / length mismatch /
truncated body).

itch_decoder's input isn't produced by another RTL module in this
testbench -- the TB itself plays the role either frontend
(moldudp64_deframer.v or itch_raw_deframer.v) would: drive the sideband
s_msg_hdr_* descriptor (hold-until-accepted), then stream the message body
(the FULL ITCH message, type byte included -- same "body[0] is the type
byte" convention moldudp64_deframer.v uses) via a normal AXI stream source.

Field layouts used by the per-type builders below are the same ones
verified against NASDAQ's ITCH 5.0 spec and transcribed into both
rtl/itch_decoder.v and cpp/itch_model.hpp -- see those files' doc comments.
Several tests additionally cross-check the DUT's common-header fields,
field_count, and error flags against cpp/itch_model_cli (via
golden/itch_model.py), the independent C++ reference implementation.
"""
import struct
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, ClockCycles, with_timeout

from cocotbext.axi import AxiStreamBus, AxiStreamSource

sys.path.insert(0, str(Path(__file__).resolve().parent))
from golden import itch_model

(IDLE, HDR_A, HDR_A_SETTLE, HDR_B, HDR_B_SETTLE,
 FIELD_PULL, FIELD_SETTLE, DRAIN, DONE) = range(9)

RECV_TIMEOUT_NS = 5000


# ---------------------------------------------------------------------------
# Per-type message builders (full ITCH message bytes, type byte included --
# same layout cpp/itch_model.hpp and rtl/itch_decoder.v's tables use).
# ---------------------------------------------------------------------------

def build_common_header(msg_type, stock_locate, tracking_number, timestamp):
    return (msg_type.encode("ascii")
            + stock_locate.to_bytes(2, "big")
            + tracking_number.to_bytes(2, "big")
            + timestamp.to_bytes(6, "big"))


def build_system_event(stock_locate, tracking_number, timestamp, event_code):
    return build_common_header("S", stock_locate, tracking_number, timestamp) + event_code.encode("ascii")


def build_stock_directory(stock_locate, tracking_number, timestamp, stock, market_category, fin_status,
                           round_lot_size, round_lots_only, issue_class, issue_subtype, authenticity,
                           short_sale_thresh, ipo_flag, luld_tier, etp_flag, etp_leverage, inverse):
    assert len(stock) == 8 and len(issue_subtype) == 2
    return (build_common_header("R", stock_locate, tracking_number, timestamp)
            + stock
            + market_category.encode("ascii")
            + fin_status.encode("ascii")
            + round_lot_size.to_bytes(4, "big")
            + round_lots_only.encode("ascii")
            + issue_class.encode("ascii")
            + issue_subtype
            + authenticity.encode("ascii")
            + short_sale_thresh.encode("ascii")
            + ipo_flag.encode("ascii")
            + luld_tier.encode("ascii")
            + etp_flag.encode("ascii")
            + etp_leverage.to_bytes(4, "big")
            + inverse.encode("ascii"))


def build_add_order(stock_locate, tracking_number, timestamp, order_ref, buy_sell, shares, stock, price):
    assert len(stock) == 8
    return (build_common_header("A", stock_locate, tracking_number, timestamp)
            + order_ref.to_bytes(8, "big")
            + buy_sell.encode("ascii")
            + shares.to_bytes(4, "big")
            + stock
            + price.to_bytes(4, "big"))


def build_add_order_mpid(stock_locate, tracking_number, timestamp, order_ref, buy_sell, shares, stock, price, mpid):
    assert len(stock) == 8 and len(mpid) == 4
    return (build_common_header("F", stock_locate, tracking_number, timestamp)
            + order_ref.to_bytes(8, "big")
            + buy_sell.encode("ascii")
            + shares.to_bytes(4, "big")
            + stock
            + price.to_bytes(4, "big")
            + mpid)


def build_order_executed(stock_locate, tracking_number, timestamp, order_ref, executed_shares, match_number):
    return (build_common_header("E", stock_locate, tracking_number, timestamp)
            + order_ref.to_bytes(8, "big")
            + executed_shares.to_bytes(4, "big")
            + match_number.to_bytes(8, "big"))


def build_order_executed_price(stock_locate, tracking_number, timestamp, order_ref, executed_shares,
                                match_number, printable, execution_price):
    return (build_common_header("C", stock_locate, tracking_number, timestamp)
            + order_ref.to_bytes(8, "big")
            + executed_shares.to_bytes(4, "big")
            + match_number.to_bytes(8, "big")
            + printable.encode("ascii")
            + execution_price.to_bytes(4, "big"))


def build_order_cancel(stock_locate, tracking_number, timestamp, order_ref, cancelled_shares):
    return (build_common_header("X", stock_locate, tracking_number, timestamp)
            + order_ref.to_bytes(8, "big")
            + cancelled_shares.to_bytes(4, "big"))


def build_order_delete(stock_locate, tracking_number, timestamp, order_ref):
    return build_common_header("D", stock_locate, tracking_number, timestamp) + order_ref.to_bytes(8, "big")


def build_order_replace(stock_locate, tracking_number, timestamp, orig_order_ref, new_order_ref, shares, price):
    return (build_common_header("U", stock_locate, tracking_number, timestamp)
            + orig_order_ref.to_bytes(8, "big")
            + new_order_ref.to_bytes(8, "big")
            + shares.to_bytes(4, "big")
            + price.to_bytes(4, "big"))


class TB:
    def __init__(self, dut):
        self.dut = dut
        self.clock = Clock(dut.clk, 10, unit="ns")
        self.source = AxiStreamSource(
            AxiStreamBus.from_prefix(dut, "s_msg_payload_axis"), dut.clk, dut.rst, reset_active_level=True)
        self.dec_events = []

    async def start(self):
        self.clock.start()
        self.dut.rst.value = 1
        self.dut.s_msg_hdr_valid.value = 0
        self.dut.s_msg_seq_num.value = 0
        self.dut.s_msg_length.value = 0
        self.dut.s_msg_type.value = 0
        self.dut.m_dec_ready.value = 1
        await ClockCycles(self.dut.clk, 3)
        self.dut.rst.value = 0
        cocotb.start_soon(self._monitor_dec())
        await RisingEdge(self.dut.clk)

    async def _monitor_dec(self):
        while True:
            await RisingEdge(self.dut.clk)
            if int(self.dut.m_dec_valid.value) and int(self.dut.m_dec_ready.value):
                self.dec_events.append(self._snapshot())

    def _snapshot(self):
        return {
            "seq_num": int(self.dut.m_dec_seq_num.value),
            "msg_type": chr(int(self.dut.m_dec_msg_type.value)) if int(self.dut.m_dec_msg_type.value) else "",
            "stock_locate": int(self.dut.m_dec_stock_locate.value),
            "tracking_number": int(self.dut.m_dec_tracking_number.value),
            "timestamp": int(self.dut.m_dec_timestamp.value),
            "field_count": int(self.dut.m_dec_field_count.value),
            "error_unknown_type": bool(int(self.dut.m_dec_error_unknown_type.value)),
            "error_length_mismatch": bool(int(self.dut.m_dec_error_length_mismatch.value)),
            "error_truncated": bool(int(self.dut.m_dec_error_truncated.value)),
            "raw_field_data": int(self.dut.m_dec_field_data.value),
        }

    async def send_message(self, msg_type, seq_num, body, declared_length=None):
        """Sideband hdr descriptor first (hold-until-accepted), then the
        body stream -- same ordering moldudp64_deframer.v's own
        MSG_HDR -> MSG_BODY uses. declared_length lets a test lie about the
        length (for truncation/mismatch cases) independent of how many
        actual body bytes are sent."""
        self.dut.s_msg_seq_num.value = seq_num
        self.dut.s_msg_length.value = len(body) if declared_length is None else declared_length
        self.dut.s_msg_type.value = ord(msg_type) if msg_type else 0
        self.dut.s_msg_hdr_valid.value = 1
        while True:
            await RisingEdge(self.dut.clk)
            if int(self.dut.s_msg_hdr_ready.value):
                break
        self.dut.s_msg_hdr_valid.value = 0
        if body:
            await self.source.send(body)

    async def wait_state(self, target, timeout_cycles=300):
        for _ in range(timeout_cycles):
            await RisingEdge(self.dut.clk)
            if int(self.dut.state.value) == target:
                return
        raise TimeoutError(f"never reached state {target}, stuck at {int(self.dut.state.value)}")

    async def recv_decoded(self):
        async def _wait():
            while not self.dec_events:
                await RisingEdge(self.dut.clk)
            return self.dec_events.pop(0)
        return await with_timeout(_wait(), RECV_TIMEOUT_NS, "ns")


def dec_int_field(ev, k):
    return (ev["raw_field_data"] >> (k * 64)) & ((1 << 64) - 1)


def dec_ascii_field(ev, k, width):
    slot = (ev["raw_field_data"] >> (k * 64)) & ((1 << 64) - 1)
    return slot.to_bytes(8, "little")[:width]


def assert_matches_golden(ev, full_msg_bytes):
    """Cross-check the common-header fields, field_count, and error flags
    against cpp/itch_model_cli -- the independent C++ implementation."""
    golden = itch_model.decode(struct.pack(">H", len(full_msg_bytes)) + full_msg_bytes)
    assert len(golden) == 1, f"expected exactly one golden-model message, got {len(golden)}"
    g = golden[0]
    assert ev["msg_type"] == g["msg_type"], f"msg_type: dut={ev['msg_type']!r} golden={g['msg_type']!r}"
    assert ev["stock_locate"] == g["stock_locate"]
    assert ev["tracking_number"] == g["tracking_number"]
    assert ev["timestamp"] == g["timestamp"]
    assert ev["field_count"] == g["field_count"]
    assert ev["error_unknown_type"] == g["error_unknown_type"]
    assert ev["error_length_mismatch"] == g["error_length_mismatch"]
    assert ev["error_truncated"] == g["error_truncated"]


# ---------------------------------------------------------------------------
# One test per message type
# ---------------------------------------------------------------------------

@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_system_event(dut):
    tb = TB(dut)
    await tb.start()

    body = build_system_event(stock_locate=1, tracking_number=2, timestamp=12345, event_code="O")
    await tb.send_message("S", seq_num=0, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 1
    assert dec_ascii_field(ev, 0, 1) == b"O"
    assert not any([ev["error_unknown_type"], ev["error_length_mismatch"], ev["error_truncated"]])
    assert_matches_golden(ev, body)
    dut._log.info("system event test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_stock_directory(dut):
    tb = TB(dut)
    await tb.start()

    body = build_stock_directory(
        stock_locate=3, tracking_number=4, timestamp=99999, stock=b"AAPL    ",
        market_category="Q", fin_status="N", round_lot_size=100, round_lots_only="N",
        issue_class="C", issue_subtype=b"CS", authenticity="P", short_sale_thresh=" ",
        ipo_flag=" ", luld_tier="1", etp_flag="N", etp_leverage=1, inverse="N",
    )
    await tb.send_message("R", seq_num=1, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 14
    assert dec_ascii_field(ev, 0, 8) == b"AAPL    "
    assert dec_ascii_field(ev, 1, 1) == b"Q"
    assert dec_ascii_field(ev, 2, 1) == b"N"
    assert dec_int_field(ev, 3) == 100
    assert dec_ascii_field(ev, 6, 2) == b"CS"
    assert dec_int_field(ev, 12) == 1
    assert dec_ascii_field(ev, 13, 1) == b"N"
    assert_matches_golden(ev, body)
    dut._log.info("stock directory test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_add_order(dut):
    tb = TB(dut)
    await tb.start()

    body = build_add_order(
        stock_locate=5, tracking_number=6, timestamp=1000,
        order_ref=123456, buy_sell="B", shares=100, stock=b"AAPL    ", price=1234500,
    )
    await tb.send_message("A", seq_num=7, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 5
    assert dec_int_field(ev, 0) == 123456
    assert dec_ascii_field(ev, 1, 1) == b"B"
    assert dec_int_field(ev, 2) == 100
    assert dec_ascii_field(ev, 3, 8) == b"AAPL    "
    assert dec_int_field(ev, 4) == 1234500
    assert_matches_golden(ev, body)
    dut._log.info("add order test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_add_order_mpid(dut):
    tb = TB(dut)
    await tb.start()

    body = build_add_order_mpid(
        stock_locate=8, tracking_number=9, timestamp=2000,
        order_ref=777, buy_sell="S", shares=50, stock=b"MSFT    ", price=3000000, mpid=b"NSDQ",
    )
    await tb.send_message("F", seq_num=2, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 6
    assert dec_int_field(ev, 0) == 777
    assert dec_ascii_field(ev, 1, 1) == b"S"
    assert dec_int_field(ev, 2) == 50
    assert dec_ascii_field(ev, 3, 8) == b"MSFT    "
    assert dec_int_field(ev, 4) == 3000000
    assert dec_ascii_field(ev, 5, 4) == b"NSDQ"
    assert_matches_golden(ev, body)
    dut._log.info("add order with MPID test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_order_executed(dut):
    tb = TB(dut)
    await tb.start()

    body = build_order_executed(
        stock_locate=1, tracking_number=1, timestamp=3000,
        order_ref=123456, executed_shares=40, match_number=999888777,
    )
    await tb.send_message("E", seq_num=3, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 3
    assert dec_int_field(ev, 0) == 123456
    assert dec_int_field(ev, 1) == 40
    assert dec_int_field(ev, 2) == 999888777
    assert_matches_golden(ev, body)
    dut._log.info("order executed test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_order_executed_price(dut):
    tb = TB(dut)
    await tb.start()

    body = build_order_executed_price(
        stock_locate=1, tracking_number=1, timestamp=4000,
        order_ref=42, executed_shares=10, match_number=5555, printable="Y", execution_price=999900,
    )
    await tb.send_message("C", seq_num=4, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 5
    assert dec_int_field(ev, 0) == 42
    assert dec_int_field(ev, 1) == 10
    assert dec_int_field(ev, 2) == 5555
    assert dec_ascii_field(ev, 3, 1) == b"Y"
    assert dec_int_field(ev, 4) == 999900
    assert_matches_golden(ev, body)
    dut._log.info("order executed with price test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_order_cancel(dut):
    tb = TB(dut)
    await tb.start()

    body = build_order_cancel(stock_locate=1, tracking_number=1, timestamp=5000, order_ref=88, cancelled_shares=20)
    await tb.send_message("X", seq_num=5, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 2
    assert dec_int_field(ev, 0) == 88
    assert dec_int_field(ev, 1) == 20
    assert_matches_golden(ev, body)
    dut._log.info("order cancel test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_order_delete(dut):
    tb = TB(dut)
    await tb.start()

    body = build_order_delete(stock_locate=1, tracking_number=1, timestamp=6000, order_ref=999)
    await tb.send_message("D", seq_num=6, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 1
    assert dec_int_field(ev, 0) == 999
    assert_matches_golden(ev, body)
    dut._log.info("order delete test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_order_replace(dut):
    tb = TB(dut)
    await tb.start()

    body = build_order_replace(
        stock_locate=1, tracking_number=1, timestamp=7000,
        orig_order_ref=111, new_order_ref=222, shares=75, price=505000,
    )
    await tb.send_message("U", seq_num=7, body=body)
    ev = await tb.recv_decoded()

    assert ev["field_count"] == 4
    assert dec_int_field(ev, 0) == 111
    assert dec_int_field(ev, 1) == 222
    assert dec_int_field(ev, 2) == 75
    assert dec_int_field(ev, 3) == 505000
    assert_matches_golden(ev, body)
    dut._log.info("order replace test passed")


# ---------------------------------------------------------------------------
# Handshake, backpressure, back-to-back, and error paths
# ---------------------------------------------------------------------------

@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_m_dec_ready_backpressure(dut):
    tb = TB(dut)
    dut.m_dec_ready.value = 0  # override TB.start()'s default; withhold from the start
    tb.clock.start()
    dut.rst.value = 1
    dut.s_msg_hdr_valid.value = 0
    await ClockCycles(dut.clk, 3)
    dut.rst.value = 0
    await RisingEdge(dut.clk)

    body = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=1)
    await tb.send_message("D", seq_num=0, body=body)
    await tb.wait_state(DONE)

    # let it sit held for a while: m_dec_valid must stay asserted with
    # stable field values, and the FSM must not advance past DONE
    for _ in range(20):
        await RisingEdge(dut.clk)
        assert int(dut.m_dec_valid.value) == 1, "m_dec_valid must stay held while m_dec_ready is low"
        assert int(dut.state.value) == DONE

    dut.m_dec_ready.value = 1
    await RisingEdge(dut.clk)
    await RisingEdge(dut.clk)
    assert int(dut.state.value) == IDLE, "must return to IDLE once accepted"
    dut._log.info("m_dec_ready backpressure test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_back_to_back_different_types(dut):
    tb = TB(dut)
    await tb.start()

    body_a = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=42)
    body_b = build_add_order(
        stock_locate=2, tracking_number=2, timestamp=2,
        order_ref=99, buy_sell="B", shares=10, stock=b"IBM     ", price=1000000,
    )

    await tb.send_message("D", seq_num=0, body=body_a)
    ev_a = await tb.recv_decoded()

    await tb.send_message("A", seq_num=1, body=body_b)
    ev_b = await tb.recv_decoded()

    assert ev_a["msg_type"] == "D" and ev_a["field_count"] == 1
    assert dec_int_field(ev_a, 0) == 42

    assert ev_b["msg_type"] == "A" and ev_b["field_count"] == 5
    assert dec_int_field(ev_b, 0) == 99
    assert dec_ascii_field(ev_b, 3, 8) == b"IBM     "

    dut._log.info("back-to-back different types test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_unknown_type_error(dut):
    tb = TB(dut)
    await tb.start()

    body = build_common_header("Z", stock_locate=1, tracking_number=1, timestamp=1) + b"\x00" * 5
    await tb.send_message("Z", seq_num=0, body=body)
    ev = await tb.recv_decoded()

    assert ev["error_unknown_type"] is True
    assert ev["error_length_mismatch"] is False
    assert ev["error_truncated"] is False
    assert ev["field_count"] == 0

    # must recover cleanly for the next message
    body2 = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=1)
    await tb.send_message("D", seq_num=1, body=body2)
    ev2 = await tb.recv_decoded()
    assert ev2["error_unknown_type"] is False
    assert dec_int_field(ev2, 0) == 1
    dut._log.info("unknown type error test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_length_mismatch_error(dut):
    tb = TB(dut)
    await tb.start()

    # Order Delete's true length is 19; declare 20 and send 20 real bytes so
    # the body stream itself is well-formed (only the declared-vs-table
    # length is wrong).
    body = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=1) + b"\x00"
    await tb.send_message("D", seq_num=0, body=body, declared_length=len(body))
    ev = await tb.recv_decoded()

    assert ev["error_length_mismatch"] is True
    assert ev["error_unknown_type"] is False
    assert ev["error_truncated"] is False
    assert ev["field_count"] == 0

    body2 = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=2)
    await tb.send_message("D", seq_num=1, body=body2)
    ev2 = await tb.recv_decoded()
    assert ev2["error_length_mismatch"] is False
    assert dec_int_field(ev2, 0) == 2
    dut._log.info("length mismatch error test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_body_truncation_error(dut):
    tb = TB(dut)
    await tb.start()

    # declare a real Add Order length (36) but only send 15 actual bytes
    # before tlast -- must not hang, must flag error_truncated.
    short_body = build_common_header("A", stock_locate=1, tracking_number=1, timestamp=1) + bytes(4)
    assert len(short_body) == 15
    await tb.send_message("A", seq_num=0, body=short_body, declared_length=36)
    ev = await tb.recv_decoded()

    assert ev["error_truncated"] is True
    assert ev["error_unknown_type"] is False
    assert ev["error_length_mismatch"] is False
    assert ev["field_count"] == 0

    body2 = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=3)
    await tb.send_message("D", seq_num=1, body=body2)
    ev2 = await tb.recv_decoded()
    assert ev2["error_truncated"] is False
    assert dec_int_field(ev2, 0) == 3
    dut._log.info("body truncation error test passed")
