# NASDAQ ITCH 5.0 Feed Parser and Limit Order Book

Two parallel implementations of a NASDAQ TotalView-ITCH 5.0 market data path:

- **An FPGA receive pipeline:** Ethernet → IPv4 → UDP → MoldUDP64 → ITCH, plus a second frontend that reads raw historical ITCH sample files directly. Verilog RTL, verified with cocotb.
- **A C++ feed handler and order book builder:** a table-driven ITCH decoder feeding a limit order book that reconstructs live book state from the message stream. Also serves as the independent golden model the RTL decoder is checked against.

```
Live path - rtl/feed_parser_top.v
  Ethernet frame -> eth_axis_rx* -> ip_eth_rx_64* -> udp_ip_rx_64* -> moldudp64_deframer -> itch_decoder

Raw path  - rtl/itch_raw_pipeline_top.v
  [2B length][ITCH message] blocks -> itch_raw_deframer ----------------------------------> itch_decoder

  * vendored, unmodified, from verilog-ethernet
```

Both frontends present the identical `m_msg_payload_axis_*`/`m_msg_hdr_*` shape to `itch_decoder.v`, which can't tell them apart - one decoder serves both paths.

```
C++ path - cpp/
  [2B length][ITCH message] blocks
    -> itch_decoder.hpp   decode_all()   stateless: bytes -> DecodedMessage
    -> order_book.hpp     apply()        dispatch on message type
         |
         +-- order_table.hpp   L3: order_ref -> {symbol, side, shares, price}
         +-- price_book.hpp    L2: sorted {price, aggregate_shares}, best first
         |
         v
       BookUpdate, through a std::function callback
```

## Status

**RTL track**

| Layer | RTL | Tests |
|---|---|---|
| Ethernet/IPv4/UDP RX (`rtl/vendor/udp_rx_top.v`) | done | `sim/test_udp_rx_top.py` - 5 passing |
| MoldUDP64 deframer (`rtl/moldudp64_deframer.v`) | done | `sim/test_moldudp64.py` - 15 passing |
| Shared byte-realignment buffer (`rtl/gearbox16.v`) | done | exercised via the ITCH decoder suites below |
| ITCH decoder - 9 message types, table-driven (`rtl/itch_decoder.v`) | done | `sim/test_itch.py` - 14 passing |
| Raw historical-file frontend (`rtl/itch_raw_deframer.v`) | done | `sim/test_itch_pipeline.py` - 3 passing |
| Live-path top-level integration (`rtl/feed_parser_top.v`) | done | `sim/test_feed_parser.py` - 2 passing |
| Wire-to-decode latency instrumentation (`sim/bench_latency.py`) | done | not a pass/fail test - reports percentiles + a histogram, see below |

**C++ track**

| Component | Status | Tests |
|---|---|---|
| ITCH decoder, table-driven (`cpp/itch_decoder.hpp`) | done | golden-model cross-check throughout `sim/test_itch.py` |
| Order table - open-addressing hash (`cpp/book/order_table.hpp`) | done | `test_order_table` - 8 passing |
| Price book - sorted bounded array (`cpp/book/price_book.hpp`) | done | `test_price_book` - 9 passing |
| Order book engine - all 9 message types (`cpp/book/order_book.hpp`) | done | `test_order_book` - 10 passing |
| Book inspection CLI (`cpp/book/order_book_cli.cpp`) | done | - |
| `apply()` latency instrumentation (`cpp/book/order_book_latency.cpp`) | done | not a pass/fail test - reports p50/p90/p99 + min/max/sample count, see below |
| Test-stream generator (`cpp/book/generate_stream.py`) | done | validated by the seven error counters reading zero across 50,000 messages |

