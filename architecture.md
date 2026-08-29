# RTL Architecture

## Overview

```
Live/transport path -- rtl/feed_parser_top.v:
  udp_rx_top.v (vendored RX chain) -> moldudp64_deframer.v -> itch_decoder.v

Raw historical-file path -- rtl/itch_raw_pipeline_top.v:
  itch_raw_deframer.v -> itch_decoder.v
```

Both frontends (`moldudp64_deframer.v`, `itch_raw_deframer.v`) present the identical `m_msg_payload_axis_*`/`m_msg_hdr_*` port shape to `itch_decoder.v`, which is frontend-agnostic and can't tell them apart. Both frontends, plus `itch_decoder.v` itself, are built on a shared byte-realignment buffer (`gearbox16.v`).

Three hard-won patterns recur throughout this codebase, documented once each below and referenced by name everywhere they applied: **the settle-cycle pattern**, **the capture-at-pull-time pattern**, and **the same-edge producer/consumer race**.

---

## `moldudp64_deframer.v`

### Purpose

Sits between the UDP RX pipeline (`udp_rx_top.m_udp_payload_axis`) and `itch_decoder.v`. Takes one continuous byte stream of back-to-back MoldUDP64 packets and turns it into a sequence of individual ITCH messages, each byte-0-aligned and tagged with a sequence number, with sequence gaps detected along the way.

**Current state:** fully implemented and tested (`sim/test_moldudp64.py`, 15 passing) — header parsing, byte-order reconstruction, gap/duplicate detection, heartbeat/end-of-session handling, per-message body streaming with downstream backpressure, and truncation detection at both the header and message-body level.

### Interface

| Signal | Dir | Description |
|---|---|---|
| `s_udp_payload_axis_*` | in | Raw MoldUDP64 byte stream, 64 bits/cycle, standard AXI-Stream (`tvalid`/`tready`/`tlast`/`tkeep`/`tuser`) |
| `m_msg_payload_axis_*` | out | Per-message body bytes, re-aligned to byte 0 — driven in `READ_MESSAGE`'s `MSG_BODY` step |
| `m_msg_hdr_valid` / `m_msg_seq_num` / `m_msg_length` / `m_msg_type` | out | Per-message sideband descriptor — held until `m_msg_hdr_ready`, same hold-until-accepted convention the vendored RX modules use |
| `gap_valid`, `gap_expected_seq` | out | Set when `seq_num > expected_seq` |
| `end_of_session` | out | Set on `count == 0xFFFF` |
| `error_truncated` | out | Set when a packet's own `eop` has already arrived but a header field or message body isn't fully available yet |
| `busy` | out | `busy <= (state != IDLE)`, one cycle lagging, same pattern the vendored RX modules use |

### The gearbox (byte realignment buffer)

The core problem this module has to solve: input arrives 8 bytes/cycle, but every field (session, seq_num, count, message lengths and bodies) can start at any byte offset, because it depends on how much variable-length data came before it. The gearbox turns "field straddles a 64-bit word boundary" into "read K bytes starting at byte 0 of a buffer."

- **Storage**: `gb_data`, a 16-byte (2-word) shift buffer. `gb_count` (0..16) tracks how many valid bytes are currently buffered. Byte 0 is always the oldest byte that hasn't been consumed yet.
- **Write side**: whenever `tvalid && tready`, appends this cycle's input beat — however many bytes `tkeep` marks valid (via `keep2count`, same lookup pattern the vendored RX modules use) — onto the tail of whatever's already buffered.
- **Read side**: the FSM sets `gb_rd_len` to say "consume this many bytes from the front this cycle" (0..8). The buffer slides down by that many bytes on the next clock. No read pointer to manage.
- **`s_udp_payload_axis_tready`** is computed one cycle ahead, based on where the buffer will sit after this cycle's slide-and-append settle, so it correctly stalls the upstream RX pipeline once the buffer fills.
- **`gb_eop`/`gb_err`**: one bit per buffered byte, riding alongside the data through every shift, so `tlast`/`tuser` survive realignment down to byte granularity.

