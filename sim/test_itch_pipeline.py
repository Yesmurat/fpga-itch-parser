"""
cocotb testbench for itch_raw_pipeline_top: itch_raw_deframer.v ->
itch_decoder.v wired end to end, exercising the raw-historical-file input
path (a continuous [2-byte length][ITCH message] block stream, no
MoldUDP64 wrapper) rather than the live/transport path
test_itch.py exercises against itch_decoder.v standalone.

Reuses test_itch.py's message builders and field-extraction helpers --
they're plain functions, not cocotb tests, so importing them here doesn't
register test_itch.py's own tests against this module.
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, ClockCycles, with_timeout

from cocotbext.axi import AxiStreamBus, AxiStreamSource

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_itch import build_order_delete, build_add_order, build_system_event, dec_int_field

RECV_TIMEOUT_NS = 5000


def block(body):
    return len(body).to_bytes(2, "big") + body


class TB:
    def __init__(self, dut):
        self.dut = dut
        self.clock = Clock(dut.clk, 10, unit="ns")
        self.source = AxiStreamSource(
            AxiStreamBus.from_prefix(dut, "s_raw_axis"), dut.clk, dut.rst, reset_active_level=True)
        self.dec_events = []

    async def start(self):
        self.clock.start()
        self.dut.rst.value = 1
        self.dut.m_dec_ready.value = 1
        await ClockCycles(self.dut.clk, 3)
        self.dut.rst.value = 0
        cocotb.start_soon(self._monitor_dec())
        await RisingEdge(self.dut.clk)

    async def _monitor_dec(self):
        while True:
            await RisingEdge(self.dut.clk)
            if int(self.dut.m_dec_valid.value) and int(self.dut.m_dec_ready.value):
                self.dec_events.append({
                    "seq_num": int(self.dut.m_dec_seq_num.value),
                    "msg_type": chr(int(self.dut.m_dec_msg_type.value)) if int(self.dut.m_dec_msg_type.value) else "",
                    "field_count": int(self.dut.m_dec_field_count.value),
                    "raw_field_data": int(self.dut.m_dec_field_data.value),
                })

    async def send_file(self, blob):
        await self.source.send(blob)

    async def recv_decoded(self):
        async def _wait():
            while not self.dec_events:
                await RisingEdge(self.dut.clk)
            return self.dec_events.pop(0)
        return await with_timeout(_wait(), RECV_TIMEOUT_NS, "ns")

    async def wait_end_of_input(self, timeout_cycles=300):
        for _ in range(timeout_cycles):
            await RisingEdge(self.dut.clk)
            if int(self.dut.raw_end_of_input.value):
                return
        raise TimeoutError("raw_end_of_input never asserted")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_single_block(dut):
    tb = TB(dut)
    await tb.start()

    body = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=42)
    await tb.send_file(block(body))

    ev = await tb.recv_decoded()
    assert ev["seq_num"] == 0, "first block's synthetic seq_num must start at 0"
    assert ev["msg_type"] == "D"
    assert ev["field_count"] == 1
    assert dec_int_field(ev, 0) == 42

    await tb.wait_end_of_input()
    assert int(dut.raw_error_truncated.value) == 0
    dut._log.info("single block test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_multiple_blocks(dut):
    tb = TB(dut)
    await tb.start()

    bodies = [
        ("D", build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=1)),
        ("A", build_add_order(stock_locate=2, tracking_number=2, timestamp=2, order_ref=2,
                               buy_sell="B", shares=10, stock=b"IBM     ", price=1000000)),
        ("S", build_system_event(stock_locate=3, tracking_number=3, timestamp=3, event_code="O")),
    ]
    blob = b"".join(block(b) for _, b in bodies)
    await tb.send_file(blob)

    events = [await tb.recv_decoded() for _ in bodies]

    assert [e["seq_num"] for e in events] == [0, 1, 2], "synthetic seq_num must be a monotonic block index"
    assert [e["msg_type"] for e in events] == ["D", "A", "S"]
    assert dec_int_field(events[0], 0) == 1
    assert dec_int_field(events[1], 0) == 2

    await tb.wait_end_of_input()
    dut._log.info("multiple blocks test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_truncated_file(dut):
    tb = TB(dut)
    await tb.start()

    # declares a real 19-byte Order Delete body, but the file ends after
    # only 10 of those bytes -- must not hang, must flag raw_error_truncated,
    # and must never hand the decoder a spurious completed message for it.
    # Built by hand (not via block()) since block() derives the length
    # prefix from the actual bytes given -- it can't express a mismatch.
    full_body = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=999)
    assert len(full_body) == 19
    short_body = full_body[:10]
    blob = len(full_body).to_bytes(2, "big") + short_body  # declares 19, only 10 actually follow
    await tb.send_file(blob)

    for _ in range(300):
        await RisingEdge(dut.clk)
        if int(dut.raw_error_truncated.value):
            break
    else:
        raise TimeoutError("raw_error_truncated never asserted")

    assert not tb.dec_events, "a truncated block must never reach the decoder as a completed decoded message"
    dut._log.info("truncated file test passed")