The 9 in-scope ITCH 5.0 message types: System Event, Stock Directory, Add Order, Add Order with MPID, Order Executed, Order Executed With Price, Order Cancel, Order Delete, Order Replace - the minimum set needed to build a real order book. All other ITCH 5.0 message types are explicitly out of scope, along with: gap/duplicate *recovery* (retransmission requests - gap and duplicate *detection* is implemented in RTL), A/B feed arbitration, and hardware deployment. Known well enough to discuss, deliberately not built.

The three modules under `rtl/vendor/` (`eth_axis_rx.v`, `ip_eth_rx_64.v`, `udp_ip_rx_64.v`) are vendored, unmodified, from Alex Forencich's [verilog-ethernet](https://github.com/alexforencich/verilog-ethernet). Everything else is original.

## Order book design

ITCH is an order-by-order feed: after an Add, every later message (`E`/`C`/`X`/`D`/`U`) identifies its order by reference number alone and never repeats the price, side, or symbol. Reconstructing a price ladder therefore requires keeping per-order state, which is why the book is two structures rather than one:

- **`OrderTable`** - open-addressing hash, `order_ref -> {symbol_index, is_buy, shares, price}`. This is the L3 (market-by-order) state, and it exists so a message carrying nothing but `order_ref=4471822, shares=50` can be resolved to a specific price level.
- **`PriceBook`** - a sorted bounded array of `{price, aggregate_shares}`, best price first, one instance per side per symbol. This is the L2 (market-by-price) view, derived from the L3 state. It never distinguishes which order contributed which shares.

`OrderBook::apply()` is the single entry point. It resolves NASDAQ's arbitrary `stock_locate` to a dense symbol index, dispatches on message type, updates whichever of the two structures the message affects, and fires a `BookUpdate` through a `std::function` callback when the top of a book changes. Malformed messages - flagged by the decoder as unknown-type, length-mismatched, or truncated - are rejected before dispatch and counted, so corrupt input cannot reach the book.

Seven error counters are tracked and reported by `report_stats()`: `locate_capacity_exceeded`, `order_table_insert_failed`, `unknown_order_ref`, `invalid_reduction`, `invalid_price_decrement`, `price_level_capacity_exceeded`, `malformed_message`.

Fixed capacities, by design: **8 symbols**, **512 price levels per side**, **65,536 orders**.

## Known limitations

- **This is a book builder, not a matching engine.** It reconstructs the exchange's book by replaying the feed; it never crosses orders or generates fills.
- **No price-time priority within a price level.** `PriceBook` collapses each level to `{price, aggregate_shares}`, so FIFO queue position inside a level is not retained. Standard for a book builder; a dealbreaker for a matching engine.
- **`E`, `C`, and `X` reduce the order's share count before confirming the price-book decrement.** If the decrement then fails, `order_table_` and the ladder disagree. The failure is counted, not repaired.
- **`U`'s insert-failure path leaves the book modified without firing a callback.** By the time the new order's insert is attempted, the original order's contribution has already been removed from the ladder.
- **Stock tickers are not retained.** The book keys off `stock_locate` only, so a price ladder cannot be traced back to the company it belongs to.
- No Trade (`P`), cross/auction (`Q`), or halt/LULD/MWCB message handling. Single-threaded.

## Setup

```bash
mamba env create -f environment.yml
conda activate fpga-itch-parser

# build the C++ decoder, order book, and test suites
cmake -S cpp -B cpp/build
cmake --build cpp/build
```

## Running tests

```bash
# RTL (cocotb/Icarus)
python3 sim/test_runner.py                          # all targets
python3 sim/test_runner.py --target itch_decoder    # one target; see TARGETS in sim/test_runner.py for the full list

# C++ order book (27 tests across 3 suites)
ctest --test-dir cpp/build
```