Sized at 16 bytes: the largest single read is 8 bytes, and worst case there are 7 bytes already buffered (one short of an 8-byte read) before a fresh 8-byte input word arrives — 7+8=15, still fits.

This module keeps its own inline copy of the gearbox rather than instantiating `rtl/gearbox16.v` (extracted later, once `itch_decoder.v` and `itch_raw_deframer.v` needed the same logic) — deliberately left untouched to avoid any regression risk to this module's already-passing suite.

### The FSM

Four states: `IDLE → READ_HEADER → READ_MESSAGE → WAIT_NEXT → (IDLE)`.

#### `IDLE`
Waits until the gearbox has `>= 8` bytes buffered, then starts the header parse.

#### `READ_HEADER`
Pulls the fixed 20-byte MoldUDP64 header (10-byte session + 8-byte seq_num + 2-byte count) as four separate reads — 8+2+8+2 bytes — via an inner `hdr_step` sub-state machine. Each field is reconstructed from the buffer's byte-order via an explicit reversal, e.g. `{ gb_data[7:0], gb_data[15:8], ... }`, because the wire format is big-endian (first byte = most significant) while the buffer just stores bytes in arrival order.

Once `count` is latched (`HDR_COUNT`), runs the sequence check unconditionally — matching `sim/golden/mold_model.py`'s `feed()` logic exactly — and branches:

| Condition | Result |
|---|---|
| `seq_num < expected_seq` | Duplicate: drop it, `expected_seq` untouched |
| `seq_num > expected_seq` | Gap: sets `gap_valid`/`gap_expected_seq`, still processes the packet |
| `count == 0xFFFF` | `end_of_session` |
| `count == 0` | Heartbeat |
| otherwise | Normal packet: `msgs_remaining <= count`, `expected_seq <= seq_num + count`, → `READ_MESSAGE` |

Each real header step also treats "not enough bytes buffered yet, but this packet's own `eop` has already arrived" as truncation rather than infinite patience: `error_truncated` is set and the FSM bails to `WAIT_NEXT`, using `gb_has_eop_buffered`.

#### `READ_MESSAGE`
Streams each of `count` message blocks (`[2-byte length][length bytes of body]`) out as a sideband descriptor plus a body stream, via an inner `msg_step` sub-state machine: `MSG_LENGTH → MSG_LENGTH_SETTLE → MSG_TYPE_PEEK → MSG_HDR → MSG_BODY → MSG_DONE`, looping back to `MSG_LENGTH` for each of `msgs_remaining` blocks.

