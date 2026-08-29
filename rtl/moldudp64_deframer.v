`timescale 1ns / 1ps
`default_nettype none

module moldudp64_deframer (
    input wire clk,
    input wire rst,

    // UDP payload in (from upd_rx_top.m_udp_payload_axis)
    input  wire [63:0] s_udp_payload_axis_tdata,
    input  wire [7:0 ] s_udp_payload_axis_tkeep,
    input  wire        s_udp_payload_axis_tvalid,
    output reg         s_udp_payload_axis_tready, // backpressure upstream
    input  wire        s_udp_payload_axis_tlast,
    input  wire        s_udp_payload_axis_tuser,

    // Per-message body out (re-aligned to byte 0)
    output reg  [63:0] m_msg_payload_axis_tdata,
    output reg  [7:0 ] m_msg_payload_axis_tkeep,
    output reg         m_msg_payload_axis_tvalid,
    input  wire        m_msg_payload_axis_tready, // backpressure from decoder
    output reg         m_msg_payload_axis_tlast,

    // Per-message sideband descriptor (valid with each message)
    output reg         m_msg_hdr_valid,
    input  wire        m_msg_hdr_ready,
    output reg  [63:0] m_msg_seq_num,
    output reg  [15:0] m_msg_length,
    output reg  [7:0 ] m_msg_type, // body[0]; valid when length > 0

    // Events/status
    output reg         gap_valid,
    output reg  [63:0] gap_expected_seq,
    output reg         error_truncated,
    output reg         end_of_session,
    output reg         busy

);

    localparam [1:0]
        IDLE                 = 2'd0,
        READ_HEADER          = 2'd1,
        READ_MESSAGE         = 2'd2,
        WAIT_NEXT            = 2'd3;

    reg [1:0] state = IDLE;

    reg [63:0] expected_seq = 0; // persists across packets (unlike seq_num below)

    reg [79:0] session = 0; // 10 bytes
    reg [63:0] seq_num = 0; // 8 bytes
    reg [15:0] count   = 0;   // 2 bytes

    reg [15:0] msgs_remaining       = 0; // counts down from 'count'
    reg [15:0] msgs_bytes_remaining = 0; // bytes left in current message body

    // Header is pulled from the gearbox as 4 sub-reads (8+2+8+2 bytes),
    // each landing entirely within one MoldUDP64 field (session hi/lo,
    // seq_num, count) so no field ever straddles a gearbox pull. That's
    // one more read than the doc's "up to three" estimate, in exchange
    // for never needing to reassemble a field split across two reads --
    // seq_num in particular (the field the gap check depends on) stays
    // a single atomic pull this way.
    //
    // Each real read step is followed by a SETTLE step: a gb_rd_len pull
    // takes one full clock to land in gb_data, so the step right after
    // issuing one must let that clock pass before trusting gb_data again.
    localparam [2:0]
        HDR_SESSION_HI = 3'd0,
        HDR_SETTLE_1   = 3'd1,
        HDR_SESSION_LO = 3'd2,
        HDR_SETTLE_2   = 3'd3,
        HDR_SEQNUM     = 3'd4,
        HDR_SETTLE_3   = 3'd5,
        HDR_COUNT      = 3'd6,
        HDR_SETTLE_4   = 3'd7;

    reg [2:0] hdr_step      = HDR_SESSION_HI;
    reg [1:0] hdr_next_state = IDLE; // decided in HDR_COUNT, applied in HDR_SETTLE_4
    reg       wn_settle       = 1'b0; // WAIT_NEXT's per-pull issue/settle alternation
    reg       wn_last_had_eop = 1'b0; // captured at issue time, acted on once that pull lands

    // READ_MESSAGE: per message block, pull a 2-byte length, emit the
    // sideband descriptor (held until m_msg_hdr_ready, matching the
    // vendored modules' own hold-until-accepted convention), then stream
    // the body. MSG_BODY folds "wait for the last pull to settle" and
    // "wait for the presented beat to be accepted" into one combined
    // guard (see the case body) rather than a dedicated settle step,
    // since unlike the header's fixed 4-step sequence, body streaming
    // loops an unknown number of times and additionally has to hold
    // through arbitrary downstream backpressure.
    localparam [2:0]
        MSG_LENGTH        = 3'd0,
        MSG_LENGTH_SETTLE = 3'd1,
        MSG_TYPE_PEEK     = 3'd2,
        MSG_HDR           = 3'd3,
        MSG_BODY          = 3'd4,
        MSG_DONE          = 3'd5;

    reg [2:0] msg_step         = MSG_LENGTH;
    reg       msg_body_settle  = 1'b0; // last body pull hasn't landed in gb_data yet
    reg       msg_eop_consumed = 1'b0; // did the most recent gb_rd_len pull in this
                                        // message's parse consume the packet's eop?
                                        // captured at pull time (see msg_body_pull_has_eop);
                                        // checked in MSG_DONE once the LAST message finishes.

    /*
    ------------------------------------------------------------------
    Gearbox: byte-addressable realignment buffer.
    
    The UDP payload arrives 8 bytes/cycle, but MoldUDP64 message
    boundaries land at arbitrary byte offsets (message N+1 starts at
    the running sum of all prior [length][data] blocks, which is
    data-dependent). The gearbox turns "field straddles a 64-bit word
    boundary" into "read K bytes starting at byte 0 of the buffer":
    bytes flow in up to 8 at a time on the write side whenever there's
    room, and the FSM below pulls out exactly the K bytes (K <= 8) it
    currently wants by driving gb_rd_len. Everything past that pull
    just slides down by K bytes on the next clock -- the FSM never has
    to reason about word alignment itself.
    
    Sized at 2 words (16 bytes, GB_BYTES) of storage: worst case going
    into a write, the buffer holds up to 7 bytes (one short of
    satisfying an 8-byte read); one more 8-byte input word brings that
    to 15, which still fits in 16. A read is never wider than 8 bytes,
    so that's the FSM's contract: pull the 20-byte header as up to
    three reads (<= 8 bytes each), not one 20-byte gulp.
    
    gb_eop/gb_err ride alongside the data, one bit per buffered byte,
    so tlast/tuser survive the realignment: whichever byte was the
    final valid byte of a tlast (or tlast+tuser) input beat keeps that
    marker as it slides through the buffer, so the FSM finds out
    "packet ends here" at the same byte granularity as everything
    else, however that end lands relative to the last 8-byte word.
    
    gb_rd_len is driven by the FSM below (READ_HEADER's steps and
    WAIT_NEXT). READ_MESSAGE doesn't drive it yet -- until that's
    built, a normal (count>0) packet fills the gearbox and correctly
    deasserts s_udp_payload_axis_tready once full, same "nothing
    downstream consuming yet" behavior as before anything was wired up.
    ------------------------------------------------------------------
    */

    localparam integer GB_BYTES = 16; // 2x 64-bit words of headroom

    reg [8*GB_BYTES-1:0] gb_data;
    reg [GB_BYTES-1:0]   gb_eop; // 1 = this byte is the last byte of its packet (tlast)
    reg [GB_BYTES-1:0]   gb_err; // 1 = this byte's packet was flagged bad (tuser)
    reg [4:0]            gb_count; // 0..16 valid bytes currently buffered

    reg [3:0] gb_rd_len = 0; // FSM-driven: bytes to pop from the front this cycle

    // Number of valid bytes in this input beat (0..8) -- same tkeep lookup the vendored modules use.
    function [3:0] keep2count;
        input [7:0] k;
        casez (k)
            8'bzzzzzzz0: keep2count = 4'd0;
            8'bzzzzzz01: keep2count = 4'd1;
            8'bzzzzz011: keep2count = 4'd2;
            8'bzzzz0111: keep2count = 4'd3;
            8'bzzz01111: keep2count = 4'd4;
            8'bzz011111: keep2count = 4'd5;
            8'bz0111111: keep2count = 4'd6;
            8'b01111111: keep2count = 4'd7;
            8'b11111111: keep2count = 4'd8;
        endcase
    endfunction

    wire [3:0] gb_in_count = keep2count(s_udp_payload_axis_tkeep);

    // Inverse of keep2count, for driving m_msg_payload_axis_tkeep from a
    // byte count -- same lookup the vendored modules use.
    function [7:0] count2keep;
        input [3:0] k;
        case (k)
            4'd0: count2keep = 8'b00000000;
            4'd1: count2keep = 8'b00000001;
            4'd2: count2keep = 8'b00000011;
            4'd3: count2keep = 8'b00000111;
            4'd4: count2keep = 8'b00001111;
            4'd5: count2keep = 8'b00011111;
            4'd6: count2keep = 8'b00111111;
            4'd7: count2keep = 8'b01111111;
            4'd8: count2keep = 8'b11111111;
        endcase
    endfunction

    // ------------------------------------------------------------------
    // Header-parse helper wires (combinational reads of the gearbox's
    // current front-of-buffer contents).
    // ------------------------------------------------------------------

    // count field, byte-order-reversed (big-endian wire -> value), read
    // from whatever is currently sitting at gb_data[15:0].
    wire [15:0] hdr_count_val = { gb_data[7:0], gb_data[15:8] };

    // True if the count field's own 2 bytes include this packet's eop --
    // i.e. the packet is exactly 20 bytes (no message blocks at all).
    wire hdr_last_chunk_has_eop = |gb_eop[1:0];

    // True if an eop bit is present ANYWHERE among the currently buffered
    // bytes -- i.e. this packet's end is already fully inside the
    // gearbox, so no more bytes are coming for it. Used to distinguish
    // "not enough bytes buffered yet, keep waiting" from "never going to
    // get enough bytes -- this packet is truncated" at both the header
    // and the message-body level.
    wire [GB_BYTES-1:0] gb_avail_mask     = ({GB_BYTES{1'b1}} >> (GB_BYTES - gb_count));
    wire                gb_has_eop_buffered = |(gb_eop & gb_avail_mask);

    // WAIT_NEXT's per-cycle discard-pull: consume min(gb_count, 8) bytes,
    // and report whether that pull includes the eop bit anywhere within
    // it (built as a shifted all-ones mask over the low wn_pull_len bits
    // of gb_eop, since a variable-width part-select isn't legal Verilog
    // but a variable shift amount is).
    wire [3:0]          wn_pull_len   = (gb_count > 8) ? 4'd8 : gb_count[3:0];
    wire [GB_BYTES-1:0] wn_pull_mask  = ({GB_BYTES{1'b1}} >> (GB_BYTES - wn_pull_len));
    wire                wn_pull_has_eop = |(gb_eop & wn_pull_mask);

    // ------------------------------------------------------------------
    // READ_MESSAGE helper wires.
    // ------------------------------------------------------------------

    // length field, byte-order-reversed, same convention as hdr_count_val.
    wire [15:0] msg_length_val = { gb_data[7:0], gb_data[15:8] };

    // How many body bytes to pull this beat: min(gb_count, msgs_bytes_remaining, 8).
    // msgs_bytes_remaining is capped at GB_BYTES for this comparison since
    // gb_count itself never exceeds GB_BYTES -- true message lengths can
    // be far larger, but then gb_count is always the binding constraint
    // anyway.
    wire [4:0] msg_bytes_remaining_capped = (msgs_bytes_remaining > GB_BYTES[15:0]) ? GB_BYTES[4:0] : msgs_bytes_remaining[4:0];
    wire [4:0] msg_avail                  = (gb_count < msg_bytes_remaining_capped) ? gb_count : msg_bytes_remaining_capped;
    wire [3:0] msg_pull_len               = (msg_avail > 8) ? 4'd8 : msg_avail[3:0];

    // True if THIS body pull's own K bytes include the packet's eop.
    // Must be captured at the moment of the pull, not re-derived later:
    // once consumed, the eop bit shifts out of the buffer along with the
    // byte it rode on, so a later "is eop currently buffered" check
    // (e.g. gb_has_eop_buffered) can no longer see it -- confirmed
    // empirically: every single-message test hung in MSG_DONE because it
    // tried exactly that re-derivation and always saw "no eop," routing
    // even well-formed packets through WAIT_NEXT, which then found
    // nothing left to drain and sat forever.
    wire [GB_BYTES-1:0] msg_pull_mask       = ({GB_BYTES{1'b1}} >> (GB_BYTES - msg_pull_len));
    wire                msg_body_pull_has_eop = |(gb_eop & msg_pull_mask);

    // True when the body can never be completed: fewer bytes are
    // buffered than this message still declares, and the packet's own
    // eop is already among what IS buffered (so no more is coming).
    // Mirrors mold_model.py's "off + 2 + length > len(packet)" check.
    wire msg_would_truncate = (gb_count < msgs_bytes_remaining) && gb_has_eop_buffered;

    // next-state (combinational)
    reg [8*GB_BYTES-1:0] gb_data_next;
    reg [GB_BYTES-1:0]   gb_eop_next;
    reg [GB_BYTES-1:0]   gb_err_next;
    reg [4:0]            gb_count_next;
    reg                  s_udp_payload_axis_tready_next;

    integer gi; // gearbox's own always @(*) loop variable
    integer mi; // READ_MESSAGE's body-byte-packing loop variable (separate always block)

    always @(*) begin

        // 1) slide down by whatever the FSM is consuming this cycle
        for (gi = 0; gi < GB_BYTES; gi = gi + 1) begin

            if (gi + gb_rd_len < gb_count) begin

                gb_data_next[gi*8 +: 8] = gb_data[(gi + gb_rd_len)*8 +: 8];
                gb_eop_next[gi]         = gb_eop[gi + gb_rd_len];
                gb_err_next[gi]         = gb_err[gi + gb_rd_len];

            end
            
            else begin

                gb_data_next[gi*8 +: 8] = 8'd0;
                gb_eop_next[gi]         = 1'b0;
                gb_err_next[gi]         = 1'b0;

            end

        end

        gb_count_next = gb_count - gb_rd_len;

        // 2) append this cycle's accepted input beat right after whatever is left post-slide
        if (s_udp_payload_axis_tvalid && s_udp_payload_axis_tready) begin

            for (gi = 0; gi < 8; gi = gi + 1) begin

                if (gi < gb_in_count) begin

                    gb_data_next[(gb_count_next + gi)*8 +: 8] = s_udp_payload_axis_tdata[gi*8 +: 8];

                    gb_eop_next[gb_count_next + gi]           = s_udp_payload_axis_tlast && (gi == gb_in_count - 1);

                    gb_err_next[gb_count_next + gi]           = s_udp_payload_axis_tuser && 
                                                                s_udp_payload_axis_tlast && 
                                                                (gi == gb_in_count - 1);

                end

            end

            gb_count_next = gb_count_next + gb_in_count;
            
        end

        // 3) decide readiness for the *next* beat based on where the
        //    buffer will actually sit once this cycle's slide+append settle
        s_udp_payload_axis_tready_next = (gb_count_next + 5'd8 <= GB_BYTES[4:0]);
        
    end

    // input alignment logic.
    always @(posedge clk) begin

        if (rst) begin

            gb_data                   <= 0;
            gb_eop                    <= 0;
            gb_err                    <= 0;
            gb_count                  <= 0;
            s_udp_payload_axis_tready <= 1; // empty buffer has room for a full word

        end
        
        else begin

            gb_data                   <= gb_data_next;
            gb_eop                    <= gb_eop_next;
            gb_err                    <= gb_err_next;
            gb_count                  <= gb_count_next;
            s_udp_payload_axis_tready <= s_udp_payload_axis_tready_next;

        end

    end

    // FSM logic + all outputs.
    always @(posedge clk) begin

        if (rst) begin

            state                     <= IDLE;
            expected_seq              <= 0;
            session                   <= 0;
            seq_num                   <= 0;
            count                     <= 0;
            hdr_step                  <= HDR_SESSION_HI;
            hdr_next_state            <= IDLE;
            wn_settle                 <= 0;
            wn_last_had_eop           <= 0;
            msg_step                  <= MSG_LENGTH;
            msg_body_settle           <= 0;
            msg_eop_consumed          <= 0;
            msgs_remaining            <= 0;
            msgs_bytes_remaining      <= 0;
            gb_rd_len                 <= 0;
            m_msg_payload_axis_tdata  <= 0;
            m_msg_payload_axis_tkeep  <= 0;
            m_msg_payload_axis_tvalid <= 0;
            m_msg_payload_axis_tlast  <= 0;
            m_msg_hdr_valid           <= 0;
            m_msg_seq_num             <= 0;
            m_msg_length              <= 0;
            m_msg_type                <= 0;
            gap_valid                 <= 0;
            gap_expected_seq          <= 0;
            error_truncated           <= 0;
            end_of_session            <= 0;
            busy                      <= 0;

        end

        else begin

            // Same 1-cycle-lagging pattern the vendored RX modules use
            // (busy_reg <= state_next != STATE_IDLE): harmless for a
            // status/debug signal not used for any control-flow decision.
            busy <= (state != IDLE);

            case (state)

                IDLE: begin

                    gb_rd_len <= 0;

                    if (gb_count >= 8) begin

                        hdr_step <= HDR_SESSION_HI;
                        state    <= READ_HEADER;

                    end

                end

                READ_HEADER: begin

                    gb_rd_len <= 0; // default: this step isn't ready to pull yet

                    // A gb_rd_len pull takes a full clock to land in
                    // gb_data (the gearbox registers its shift on the next
                    // posedge, same as everything else in this design) --
                    // so every step that just issued a pull is followed by
                    // an explicit *_SETTLE step that does nothing but let
                    // one clock pass before the next step trusts gb_data.
                    // Skipping this (reading gb_data the very next cycle
                    // after requesting a shift) reads stale, pre-shift
                    // bytes -- confirmed by tracing session[15:0] landing
                    // on the wrong bytes before this fix.
                    case (hdr_step)

                        // Each real step below also checks gb_has_eop_buffered
                        // in its else branch: "not enough bytes YET" (keep
                        // waiting) is only safe to assume if more bytes
                        // could still arrive. If this packet's own eop is
                        // already sitting in the buffer, no more are
                        // coming -- that's a truncated header, not a
                        // pending one. Real MoldUDP64 traffic is always
                        // >= 20 bytes, but malformed input shouldn't hang
                        // this FSM forever.
                        HDR_SESSION_HI: begin // session bytes 0..7

                            if (gb_count >= 8) begin

                                session[79:16] <= { gb_data[7:0],   gb_data[15:8],  gb_data[23:16], gb_data[31:24],
                                                    gb_data[39:32], gb_data[47:40], gb_data[55:48], gb_data[63:56] };

                                gb_rd_len <= 8;

                                hdr_step  <= HDR_SETTLE_1;

                            end else if (gb_has_eop_buffered) begin
                                error_truncated <= 1'b1;
                                state           <= WAIT_NEXT;
                            end

                        end

                        HDR_SETTLE_1: hdr_step <= HDR_SESSION_LO;

                        HDR_SESSION_LO: begin // session bytes 8..9

                            if (gb_count >= 2) begin

                                session[15:0] <= { gb_data[7:0], gb_data[15:8] };
                                gb_rd_len     <= 2;
                                hdr_step      <= HDR_SETTLE_2;

                            end else if (gb_has_eop_buffered) begin
                                error_truncated <= 1'b1;
                                state           <= WAIT_NEXT;
                            end

                        end

                        HDR_SETTLE_2: hdr_step <= HDR_SEQNUM;

                        HDR_SEQNUM: begin // seq_num bytes 0..7

                            if (gb_count >= 8) begin

                                seq_num   <= { gb_data[7:0],   gb_data[15:8],  gb_data[23:16], gb_data[31:24],
                                             gb_data[39:32], gb_data[47:40], gb_data[55:48], gb_data[63:56] };

                                gb_rd_len <= 8;

                                hdr_step  <= HDR_SETTLE_3;

                            end else if (gb_has_eop_buffered) begin
                                error_truncated <= 1'b1;
                                state           <= WAIT_NEXT;
                            end

                        end

                        HDR_SETTLE_3: hdr_step <= HDR_COUNT;

                        HDR_COUNT: begin // count bytes 0..1, then branch on the fully-latched header

                            if (gb_count >= 2) begin
                                count     <= hdr_count_val;
                                gb_rd_len <= 2;

                                // Sequence check runs unconditionally, same
                                // as mold_model.py's feed(), before the
                                // count==0/0xFFFF special cases are even
                                // considered.

                                gap_valid <= (seq_num > expected_seq);

                                if (seq_num > expected_seq)
                                    gap_expected_seq <= expected_seq;

                                if (seq_num < expected_seq) begin
                                    // Duplicate/old packet: drop it whole,
                                    // expected_seq untouched. If this
                                    // header's own last bytes already
                                    // carried eop (zero-body packet)
                                    // there's nothing left to drain;
                                    // otherwise its message bytes are
                                    // still sitting in the gearbox and
                                    // WAIT_NEXT has to discard them first.
                                    hdr_next_state <= hdr_last_chunk_has_eop ? IDLE : WAIT_NEXT;
                                end
                                
                                else if (hdr_count_val == 16'hFFFF) begin

                                    end_of_session <= 1'b1;
                                    expected_seq   <= seq_num;
                                    hdr_next_state <= hdr_last_chunk_has_eop ? IDLE : WAIT_NEXT;

                                end
                                
                                else if (hdr_count_val == 16'h0000) begin

                                    expected_seq   <= seq_num;
                                    hdr_next_state <= hdr_last_chunk_has_eop ? IDLE : WAIT_NEXT;

                                end
                                
                                else begin

                                    expected_seq   <= seq_num + hdr_count_val;
                                    msgs_remaining <= hdr_count_val;
                                    hdr_next_state <= READ_MESSAGE;

                                end

                                hdr_step <= HDR_SETTLE_4;
                            end else if (gb_has_eop_buffered) begin
                                error_truncated <= 1'b1;
                                state           <= WAIT_NEXT;
                            end

                        end

                        // Same settle-then-dispatch pattern for the branch
                        // decided above: hdr_next_state was picked using
                        // register values (seq_num, hdr_count_val) that
                        // were already safe to read, but the state
                        // transition itself must still wait for COUNT's
                        // own gb_rd_len<=2 to land, or WAIT_NEXT/READ_MESSAGE
                        // would start reading the same stale gb_data bug
                        // all over again on their very first cycle.
                        HDR_SETTLE_4: begin
                            state    <= hdr_next_state;
                            hdr_step <= HDR_SESSION_HI;
                        end

                    endcase
                end

                READ_MESSAGE: begin
                    gb_rd_len <= 0; // default: no pull issued unless a step below overrides it

                    case (msg_step)

                        MSG_LENGTH: begin // this block's 2-byte length field
                            if (gb_count >= 2) begin
                                msgs_bytes_remaining <= msg_length_val;
                                m_msg_length         <= msg_length_val;
                                m_msg_seq_num        <= seq_num + (count - msgs_remaining);
                                gb_rd_len            <= 2;
                                msg_eop_consumed     <= |gb_eop[1:0]; // in case this is a zero-length message
                                msg_step             <= MSG_LENGTH_SETTLE;
                            end else if (gb_has_eop_buffered) begin
                                // packet ended before even this length field arrived
                                error_truncated <= 1'b1;
                                state           <= WAIT_NEXT;
                            end
                        end

                        MSG_LENGTH_SETTLE: msg_step <= MSG_TYPE_PEEK;

                        // A downstream consumer (e.g. itch_decoder.v) reads
                        // m_msg_type the instant it accepts the hdr
                        // handshake -- before any body byte has actually
                        // been streamed on m_msg_payload_axis -- so the type
                        // byte (body[0]) must already be correct by the
                        // time MSG_HDR asserts m_msg_hdr_valid. Peeking
                        // gb_data[7:0] here (without pulling -- gb_rd_len
                        // stays 0, per the blanket default at the top of
                        // READ_MESSAGE) is safe and doesn't disturb
                        // MSG_BODY's own later pull of the same byte.
                        // Confirmed empirically wiring this deframer up to
                        // itch_decoder.v: without this step, m_msg_type was
                        // only ever updated during MSG_BODY's first real
                        // pull, one state too late -- every decoded message
                        // came back tagged with the PREVIOUS message's type
                        // (or 0, for the first message).
                        MSG_TYPE_PEEK: begin
                            if (msgs_bytes_remaining == 16'd0) begin
                                msg_step <= MSG_HDR; // zero-length message: no body, no type byte to peek
                            end else if (gb_count >= 1) begin
                                m_msg_type <= gb_data[7:0];
                                msg_step    <= MSG_HDR;
                            end else if (gb_has_eop_buffered) begin
                                // declared a non-zero-length body but the
                                // packet ends before even one byte of it
                                // arrives -- leave m_msg_type stale and
                                // proceed; MSG_BODY's own msg_would_truncate
                                // check catches this properly.
                                msg_step <= MSG_HDR;
                            end
                        end

                        MSG_HDR: begin
                            // Hold valid until accepted -- same convention
                            // the vendored RX modules use for their own
                            // *_hdr_valid outputs, not a single-cycle pulse.
                            m_msg_hdr_valid <= 1'b1;
                            if (m_msg_hdr_valid && m_msg_hdr_ready) begin
                                m_msg_hdr_valid <= 1'b0;
                                // zero-length message (length==0): no body,
                                // no type byte (mirrors mold_model.py's
                                // "type_byte = body[0] if length > 0 else
                                // None") -- skip straight to MSG_DONE.
                                msg_step <= (msgs_bytes_remaining == 16'd0) ? MSG_DONE : MSG_BODY;
                            end
                        end

                        MSG_BODY: begin
                            // Clear tvalid the instant the presented beat
                            // is accepted, independent of msg_body_settle
                            // below: tvalid is visible to the outside
                            // world every cycle it's held high, so a
                            // consumer samples tvalid&&tready on its own
                            // schedule regardless of our internal
                            // bookkeeping. Leaving tvalid asserted through
                            // the settle cycle without this check let a
                            // fast consumer accept the same beat twice --
                            // confirmed empirically (a 1-beat "FIRST"
                            // message arrived as two identical RX frames).
                            if (m_msg_payload_axis_tvalid && m_msg_payload_axis_tready) begin
                                m_msg_payload_axis_tvalid <= 1'b0;
                            end

                            if (msg_body_settle) begin
                                // last body pull hasn't landed in gb_data
                                // yet; nothing else to do this cycle.
                                msg_body_settle <= 1'b0;
                            end else if (m_msg_payload_axis_tvalid && !m_msg_payload_axis_tready) begin
                                // presented beat not yet accepted; hold tdata/tvalid steady
                            end else if (msgs_bytes_remaining == 16'd0) begin
                                // body fully streamed (any final beat was
                                // just accepted above)
                                msg_step <= MSG_DONE;
                            end else if (msg_would_truncate) begin
                                // fewer bytes buffered than this message
                                // still declares, and this packet's eop is
                                // already among what IS buffered -- no more
                                // is coming. Mirrors mold_model.py's
                                // "off + 2 + length > len(packet)" check.
                                error_truncated           <= 1'b1;
                                m_msg_payload_axis_tvalid <= 1'b0;
                                state                     <= WAIT_NEXT;
                            end else if (gb_count > 0) begin
                                for (mi = 0; mi < 8; mi = mi + 1) begin
                                    if (mi < msg_pull_len) begin
                                        m_msg_payload_axis_tdata[mi*8 +: 8] <= gb_data[mi*8 +: 8];
                                    end else begin
                                        m_msg_payload_axis_tdata[mi*8 +: 8] <= 8'd0;
                                    end
                                end
                                m_msg_payload_axis_tkeep  <= count2keep(msg_pull_len);
                                m_msg_payload_axis_tvalid <= 1'b1;
                                m_msg_payload_axis_tlast  <= (msgs_bytes_remaining == {12'd0, msg_pull_len});
                                if (msgs_bytes_remaining == m_msg_length)
                                    m_msg_type <= gb_data[7:0]; // first body byte only
                                msgs_bytes_remaining <= msgs_bytes_remaining - {12'd0, msg_pull_len};
                                gb_rd_len            <= msg_pull_len;
                                msg_eop_consumed     <= msg_body_pull_has_eop;
                                msg_body_settle      <= 1'b1;
                            end
                            // else: gb_count==0, not truncated -- more input still expected, wait
                        end

                        MSG_DONE: begin
                            // Neither this branch nor the MSG_HDR zero-length
                            // path above issues a gb_rd_len of its own, so
                            // gb_data/gb_count are already fresh here -- no
                            // settle needed before looping straight back
                            // into MSG_LENGTH. The IDLE-vs-WAIT_NEXT choice
                            // below uses msg_eop_consumed (captured at pull
                            // time), NOT a fresh gb_has_eop_buffered check:
                            // the eop bit shifts out of the buffer together
                            // with the byte it rode on, so by the time a
                            // well-formed packet's last pull has landed,
                            // gb_has_eop_buffered would already read back
                            // false -- confirmed empirically, this was
                            // hanging every normal single/multi-message test.
                            msgs_remaining <= msgs_remaining - 16'd1;
                            msg_step       <= MSG_LENGTH;
                            if (msgs_remaining == 16'd1) begin
                                state <= msg_eop_consumed ? IDLE : WAIT_NEXT;
                            end
                        end

                    endcase
                end

                WAIT_NEXT: begin

                    // Not a general "drain to end of packet" state yet --
                    // just enough to discard a dropped duplicate's
                    // leftover message bytes and find its eop. Reused
                    // as-is once READ_MESSAGE exists and needs the same
                    // "abandon this packet" path (e.g. on error_truncated).
                    //
                    // Same issue-then-settle alternation as the header
                    // steps, and for the same reason: a gb_rd_len pull
                    // takes a full clock to land. Re-deciding a fresh
                    // gb_rd_len every cycle from gb_count (no settle
                    // cycle in between) means the second decision is
                    // still looking at the pre-shift byte count from the
                    // first pull -- confirmed empirically: it issued a
                    // second 8-byte pull while only 4 bytes truly
                    // remained, underflowing gb_count (4-8 wrapped to 28
                    // in 5-bit unsigned). wn_settle enforces one clock of
                    // wait between every pull and the next gb_count read
                    // that depends on it, not just on the final exit.

                    gb_rd_len <= 0; // default, same pattern as IDLE/READ_HEADER above

                    if (wn_settle) begin

                        wn_settle <= 1'b0;

                        if (wn_last_had_eop) begin
                            state <= IDLE;
                        end

                    end
                    
                    else if (gb_count > 0) begin

                        gb_rd_len       <= wn_pull_len;
                        wn_last_had_eop <= wn_pull_has_eop;
                        wn_settle       <= 1'b1;

                    end

                end
                
                default: state <= IDLE;

            endcase
            
        end
        
    end
    
endmodule