Simulator: Icarus Verilog via [cocotb](https://www.cocotb.org/) 2.0, packet construction via [Scapy](https://scapy.net/), AXI-Stream driving via [cocotbext-axi](https://github.com/alexforencich/cocotbext-axi). `sim/golden/itch_model.py` shells out to the built `cpp/itch_model_cli` binary as the ITCH decoder's self-checking oracle - run the `cmake --build` step above before `sim/test_itch.py`.

## Inspecting a stream

No `.bin` streams are committed - `generate_stream.py` writes them, in either of two sizes:

```bash
cd cpp/book
python3 generate_stream.py sample     # sample_stream.bin - 4 messages, hand-written, for eyeballing
python3 generate_stream.py large      # large_stream.bin  - 50,000 messages, ~1.5 MB

cd ../.. && ./cpp/build/order_book_cli cpp/book/large_stream.bin | tail -8
```

`order_book_cli` prints a `BookUpdate` for every change to the top of a book, then the seven error counters. On a well-formed stream all seven read zero - which is the real check that the input exercised book-building work rather than error paths. `itch_model_cli` is the complementary tool: it prints one line per message with every decoded field and the three decoder error flags, so it localises a framing or field-placement bug to a specific `seq_num` where the counters only give an aggregate.

The large stream is generated from a seeded RNG against a live-order pool - the generator keeps its own `order_ref -> (locate, side, shares, price)` table mirroring `OrderTable`, so that every Cancel, Delete, and Replace it emits names an order that is actually resting on the book. That is what makes the zero-counter check meaningful: a stateless random generator would emit mostly dead references, score `unknown_order_ref` on nearly every message, and measure almost no book-building work. Composition is roughly 40% Add / 25% Cancel / 20% Delete / 15% Replace after a 500-message warm-up of pure Adds, with prices drawn from a narrow band per side so bids never cross asks and levels are revisited rather than created once each.

## Latency

**RTL - wire in to decoded message out**

```bash
python3 sim/test_runner.py --target bench_latency
```

Drives 5000 realistic-mix ITCH messages (weighted toward real traffic composition - mostly Add Order, rare Stock Directory/System Event) back-to-back through `feed_parser_top.v` and reports p50/p90/p99/p99.9 + a histogram, both end-to-end (Ethernet frame in → `m_dec_valid`) and per stage (RX+framing vs. decode). This is **simulated RTL cycle latency in Icarus**, not real silicon timing - there's no synthesis or board in this project, so there's no closed clock frequency to convert cycles into real nanoseconds yet.

**C++ - decoded message in to book update out**

```bash
cd cpp/book && python3 generate_stream.py large
cd ../.. && ./cpp/build/order_book_latency cpp/book/large_stream.bin
```

**What is inside the measurement window:** `OrderBook::apply()`, and nothing else. `decode_all()` runs to completion before any timer starts, so decoding cost is excluded entirely - these are per-book-update numbers, not per-message end-to-end numbers. The clock is `std::chrono::steady_clock`, started immediately before `apply()` and read inside the `BookUpdate` callback, so the interval spans dispatch plus whichever of `OrderTable`/`PriceBook` the message touched.

Across four runs of the 50,000-message stream (49,992 samples each - every non-Stock-Directory message fires a callback):

| | observed |
|---|---|
| p50 | 280 - 370 ns |
| p90 | 430 - 650 ns |
| p99 | 660 - 1510 ns |
| min | 64 - 75 ns |
| max | 33 - 67 µs |

Ranges, not single figures, because that is what the data supports: p50 spread ~30% across runs and p99 moved 663 → 1507 ns, all on byte-identical input. The `max` is two orders of magnitude above p99 and is scheduler preemption or a page fault rather than anything the book did - it is reported for honesty, not as something to optimise. This is a development machine under normal load, not a pinned core on a tuned kernel.

The mixed stream is roughly 2.5x slower than an Adds-only stream, which is the more interesting result: `X`/`D`/`U` each perform an `OrderTable::find` that Adds skip, the table sits at ~15% load factor rather than near-empty so probe chains are longer, and `U` is the heaviest path in `apply()` - find, decrement, erase, insert, increment.