- **`MSG_LENGTH`** pulls the block's 2-byte length, latches it into `msgs_bytes_remaining`/`m_msg_length`, and computes `m_msg_seq_num` as `seq_num + (count - msgs_remaining)`. This early — not at `MSG_HDR`'s accept — matters; see "The same-edge producer/consumer race" below.
- **`MSG_TYPE_PEEK`** peeks `gb_data[7:0]` (the first body byte, without pulling it) and latches it into `m_msg_type`, *before* `MSG_HDR` ever asserts `m_msg_hdr_valid`. See "The same-edge producer/consumer race."
- **`MSG_HDR`** asserts `m_msg_hdr_valid` and holds it until `m_msg_hdr_ready` accepts it. A zero-length message (`length == 0`) skips straight to `MSG_DONE`: no body, no type byte, mirroring `mold_model.py`'s `type_byte = body[0] if length > 0 else None`.
- **`MSG_BODY`** streams the body out on `m_msg_payload_axis_*`, one gearbox pull per beat (`msg_pull_len = min(gb_count, msgs_bytes_remaining, 8)`), re-packing bytes to start at byte 0 and driving `tkeep` via `count2keep`. `tlast` is set on the pull that exactly empties `msgs_bytes_remaining`. Backpressure (`m_msg_payload_axis_tready`) holds the presented beat steady until accepted; `msg_body_settle` enforces the settle-cycle gap. `msg_would_truncate` (mirroring `mold_model.py`'s `off + 2 + length > len(packet)` check) routes to `WAIT_NEXT` on a short body.
- **`MSG_DONE`** decrements `msgs_remaining` and loops back to `MSG_LENGTH`, unless this was the last block, in which case it dispatches to `IDLE` or `WAIT_NEXT` depending on `msg_eop_consumed` — captured at pull time, see below.

Backpressure is propagated in both directions this state can stall on: `m_msg_hdr_ready` gates the sideband descriptor, `m_msg_payload_axis_tready` gates the body stream.

#### `WAIT_NEXT`
Drains whatever is left of the current packet — a dropped duplicate's leftover message bytes, or the remainder of a packet abandoned mid-parse after `error_truncated` — discarding up to 8 bytes/cycle via the same issue-then-settle alternation as the header steps, until it finds the packet's `eop`, then returns to `IDLE`.

---

## `gearbox16.v`

Extracted from `moldudp64_deframer.v`'s own inline gearbox once `itch_decoder.v` and `itch_raw_deframer.v` needed the exact same logic — same storage/write-side/read-side/`tready`-lookahead/`eop`-tracking design described above, made standalone with a clean pull-based port interface (`rd_len` in; `data`/`eop`/`err`/`count`/`has_eop_buffered` out). `moldudp64_deframer.v` itself was never refactored to use it — zero regression risk to its already-passing suite.

`has_eop_buffered` is exposed only as a whole-buffer lookahead ("is `eop` present anywhere currently buffered"). A parameterized "does a candidate pull of length L include eop" helper is deliberately *not* part of the module — every consumer builds that locally from `eop` and its own candidate pull length, keeping the capture-at-pull-time discipline (see below) visibly owned by each consumer's own FSM, not hidden inside the shared module.

`count2keep` (the inverse tkeep lookup, needed only by a module that streams a *byte-stream* body out, not by `itch_decoder.v` which has no such output) is *not* part of the shared module either — `itch_raw_deframer.v` keeps its own local copy, same as `moldudp64_deframer.v` does.

---

## `itch_decoder.v`

### Purpose

Table-driven field extractor for the 9 in-scope NASDAQ TotalView-ITCH 5.0 message types: System Event (`S`), Stock Directory (`R`), Add Order (`A`), Add Order w/ MPID (`F`), Order Executed (`E`), Order Executed With Price (`C`), Order Cancel (`X`), Order Delete (`D`), Order Replace (`U`). All other ITCH 5.0 message types are out of scope, reported via `m_dec_error_unknown_type`.

Field layouts were verified directly against NASDAQ's published TotalView-ITCH 5.0 Interface Specification (v5.0, 03/06/2015) — not reconstructed from memory alone, since one field ordering (Stock Directory's run of single-byte flags) was genuinely uncertain beforehand. The tables are transcribed identically into `cpp/itch_model.hpp`, the independent C++ reference decoder `sim/test_itch.py` cross-checks the RTL against for every test vector.

**Current state:** fully implemented and tested (`sim/test_itch.py`, 14 passing).

### Why table-driven, not state-per-field

The 9 types range from 1 field (System Event) to 14 fields (Stock Directory) after a shared 11-byte common header (`type(1) + stock_locate(2) + tracking_number(2) + timestamp(6)`). A literal state-per-field FSM, like `moldudp64_deframer.v`'s own fixed 4-step header parse, would need dozens of states across 9 types. Instead there's a fixed ~9-state FSM regardless of message type: two states pull the common header (`HDR_A`, `HDR_B`), then one generic loop (`FIELD_PULL`/`FIELD_SETTLE`) pulls every per-type field, driven by `type_field_count()`/`field_width()`/`field_is_ascii()` lookup functions keyed on the message type byte. Every field in every one of the 9 types is ≤8 bytes, so — the same constraint the deframer's own header parse relies on — no field ever straddles one gearbox pull.

Each type's total length is *fixed*, unlike a MoldUDP64 block's runtime-declared length, and known purely from the type byte via `type_total_length()`. This is checked in `IDLE` against the frontend's declared `s_msg_length` *before* a single body byte is touched — a cheap structural check (`m_dec_error_length_mismatch`) the deframer's own runtime-only truncation logic can't do.

### Output field-slot design

