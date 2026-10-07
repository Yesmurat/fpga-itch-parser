# NASDAQ ITCH 5.0 Feed Parser and Limit Order Book

A C++ feed handler and limit order book for NASDAQ TotalView-ITCH 5.0: a table-driven ITCH decoder feeding an order book that reconstructs live book state by replaying the message stream.

```
  [2B length][ITCH message] blocks
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

| Component | Status | Tests |
|---|---|---|
| ITCH decoder, table-driven (`itch_decoder.hpp`) | done | **none yet** - see Known limitations |
| Order table - open-addressing hash (`book/order_table.hpp`) | done | `test_order_table` - 8 passing |
| Price book - sorted bounded array (`book/price_book.hpp`) | done | `test_price_book` - 9 passing |
| Order book engine - all 9 message types (`book/order_book.hpp`) | done | `test_order_book` - 10 passing |
| Book inspection CLI (`book/order_book_cli.cpp`) | done | - |
| `apply()` latency instrumentation (`book/order_book_latency.cpp`) | done | not a pass/fail test - reports p50/p90/p99 + min/max/sample count, see below |
| Test-stream generator (`book/generate_stream.py`) | done | validated by the seven error counters reading zero across 50,000 messages |

The 9 in-scope ITCH 5.0 message types: System Event, Stock Directory, Add Order, Add Order with MPID, Order Executed, Order Executed With Price, Order Cancel, Order Delete, Order Replace - the minimum set needed to build a real order book. All other ITCH 5.0 message types are explicitly out of scope.

So is the transport layer. The decoder consumes bare `[2B length][ITCH message]` blocks - the framing NASDAQ's historical sample files use - so MoldUDP64 packet handling, sequence-gap detection and retransmission requests, and A/B feed arbitration are all outside this project. Known well enough to discuss, deliberately not built.

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
- **`itch_decoder.hpp` has no test suite of its own.** It is exercised indirectly - every order-book test and both CLIs decode through it - but there are no direct tests covering field placement, big-endian widths, ASCII fields, or the three error flags. This is the top of the to-do list.
- No Trade (`P`), cross/auction (`Q`), or halt/LULD/MWCB message handling. Single-threaded.

## Setup

```bash
mamba env create -f environment.yml
conda activate itch-order-book

# build the decoder, order book, CLIs, and test suites
cmake -B build
cmake --build build
```

Python 3 is used only by `book/generate_stream.py`, which needs no third-party packages - the standard library is enough. The C++ is C++17 with no external dependencies.

## Running tests

```bash
ctest --test-dir build     # 27 tests across 3 suites
```

## Inspecting a stream

No `.bin` streams are committed - `generate_stream.py` writes them, in either of two sizes:

```bash
cd book
python3 generate_stream.py sample     # sample_stream.bin - 4 messages, hand-written, for eyeballing
python3 generate_stream.py large      # large_stream.bin  - 50,000 messages, ~1.5 MB

cd .. && ./build/order_book_cli book/large_stream.bin | tail -8
```

`order_book_cli` prints a `BookUpdate` for every change to the top of a book, then the seven error counters. On a well-formed stream all seven read zero - which is the real check that the input exercised book-building work rather than error paths. `itch_model_cli` is the complementary tool: it prints one line per message with every decoded field and the three decoder error flags, so it localises a framing or field-placement bug to a specific `seq_num` where the counters only give an aggregate.

The large stream is generated from a seeded RNG against a live-order pool - the generator keeps its own `order_ref -> (locate, side, shares, price)` table mirroring `OrderTable`, so that every Cancel, Delete, and Replace it emits names an order that is actually resting on the book. That is what makes the zero-counter check meaningful: a stateless random generator would emit mostly dead references, score `unknown_order_ref` on nearly every message, and measure almost no book-building work. Composition is roughly 40% Add / 25% Cancel / 20% Delete / 15% Replace after a 500-message warm-up of pure Adds, with prices drawn from a narrow band per side so bids never cross asks and levels are revisited rather than created once each.

## Latency

```bash
cd book && python3 generate_stream.py large
cd .. && ./build/order_book_latency book/large_stream.bin
```

**What is inside the measurement window:** `OrderBook::apply()`, and nothing else. `decode_all()` runs to completion before any timer starts, so decoding cost is excluded entirely - these are per-book-update numbers, not per-message end-to-end numbers. The clock is `std::chrono::steady_clock`, started immediately before `apply()` and read inside the `BookUpdate` callback, so the interval spans dispatch plus whichever of `OrderTable`/`PriceBook` the message touched.

Across ten runs of the 50,000-message stream, spread over two sessions (49,992 samples each - every non-Stock-Directory message fires a callback):

| | observed |
|---|---|
| p50 | 280 - 520 ns |
| p90 | 430 - 820 ns |
| p99 | 660 - 1510 ns |
| min | 64 - 150 ns |
| max | 33 - 618 µs |

Wide ranges, and deliberately reported that way. Within one session the spread is modest, but between sessions the whole distribution shifted by roughly 2x on byte-identical input - `min` alone moved from ~70 ns to ~145 ns, which is a systematic change in machine state (CPU frequency scaling, background load), not measurement noise. This is an untuned development machine, not a pinned core on an isolated kernel.

So the absolute figures should be read as an order of magnitude - hundreds of nanoseconds per book update - rather than a benchmark. What the data does support reliably is the *shape*: p90 sits ~1.4x above p50, p99 ~2x above, and `max` two to three orders of magnitude above p99. That tail is scheduler preemption or a page fault, not anything `apply()` did; it is reported for honesty, not as something to optimise.

The mixed stream is roughly 2.5x slower than an Adds-only stream, which is the more interesting result: `X`/`D`/`U` each perform an `OrderTable::find` that Adds skip, the table sits at ~15% load factor rather than near-empty so probe chains are longer, and `U` is the heaviest path in `apply()` - find, decrement, erase, insert, increment.
