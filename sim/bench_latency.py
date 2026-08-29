"""
Wire-to-decode latency measurement for feed_parser_top.v.

Not a correctness test -- this cocotb test always passes; its job is to
drive a realistic-ish, back-to-back message stream through the full live
path (Ethernet -> IPv4 -> UDP -> MoldUDP64 -> ITCH decode) and report
latency percentiles + a histogram, both end-to-end and per stage
(RX+framing vs. decode).

Numbers are simulated RTL clock cycles / simulated ns in Icarus -- this
project has no synthesis or board, so there's no closed clock frequency to
convert these into real wall-clock silicon timing yet. The "book-update-
valid" endpoint from the original latency-instrumentation recommendation
doesn't exist either -- there's no order book engine (a later phase, not
built) -- so this measures exactly as much of the pipeline as exists
today: wire in to decoded ITCH message out.

Message mix is weighted toward real ITCH traffic composition (Add Order
dominates; Stock Directory/System Event are rare, sent a handful of times
per session, not per burst) since no real NASDAQ sample file is available
in this repo to draw from.
"""
import random
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, ClockCycles
from cocotb.utils import get_sim_time

from cocotbext.axi import AxiStreamBus, AxiStreamSource

from scapy.all import Ether, IP, UDP, Raw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_moldudp64 import build_header, build_message_block
from test_itch import (
    build_system_event, build_stock_directory, build_add_order, build_add_order_mpid,
    build_order_executed, build_order_executed_price, build_order_cancel,
    build_order_delete, build_order_replace,
)

N_MESSAGES = 5000
SEED = 1234
CLK_PERIOD_NS = 10

DST_MAC = "02:00:00:00:00:02"
SRC_MAC = "02:00:00:00:00:01"
SRC_IP = "192.168.1.10"
DST_IP = "192.168.1.20"
SRC_PORT = 5555
FEED_PORT = 12345

STOCKS = [b"AAPL    ", b"MSFT    ", b"GOOG    ", b"AMZN    ", b"TSLA    ", b"NVDA    ", b"META    ", b"NFLX    "]

# Rough real-world ITCH traffic composition: Add Order dominates, then
# Cancel/Delete/Executed; MPID/Replace/ExecutedWithPrice rarer; Stock
# Directory/System Event genuinely rare (a handful of times per session).
TYPE_WEIGHTS = [
    ("A", 0.40), ("X", 0.15), ("D", 0.15), ("E", 0.12),
    ("F", 0.06), ("C", 0.05), ("U", 0.05), ("R", 0.01), ("S", 0.01),
]


def build_eth_frame(udp_payload):
    pkt = (Ether(dst=DST_MAC, src=SRC_MAC)
           / IP(src=SRC_IP, dst=DST_IP)
           / UDP(sport=SRC_PORT, dport=FEED_PORT)
           / Raw(load=udp_payload))
    return bytes(pkt)


def weighted_type(rng):
    r = rng.random()
    acc = 0.0
    for t, w in TYPE_WEIGHTS:
        acc += w
        if r < acc:
            return t
    return TYPE_WEIGHTS[-1][0]


def random_itch_body(rng, msg_type):
    locate = rng.randrange(1, 9)
    tracking = rng.randrange(1, 1000)
    timestamp = rng.randrange(0, 1_000_000_000)
    stock = rng.choice(STOCKS)
    order_ref = rng.randrange(1, 1_000_000)
    shares = rng.randrange(1, 10000)
    price = rng.randrange(100, 5_000_000)
    buy_sell = rng.choice("BS")

    if msg_type == "S":
        return build_system_event(locate, tracking, timestamp, rng.choice(["O", "S", "Q", "M", "E", "C"]))
    if msg_type == "R":
        return build_stock_directory(
            locate, tracking, timestamp, stock, "Q", "N", 100, "N", "C", b"CS",
            "P", " ", " ", "1", "N", 1, "N",
        )
    if msg_type == "A":
        return build_add_order(locate, tracking, timestamp, order_ref, buy_sell, shares, stock, price)
    if msg_type == "F":
        return build_add_order_mpid(locate, tracking, timestamp, order_ref, buy_sell, shares, stock, price, b"NSDQ")
    if msg_type == "E":
        return build_order_executed(locate, tracking, timestamp, order_ref, shares, rng.randrange(1, 1_000_000))
    if msg_type == "C":
        return build_order_executed_price(
            locate, tracking, timestamp, order_ref, shares, rng.randrange(1, 1_000_000), "Y", price)
    if msg_type == "X":
        return build_order_cancel(locate, tracking, timestamp, order_ref, shares)
    if msg_type == "D":
        return build_order_delete(locate, tracking, timestamp, order_ref)
    if msg_type == "U":
        return build_order_replace(locate, tracking, timestamp, order_ref, order_ref + 1, shares, price)
    raise ValueError(msg_type)