Dedicated ports for the always-meaningful common-header fields (`m_dec_msg_type`/`stock_locate`/`tracking_number`/`timestamp`, plus a `seq_num` passthrough), and `N_SLOTS=14` generic 64-bit slots for the per-type tail, flattened into one packed vector `m_dec_field_data` (matches this repo's Verilog-2001 style — no port-array sugar). Slot `k` lives at `m_dec_field_data[(k+1)*64-1 -: 64]`. ASCII fields are packed in arrival order, left-justified, zero-padded above their width; integer fields are byte-reversed into a right-justified unsigned value. `m_dec_field_count` says how many slots are meaningful (0 on any error path).

### Errors, per-event

Three flags — `m_dec_error_unknown_type`, `m_dec_error_length_mismatch`, `m_dec_error_truncated` — tagged *per decoded-message event* (alongside that message's own `m_dec_valid` pulse), not a coarse module-level status bit like the deframer's `error_truncated`. Possible here because the input is already message-framed by the upstream module — its own `tlast` always lands on a message boundary, so there's never ambiguity about which message an error belongs to. All three route through `DRAIN` (a copy of `WAIT_NEXT`) so every input message, success or error, produces exactly one `m_dec_valid` event.

---

## `itch_raw_deframer.v`

### Purpose

Frontend for raw historical NASDAQ ITCH sample files: a continuous stream of `[2-byte length][ITCH message]` blocks, structurally identical to one MoldUDP64 message block minus the session/sequence-number/gap-detection machinery around it. FSM: `LENGTH → LENGTH_SETTLE → TYPE_PEEK → HDR → BODY → DRAIN`, looping forever (no `count` field bounds how many blocks there are — the file just ends). Effectively `moldudp64_deframer.v`'s `READ_MESSAGE` sub-FSM promoted to be the entire top-level FSM.

**Current state:** fully implemented and tested (`sim/test_itch_pipeline.py`, 3 passing, exercised via `itch_raw_pipeline_top.v`).

Two signal-name reuses mean something different here than everywhere else in this repo:
- **`s_raw_axis_tlast` means end of file**, not end of one packet.
- **`m_msg_seq_num` is a synthetic local block index** (0, 1, 2, ... in file order), not a real MoldUDP64 sequence number.

### `end_of_input` detection needs a captured flag, not a live re-check

`LENGTH`'s own end-of-file / truncation check originally used a live `gb_has_eop_buffered` re-check, exactly the mistake "the capture-at-pull-time pattern" section below already warns about — and it reproduced the exact same failure mode. By the time `BODY`'s last pull consumes the file's final byte (which carries the `eop` marker), that bit has already shifted out of the buffer. A live re-check in `LENGTH` afterward always read false, so `end_of_input`/`error_truncated` never fired and every multi-cycle pipeline test hung.

**Fix:** `input_eop_consumed`, a register OR-accumulated (never cleared) at *every* pull site in the module — `LENGTH`'s own length-field pull, `BODY`'s body pulls, `DRAIN`'s discard pulls. `LENGTH` checks `gb_has_eop_buffered || input_eop_consumed`: the live check catches `eop` still sitting unconsumed (e.g. a dangling 1-byte fragment); the captured flag catches `eop` that was *just* consumed by whatever the immediately preceding pull was. Never needs clearing — real end-of-file is a one-time, permanent condition.

---

## `feed_parser_top.v` / `itch_raw_pipeline_top.v`

Pure wiring, no logic of their own — the live and raw-file paths shown in the Overview diagram, giving cocotb one bindable hierarchy per path (same reason `udp_rx_top.v` itself exists). `sim/test_feed_parser.py` (2 tests) and `sim/test_itch_pipeline.py` (3 tests) are thin end-to-end smoke tests, not full re-tests of each stage — each stage already has its own dedicated suite.

`feed_parser_top.v` is the first real instantiation site for `moldudp64_deframer.v`, which is why its module keyword (previously the typo `moldupd64_deframer`) was renamed to match the filename here — zero regression risk, since cocotb binds by port name, not module name, and nothing else referenced the old spelling.

---

## The settle-cycle pattern

A `gb_rd_len` pull takes a full clock to land in `gb_data`/`gb_count` — the gearbox registers its shift on the next posedge. Any state that issues a pull cannot read the result on the very next cycle; it must wait one cycle for the shift to settle first, and — easy to get backwards — the state that *resets `gb_rd_len` back to 0* must do so in that same settle cycle, not the state after: the gearbox's combinational slide reads `gb_rd_len` live, every cycle, so deferring the reset by even one further state re-issues the same shift a second time.

Instances found:

1. `moldudp64_deframer.v`, `READ_HEADER`: advancing straight from one header field to the next (no wait) read stale, pre-shift bytes. Fixed with explicit `HDR_SETTLE_1`–`4` steps.
2. `moldudp64_deframer.v`, `WAIT_NEXT`'s exit to `IDLE`: same problem, `IDLE` would start reading the dropped packet's tail as the next packet's header.
3. `moldudp64_deframer.v`, `WAIT_NEXT`'s internal loop: re-deciding a fresh `gb_rd_len` every cycle without waiting for the previous pull to land could issue an 8-byte pull when only 4 bytes truly remained — a real `gb_count` underflow (`4-8` wrapped to `28` in 5-bit unsigned).
4. `itch_decoder.v`, `HDR_A_SETTLE`/`HDR_B_SETTLE`/`FIELD_SETTLE`: written initially as bare one-line state transitions with no `gb_rd_len` reset of their own, reasoning (wrongly) that leaving it untouched through the settle cycle was equivalent to `moldudp64_deframer.v`'s blanket "reset at the top of every state" style. Traced by hand against the working reference before it ever ran in simulation: the reset has to happen *in* the settle state, not deferred to the state after, or the pulled length stays live for a second cycle and double-shifts the buffer. Fixed by making every settle state reset `gb_rd_len` itself.

## The capture-at-pull-time pattern (eop tracking)

A related but distinct bug from the settle-cycle one above: once a `gb_rd_len` pull consumes a byte, that byte's `gb_eop` bit is shifted out of the buffer right along with it. A helper like `gb_has_eop_buffered` ("is `eop` present anywhere in what's currently buffered") is only meaningful as a *lookahead* check — "has this packet's end already arrived, even if not yet consumed" — never as a way to ask, after the fact, "did the pull I just issued consume the `eop`?" By the time that pull's result has landed, the `eop` bit is gone from the buffer either way.

Instances found:

1. `moldudp64_deframer.v`, `MSG_DONE`: originally re-checked `gb_has_eop_buffered` to decide `IDLE` vs. `WAIT_NEXT`. For every well-formed packet the final body pull consumes the last byte *and* its `eop` bit together, so `gb_has_eop_buffered` was always false by the time `MSG_DONE` ran, misrouting even normal packets into `WAIT_NEXT`, which found nothing left to drain and hung forever. Fixed with `msg_eop_consumed`, captured at the moment of each relevant pull (`hdr_last_chunk_has_eop`, `wn_last_had_eop`, and `msg_eop_consumed` itself all follow this same shape).
2. `itch_raw_deframer.v`, `LENGTH`'s end-of-file check: the identical mistake, reproduced independently — see `itch_raw_deframer.v`'s own section above for the fix (`input_eop_consumed`).

## The same-edge producer/consumer race

A third pattern, distinct from the two above, surfaced only once `itch_decoder.v` had a real frontend feeding it (rather than a testbench that sequences hdr-then-body by hand): a signal a producer module writes *at the same clock edge* its consumer's accept condition also evaluates is invisible to that consumer *at that edge* — the consumer's `always @(posedge clk)` block reads the signal's value as it stood *before* the edge, never a value the producer is *simultaneously* scheduling to land at that same edge. This is ordinary synchronous-logic behavior, not a simulator quirk, but it's easy to get backwards when two independent state machines are driving and consuming a signal across a module boundary.

Instances found, both in `itch_raw_deframer.v` (written from scratch this pass, not carried over from `moldudp64_deframer.v`, which already avoided both by accident of its own field ordering):

1. **`m_msg_type`**: originally set only in `BODY`'s first real pull (`if (msgs_bytes_remaining == m_msg_length) m_msg_type <= gb_data[7:0];`) — a full state *after* `HDR`'s accept. `itch_decoder.v` reads `s_msg_type` the instant it accepts the hdr handshake, before any body byte has streamed — so every decoded message came back tagged with the *previous* message's type (or 0, for the first message ever). Fixed by adding `TYPE_PEEK`, a state before `HDR` that peeks `gb_data[7:0]` (without consuming it — `gb_rd_len` stays 0) and latches `m_msg_type` there, so it's already settled by the time `HDR` ever asserts `m_msg_hdr_valid`. The same fix was needed in — and applied to — `moldudp64_deframer.v`'s own `READ_MESSAGE` (`MSG_TYPE_PEEK`), since it had the identical gap; it was simply never exercised before `itch_decoder.v` existed as a real consumer.
2. **`m_msg_seq_num`**: originally set inside `HDR`'s own accept branch (`if (m_msg_hdr_valid && m_msg_hdr_ready) begin m_msg_seq_num <= local_seq; ... end`) — precisely the same edge `itch_decoder.v`'s `IDLE` also evaluates that same accept condition to decide whether to latch `s_msg_seq_num`. Confirmed empirically: every message after the first decoded with the *previous* message's `seq_num`, one message behind (traced by adding `local_seq` itself to a cycle-by-cycle dump — it incremented correctly; only the consumer-visible `m_msg_seq_num` lagged). Fixed by moving the assignment into `LENGTH` instead, well before `HDR` is ever reached — matching `moldudp64_deframer.v`'s own `MSG_LENGTH`, which already set `m_msg_seq_num` this early and was never actually broken.

## Latency characteristics: cut-through, not store-and-forward

`sim/bench_latency.py` measures a ~24-cycle minimum from Ethernet frame in to `moldudp64_deframer`'s header-accept (see README's Latency section). The question worth being able to answer cold: is that 24 cycles a sign the design buffers a frame before processing it, or is it real, accounted-for pipeline depth in an otherwise cut-through design? Verified directly against the code (not inferred from behavior alone) — it's the latter. No stage anywhere in this chain waits for a full frame, or even most of a header, before starting to interpret bytes.

**`eth_axis_rx.v`** (Ethernet, vendored): fields are captured combinationally off `s_axis_tdata`, keyed by a byte-offset pointer (`ptr_reg`) via the `_HEADER_FIELD_` macro — no frame buffer, no FIFO. The only storage is a single-word shift register, needed because `HDR_SIZE=14` isn't a multiple of the 8-byte bus (`OFFSET = 14 % 8 = 6`), so the last header beat also carries the first 2 payload bytes and has to be realigned — a byte-alignment cost of a wide bus, not a buffering choice. Header-complete detection and the switch to forwarding payload happen on the same accepted beat that completes the header (`ptr_reg == 13/BYTE_LANES`).

**`ip_eth_rx_64.v`** (IPv4, vendored) is the clearest example, and it's the textbook "parse optimistically, abort on a bad checksum" pattern: the header checksum is accumulated incrementally as each word of the header streams in (`hdr_sum_low_reg`/`hdr_sum_high_reg`, updated every valid cycle). Only the *final comparison* is deferred by one cycle — the code's own comment says why: `// check header checksum on next cycle for improved timing`. Critically, the module has *already* moved to `STATE_READ_PAYLOAD` and starts asserting `m_ip_payload_axis_tvalid` before that deferred check resolves. If the checksum then comes back bad, it retracts what it just asserted in the same cycle (`error_invalid_checksum_next=1; m_ip_payload_axis_tvalid_int=1'b0;`) and drops the rest of the frame. Confirmed by direct read of the RTL (`ip_eth_rx_64.v`, the `STATE_READ_HEADER`/`STATE_READ_PAYLOAD` cases), not inferred.

**`udp_ip_rx_64.v`** (UDP, vendored): the 8-byte UDP header is exactly one bus beat, so there's no multi-beat assembly at all — header capture and `m_udp_hdr_valid` fire in the same cycle the one header beat is accepted. (Separately worth noting: this module never validates the UDP checksum — it's captured and passed through, not checked. A coverage gap, not a latency one.)

