# Nasdaq TotalView ITCH 5.0 Feed Parser and Limit Order Book

A C++ feed handler and limit order book: a table-driven ITCH decoder feeding a book that reconstructs live state by replaying the message stream.

```
  [2-byte length][ITCH message] blocks
    -> itch_decoder.hpp        decode_all()   stateless: bytes -> DecodedMessage
    -> book/order_book.hpp     apply()        dispatch on message type
         |
         +-- book/order_table.hpp   L3: order_ref -> {symbol, side, shares, price}
         +-- book/price_book.hpp    L2: sorted {price, aggregate_shares}, best first
         |
         v
       BookUpdate, through a std::function callback
```

## Status

| Component | Tests |
|---|---|
| ITCH decoder, table-driven (`itch_decoder.hpp`) | **none yet** - see Limitations |
| Order table - open-addressing hash (`book/order_table.hpp`) | `book/tests/test_order_table.cpp` - 8 |
| Price book - sorted bounded array (`book/price_book.hpp`) | `book/tests/test_price_book.cpp` - 9 |
| Order book engine, all 9 message types (`book/order_book.hpp`) | `book/tests/test_order_book.cpp` - 10 |
| Book inspection CLI (`book/order_book_cli.cpp`) | - |
| Decoder inspection CLI (`itch_model_cli.cpp`) | - |
| `apply()` latency instrumentation (`book/order_book_latency.cpp`) | - |
| Test-stream generator (`book/generate_stream.py`) | seven error counters read zero across 50,000 messages |

In scope: the 9 ITCH 5.0 message types needed to build a book - System Event, Stock Directory, Add Order, Add Order with MPID, Order Executed, Order Executed With Price, Order Cancel, Order Delete, Order Replace.

Out of scope: all other message types, and the transport layer. The decoder consumes bare `[2-byte length][ITCH message]` blocks, the framing Nasdaq's historical sample files use - so MoldUDP64 packet handling, gap detection, retransmission requests and A/B feed arbitration are not here.

## Design

ITCH is order-by-order: after an Add, every later message (`E`/`C`/`X`/`D`/`U`) identifies its order by reference number alone and never repeats price, side or symbol. Rebuilding a price ladder therefore needs per-order state, so the book is two structures:

- **`OrderTable`** - open-addressing hash, `order_ref -> {symbol_index, is_buy, shares, price}`. The L3 state, so a message carrying only `order_ref=4471822, shares=50` can be resolved to a price level.
- **`PriceBook`** - sorted bounded array of `{price, aggregate_shares}`, best first, one per side per symbol. The L2 view, derived from L3. Never tracks which order contributed which shares.

`OrderBook::apply()` is the single entry point: resolve `stock_locate` to a dense symbol index, dispatch on type, update whichever structure the message affects, fire a `BookUpdate`. Messages the decoder flagged as unknown-type, length-mismatched or truncated are rejected before dispatch and counted, so corrupt input never reaches the book.

The callback fires after **every** successful mutation, not only when the best price moves: an Add deep in the book still fires, reporting the unchanged top. It is a "something changed, here is the current top" notification, not a top-of-book *change* feed.

`report_stats()` returns seven counters: `locate_capacity_exceeded`, `order_table_insert_failed`, `unknown_order_ref`, `invalid_reduction`, `invalid_price_decrement`, `price_level_capacity_exceeded`, `malformed_message`.

Fixed capacities, by design: **8 symbols**, **512 price levels per side**, **65,536 orders**.

## Limitations

- **A book builder, not a matching engine.** It replays the feed; it never crosses orders or generates fills.
- **No price-time priority within a level.** `PriceBook` collapses each level to `{price, aggregate_shares}`, so FIFO queue position is lost. Normal for a book builder, fatal for a matching engine.
- **`E`/`C`/`X` reduce the order's share count before confirming the price-book decrement.** If the decrement then fails, `order_table_` and the ladder disagree. Counted, not repaired.
- **`U`'s insert-failure path changes the book without firing a callback.** The original order's shares are already removed from the ladder by then.
- **Stock tickers are discarded.** The book keys off `stock_locate`, so a ladder cannot be traced back to a company.
- **`itch_decoder.hpp` has no tests of its own.** Exercised indirectly by every book test and both CLIs, but nothing directly covers field placement, big-endian widths, ASCII fields or the three error flags. Top of the to-do list.
- No Trade (`P`), cross/auction (`Q`) or halt/LULD/MWCB handling. Single-threaded.