def build_packet(rng, seq_num):
    body = random_itch_body(rng, weighted_type(rng))
    mold_payload = build_header(seq_num=seq_num, count=1) + build_message_block(body)
    return build_eth_frame(mold_payload)


class LatencyTB:
    def __init__(self, dut):
        self.dut = dut
        self.clock = Clock(dut.clk, CLK_PERIOD_NS, unit="ns")
        self.source = AxiStreamSource(
            AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst, reset_active_level=True)
        self.t_in = []
        self.t_rx_done = []
        self.t_decode_done = []
        self._frame_active = False

    async def start(self):
        self.clock.start()
        self.dut.rst.value = 1
        self.dut.s_axis_tuser.value = 0
        self.dut.m_dec_ready.value = 1
        await ClockCycles(self.dut.clk, 5)
        self.dut.rst.value = 0
        cocotb.start_soon(self._monitor_frame_in())
        cocotb.start_soon(self._monitor_rx_done())
        cocotb.start_soon(self._monitor_decode_done())
        await RisingEdge(self.dut.clk)

    async def _monitor_frame_in(self):
        while True:
            await RisingEdge(self.dut.clk)
            accepted = int(self.dut.s_axis_tvalid.value) and int(self.dut.s_axis_tready.value)
            if accepted and not self._frame_active:
                self.t_in.append(get_sim_time(unit="ns"))
                self._frame_active = True
            if accepted and int(self.dut.s_axis_tlast.value):
                self._frame_active = False

    async def _monitor_rx_done(self):
        while True:
            await RisingEdge(self.dut.clk)
            if int(self.dut.mold.m_msg_hdr_valid.value) and int(self.dut.mold.m_msg_hdr_ready.value):
                self.t_rx_done.append(get_sim_time(unit="ns"))

    async def _monitor_decode_done(self):
        while True:
            await RisingEdge(self.dut.clk)
            if int(self.dut.m_dec_valid.value) and int(self.dut.m_dec_ready.value):
                self.t_decode_done.append(get_sim_time(unit="ns"))


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    k = (len(sorted_vals) - 1) * p
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return sorted_vals[f]
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def ascii_histogram(sorted_vals, bins=20, width=40):
    lo, hi = sorted_vals[0], sorted_vals[-1]
    if lo == hi:
        hi = lo + 1
    bin_w = (hi - lo) / bins
    counts = [0] * bins
    for v in sorted_vals:
        idx = min(int((v - lo) / bin_w), bins - 1)
        counts[idx] += 1
    peak = max(counts) or 1
    lines = []
    for i, c in enumerate(counts):
        lo_edge = lo + i * bin_w
        bar = "#" * int(width * c / peak)
        lines.append(f"  {lo_edge:9.1f} ns  {bar} {c}")
    return "\n".join(lines)


def report(name, values_ns):
    sv = sorted(values_ns)
    print(f"\n--- {name} (n={len(sv)}) ---")
    print(f"  min={sv[0]:.1f}ns ({sv[0]/CLK_PERIOD_NS:.1f} cyc)  "
          f"p50={percentile(sv, 0.50):.1f}ns  p90={percentile(sv, 0.90):.1f}ns  "
          f"p99={percentile(sv, 0.99):.1f}ns  p99.9={percentile(sv, 0.999):.1f}ns  "
          f"max={sv[-1]:.1f}ns ({sv[-1]/CLK_PERIOD_NS:.1f} cyc)")
    print(ascii_histogram(sv))


@cocotb.test(timeout_time=5, timeout_unit="ms")
async def bench_wire_to_decode(dut):
    tb = LatencyTB(dut)
    await tb.start()

    rng = random.Random(SEED)
    for i in range(N_MESSAGES):
        frame = build_packet(rng, seq_num=i + 1)
        await tb.source.send(frame)

    while len(tb.t_decode_done) < N_MESSAGES:
        await RisingEdge(dut.clk)

    assert len(tb.t_in) == N_MESSAGES, f"expected {N_MESSAGES} frame-in events, saw {len(tb.t_in)}"
    assert len(tb.t_rx_done) == N_MESSAGES, f"expected {N_MESSAGES} rx-done events, saw {len(tb.t_rx_done)}"

    end_to_end = [d - i for i, d in zip(tb.t_in, tb.t_decode_done)]
    rx_framing = [r - i for i, r in zip(tb.t_in, tb.t_rx_done)]
    decode_stage = [d - r for r, d in zip(tb.t_rx_done, tb.t_decode_done)]

    print(f"\n=== wire-to-decode latency, N={N_MESSAGES}, clk={CLK_PERIOD_NS}ns "
          f"(simulated RTL cycles in Icarus -- not real silicon timing, no synthesis/board exists) ===")
    report("end-to-end (Ethernet frame in -> m_dec_valid)", end_to_end)
    report("RX + framing (Ethernet in -> moldudp64 hdr accept)", rx_framing)
    report("decode (moldudp64 hdr accept -> m_dec_valid)", decode_stage)