**Chaining** (`udp_rx_top.v`, pure wiring, no registers of its own): the eth→ip handoff is fully overlapped — no bubble. The ip→udp handoff has exactly one bubble cycle: `ip_eth_rx_64`'s `STATE_IDLE` doesn't open `s_ip_payload_axis_tready` until the cycle after the header handshake (unlike the eth→ip boundary, which opens the next stage's ready in the same cycle), so an already-arrived UDP-header beat sits one extra cycle. Real, but small and localized to one specific boundary — not a sign of serialized, wait-for-the-previous-stage-to-fully-finish behavior.

**`moldudp64_deframer.v`'s `IDLE`** (own code): waits for `gb_count >= 8` before starting the header parse — but this is the unavoidable minimum, not slack. The gearbox's write side fills independently of FSM state every cycle regardless (`s_udp_payload_axis_tready_next` in the combinational block has no reference to `state` at all), and the very first real read (`HDR_SESSION_HI`, an 8-byte field) needs `gb_count>=8` regardless of which state name is doing the waiting. Given an 8-byte/cycle bus and an 8-byte first field, there's no way to start extracting it before 8 bytes have physically arrived.

Reconstructing the ~24-cycle minimum stage by stage (ideal back-to-back beats, decoder already idle):

| Segment | Cycles | What it is |
|---|---|---|
| `eth_axis_rx` | ~3 | 2 beats for the 14-byte header + 1 output-register cycle |
| `ip_eth_rx_64` | ~4 | 3 beats for the 20-byte header + 1 deferred-checksum cycle |
| `udp_ip_rx_64` (incl. the 1-cycle ip→udp bubble) | ~3 | 1-cycle chaining bubble + 1 beat (8B header) + 1 output-register cycle |
| **RX chain subtotal** | **≈10** | Ethernet byte 0 → first MoldUDP64 payload byte reaching the deframer |
| `moldudp64_deframer` `IDLE` | 2 | wait for `gb_count>=8` — the true minimum, not slack |
| `moldudp64_deframer` `READ_HEADER` | 8 | 4 gearbox pulls (8+2+8+2 bytes), each followed by its mandatory settle cycle |
| `moldudp64_deframer` `READ_MESSAGE` → `MSG_HDR` accept | 4 | `MSG_LENGTH`, `MSG_LENGTH_SETTLE`, `MSG_TYPE_PEEK`, `MSG_HDR`-issue |
| **Deframer subtotal** | **≈14** | first MoldUDP64 byte accepted → header + first message-header handshake |
| **Total** | **≈24** | matches the measured minimum |

The one place there's real, quantifiable, already-documented slack is the 8 cycles inside `READ_HEADER`: 20 bytes at up to 8B/cycle is a 3-beat quantity in principle, and the header parse already spends a 4th read on purpose to keep `seq_num` atomic (see that section above), then the mandatory settle-after-every-pull doubles those 4 reads into 8 FSM cycles. That's a real, deliberate correctness-over-latency trade-off — not an oversight — made after hitting the specific data-corruption and hang bugs documented in "The settle-cycle pattern" above. In an interview: not "I don't know where the cycles go," but "10 cycles are pipeline-register depth across three vendored stages, 14 are mine, and 8 of those 14 are a settle-cycle discipline I added after cutting that corner produced two real, reproducible bugs."

What this measurement is *not*: it's wire-to-decoded-message latency (a protocol parser's number), not wire-to-book-update latency (a market-data-handler's number) — that second number needs an order book engine, which doesn't exist yet. It's also cycle count, not nanoseconds — there's no synthesis or board in this project, so there's no closed clock frequency to convert against.

## Vendored vs. original

`rtl/vendor/*.v` — the Ethernet/IPv4/UDP RX chain — is vendored, unmodified, from Alex Forencich's [verilog-ethernet](https://github.com/alexforencich/verilog-ethernet). Everything else in `rtl/` (`moldudp64_deframer.v`, `gearbox16.v`, `itch_decoder.v`, `itch_raw_deframer.v`, `feed_parser_top.v`, `itch_raw_pipeline_top.v`) and everything in `cpp/` is original work.
