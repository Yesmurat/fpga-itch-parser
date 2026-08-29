# FPGA Market Data Feed Parser (NASDAQ ITCH 5.0)

An FPGA receive pipeline that parses NASDAQ ITCH 5.0 market data: Ethernet →
IPv4 → UDP → MoldUDP64 → ITCH, plus a second path that reads raw historical
ITCH sample files directly. Verilog RTL, verified with cocotb against an
independent C++ reference decoder.

```
Live/transport path -- rtl/feed_parser_top.v:

  Ethernet frame -> [eth_axis_rx] -> [ip_eth_rx_64] -> [udp_ip_rx_64] -> udp_rx_top.v
                     (vendored)       (vendored)        (vendored)
                                                                            |
                                                                            v
                                                        [moldudp64_deframer] -> per-message stream -\
                                                         (de-block, re-align,                         \
                                                          sequence, gap-detect)                         \
                                                                                                           v
Raw historical ITCH file -- rtl/itch_raw_pipeline_top.v:                                          [itch_decoder]
                                                                                                     (9 message types,
  [2B length][ITCH message] blocks -> [itch_raw_deframer] -> per-message stream ------------------->  table-driven
  (no MoldUDP64 wrapper)                                                                                field extraction)
                                                                                                           |
                                                                                                           v
                                                                                                 decoded ITCH messages
                                                                                          (cross-checked in test against
                                                                                           cpp/itch_model_cli, the C++
                                                                                           reference decoder)
```

Both frontends present the identical `m_msg_payload_axis_*`/`m_msg_hdr_*` shape to `itch_decoder.v`, which can't tell them apart -- one decoder serves both paths.

## Status

| Layer | RTL | Tests |
|---|---|---|
| Ethernet/IPv4/UDP RX (`rtl/vendor/udp_rx_top.v`) | done | `sim/test_udp_rx_top.py` -- 5 passing |
| MoldUDP64 deframer (`rtl/moldudp64_deframer.v`) | done | `sim/test_moldudp64.py` -- 15 passing |
| Shared byte-realignment buffer (`rtl/gearbox16.v`) | done | exercised via the ITCH decoder suites below |
| ITCH decoder -- 9 message types, table-driven (`rtl/itch_decoder.v`) | done | `sim/test_itch.py` -- 14 passing |
| Raw historical-file frontend (`rtl/itch_raw_deframer.v`) | done | `sim/test_itch_pipeline.py` -- 3 passing |
| Live-path top-level integration (`rtl/feed_parser_top.v`) | done | `sim/test_feed_parser.py` -- 2 passing |
| C++ reference decoder (`cpp/itch_model_cli`) | done | golden-model cross-check used throughout `sim/test_itch.py` |
| Wire-to-decode latency instrumentation (`sim/bench_latency.py`) | done | not a pass/fail test -- reports percentiles + a histogram, see below |
| Order book engine | not started | -- |
| Decode-to-book-update latency | not started | needs the order book engine above |

The 9 in-scope ITCH 5.0 message types: System Event, Stock Directory, Add Order, Add Order with MPID, Order Executed, Order Executed With Price, Order Cancel, Order Delete, Order Replace -- the minimum set needed to build a real order book. All other ITCH 5.0 message types are explicitly out of scope, along with: gap/duplicate *recovery* (retransmission requests -- gap and duplicate *detection* is implemented), A/B feed arbitration, and hardware deployment. Known well enough to discuss, deliberately not built.

The three modules under `rtl/vendor/` (`eth_axis_rx.v`, `ip_eth_rx_64.v`, `udp_ip_rx_64.v`) are vendored, unmodified, from Alex Forencich's [verilog-ethernet](https://github.com/alexforencich/verilog-ethernet). Everything else is original.

## Setup

```bash
mamba env create -f environment.yml
conda activate fpga-itch-parser

# build the C++ reference decoder (once, or after editing cpp/*)
cmake -S cpp -B cpp/build
cmake --build cpp/build
```

## Running tests

```bash
python3 sim/test_runner.py                          # all targets
python3 sim/test_runner.py --target itch_decoder     # one target; see TARGETS in sim/test_runner.py for the full list
```

Simulator: Icarus Verilog via [cocotb](https://www.cocotb.org/) 2.0, packet construction via [Scapy](https://scapy.net/), AXI-Stream driving via [cocotbext-axi](https://github.com/alexforencich/cocotbext-axi). `sim/golden/itch_model.py` shells out to the built `cpp/itch_model_cli` binary as the ITCH decoder's self-checking oracle -- run the `cmake --build` step above before `sim/test_itch.py`.

## Latency

```bash
python3 sim/test_runner.py --target bench_latency
```

Drives 5000 realistic-mix ITCH messages (weighted toward real traffic composition -- mostly Add Order, rare Stock Directory/System Event) back-to-back through `feed_parser_top.v` and reports p50/p90/p99/p99.9 + a histogram, both end-to-end (Ethernet frame in → `m_dec_valid`) and per stage (RX+framing vs. decode). This is **simulated RTL cycle latency in Icarus**, not real silicon timing -- there's no synthesis or board in this project, so there's no closed clock frequency to convert cycles into real nanoseconds yet. It also isn't the full latency number the project eventually wants (message byte in → book update out) -- that needs an order book engine, which doesn't exist. This measures exactly what's built so far: wire in to decoded message out.