## Build and test

```bash
mamba env create -f environment.yml && conda activate itch-order-book
cmake -B build && cmake --build build
ctest --test-dir build              # 27 tests across 3 suites
```

C++17, no external dependencies. Python 3 is used only by `book/generate_stream.py`, standard library only.

**Note**: Do not configure with `-DCMAKE_BUILD_TYPE=Release`. Every optimized build type also defines `NDEBUG`, which compiles `assert()` to nothing, and all 171 test assertions are plain `assert()`, so you would get 27 tests passing while checking nothing. `CMakeLists.txt` sets `-O2 -Wall -Wextra` directly via `add_compile_options` to keep both the optimization and the assertions.

## Streams

No `.bin` files are in the repo; `generate_stream.py` writes them.

```bash
cd book
python3 generate_stream.py sample     # sample_stream.bin (4 messages)
python3 generate_stream.py large      # large_stream.bin  (50,000 messages)
cd .. && ./build/order_book_cli book/large_stream.bin | tail -8
```

`order_book_cli` prints a `BookUpdate` per successful mutation, then the seven counters that all should be zero on a well-formed stream, which is the real check that the input exercised book-building rather than error paths.

`itch_model_cli` is the decoder-level counterpart: one line per message with every decoded field and the three error flags, so a framing bug localises to a `seq_num` instead of an aggregate count.

The large stream is driven by a seeded RNG against a live-order pool: the generator mirrors `OrderTable` in its own `order_ref -> (locate, side, shares, price)` dict, so every Cancel, Delete and Replace names an order actually resting on the book. That is what makes the zero-counter check meaningful because a stateless generator would emit mostly dead references and score `unknown_order_ref` on nearly everything.

Prices come from a narrow band per side, so bids never cross asks and levels are revisited rather than created once each. After a 500-message warm-up of pure Adds, branch weights are 40/25/20/15 for Add/Cancel/Delete/Replace, but the realised mix is `A 40.9% / X 20.4% / D 23.9% / U 14.8%`. A partial cancel needs two round lots to leave something behind, so 100-share orders get a Delete instead.

## Latency

To measure latency run the following:
```bash
./build/order_book_latency book/large_stream.bin
```

Times `OrderBook::apply()` and nothing else: `decode_all()` finishes before any timer starts, so these are per-book-update figures, not per-message end-to-end. `steady_clock` is read immediately before `apply()` and again inside the callback.

More than a dozen runs at `-O2`, 49,992 samples each (one per successful mutation, i.e. all but the 8 Stock Directory messages):

|     |                 observed                     |
| --- |               ---                            |
| p50 | 130 - 250 ns (mostly ~160)                   |
| p90 | 170 - 360 ns                                 |
| p99 | 360 - 620 ns                                 |
| min | 20 - 32 ns (measurement floor, not a result) |
| max | 30 - 75 us (scheduler noise)                 |

An empty `now() -> now()` interval costs 22 ns here, so `min` measures the clock rather than the book, and ~14% of the p50 figure is timer overhead. Resolving operations this short properly would mean timing batches, not single calls. Treat the median as ~150-200 ns per book update, with p90 about 2x and p99 about 3x that; `max` is preemption or a page fault, reported for honesty rather than as something to optimise. Untuned development machine, not a pinned core.

Against an Adds-only stream of the same length:

```
mixed      p50 130-250 ns    p90 170-360 ns    p99 360-620 ns
Adds-only  p50  60- 70 ns    p90 180-300 ns    p99 380-540 ns
```

Mixed costs ~2x more at the median (~2.6x once the 22 ns overhead is subtracted from both): `X`/`D`/`U` each do an `OrderTable::find` that Adds skip, the table sits at ~15% load so probes are longer, and `U` is the heaviest path - find, decrement, erase, insert, increment. But the tails are indistinguishable, because every first Add at a new price must insert into the sorted array and shift everything above it. Mixed raises the typical cost; Adds-only raises the worst cost.
