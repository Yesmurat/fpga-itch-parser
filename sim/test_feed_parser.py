"""
cocotb testbench for feed_parser_top: end-to-end smoke test for the live
transport path -- Ethernet -> IPv4 -> UDP -> MoldUDP64 -> ITCH decode, all
chained together in one module. Each stage already has its own dedicated
regression suite (test_udp_rx_top.py, test_moldudp64.py, test_itch.py);
this just proves feed_parser_top.v's wiring actually carries one real
message correctly all the way through, not a full re-test of every stage.
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, ClockCycles, with_timeout

from cocotbext.axi import AxiStreamBus, AxiStreamSource

from scapy.all import Ether, IP, UDP, Raw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_moldudp64 import build_header, build_message_block
from test_itch import build_add_order, build_order_delete, dec_int_field, dec_ascii_field

RECV_TIMEOUT_NS = 5000

DST_MAC = "02:00:00:00:00:02"
SRC_MAC = "02:00:00:00:00:01"
SRC_IP = "192.168.1.10"
DST_IP = "192.168.1.20"
SRC_PORT = 5555
FEED_PORT = 12345


def build_eth_frame(udp_payload):
    pkt = (Ether(dst=DST_MAC, src=SRC_MAC)
           / IP(src=SRC_IP, dst=DST_IP)
           / UDP(sport=SRC_PORT, dport=FEED_PORT)
           / Raw(load=udp_payload))
    return bytes(pkt)


class TB:
    def __init__(self, dut):
        self.dut = dut
        self.clock = Clock(dut.clk, 10, unit="ns")
        self.source = AxiStreamSource(
            AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst, reset_active_level=True)
        self.dec_events = []

    async def start(self):
        self.clock.start()
        self.dut.rst.value = 1
        self.dut.s_axis_tuser.value = 0
        self.dut.m_dec_ready.value = 1
        await ClockCycles(self.dut.clk, 5)
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

    async def recv_decoded(self):
        async def _wait():
            while not self.dec_events:
                await RisingEdge(self.dut.clk)
            return self.dec_events.pop(0)
        return await with_timeout(_wait(), RECV_TIMEOUT_NS, "ns")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_end_to_end_single_message(dut):
    tb = TB(dut)
    await tb.start()

    itch_body = build_add_order(
        stock_locate=1, tracking_number=1, timestamp=42,
        order_ref=555, buy_sell="B", shares=25, stock=b"GOOG    ", price=2750000,
    )
    mold_payload = build_header(seq_num=0, count=1) + build_message_block(itch_body)
    eth_frame = build_eth_frame(mold_payload)

    await tb.source.send(eth_frame)
    ev = await tb.recv_decoded()

    assert ev["seq_num"] == 0
    assert ev["msg_type"] == "A"
    assert ev["field_count"] == 5
    assert dec_int_field(ev, 0) == 555
    assert dec_ascii_field(ev, 1, 1) == b"B"
    assert dec_int_field(ev, 2) == 25
    assert dec_ascii_field(ev, 3, 8) == b"GOOG    "
    assert dec_int_field(ev, 4) == 2750000
    dut._log.info("end-to-end single message test passed")


@cocotb.test(timeout_time=10000, timeout_unit="ns")
async def test_end_to_end_multiple_messages(dut):
    tb = TB(dut)
    await tb.start()

    body_a = build_order_delete(stock_locate=1, tracking_number=1, timestamp=1, order_ref=42)
    body_b = build_add_order(
        stock_locate=2, tracking_number=2, timestamp=2,
        order_ref=99, buy_sell="S", shares=10, stock=b"MSFT    ", price=3000000,
    )
    mold_payload = (build_header(seq_num=10, count=2)
                     + build_message_block(body_a)
                     + build_message_block(body_b))
    eth_frame = build_eth_frame(mold_payload)

    await tb.source.send(eth_frame)
    ev_a = await tb.recv_decoded()
    ev_b = await tb.recv_decoded()

    assert ev_a["seq_num"] == 10 and ev_a["msg_type"] == "D"
    assert dec_int_field(ev_a, 0) == 42

    assert ev_b["seq_num"] == 11 and ev_b["msg_type"] == "A"
    assert dec_int_field(ev_b, 0) == 99
    assert dec_ascii_field(ev_b, 3, 8) == b"MSFT    "

    dut._log.info("end-to-end multiple messages test passed")
