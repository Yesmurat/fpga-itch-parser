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

    // Header pulled as 4 reads (8+2+8+2 bytes), one per field, so nothing
    // straddles a pull - seq_num in particular stays atomic since the
    // gap check depends on it.
    //
    // Each real step is followed by a SETTLE step: a gb_rd_len pull takes
    // one clock to land in gb_data, so the next step has to wait for it.
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

    // READ_MESSAGE: per block, pull a 2-byte length, emit the sideband
    // descriptor (held until accepted, like the vendored modules), then
    // stream the body. MSG_BODY combines its settle wait with the
    // backpressure wait instead of a dedicated settle step, since body
    // streaming loops an unknown number of times.
    localparam [2:0]
        MSG_LENGTH        = 3'd0,
        MSG_LENGTH_SETTLE = 3'd1,
        MSG_TYPE_PEEK     = 3'd2,
        MSG_HDR           = 3'd3,
        MSG_BODY          = 3'd4,
        MSG_DONE          = 3'd5;

    reg [2:0] msg_step         = MSG_LENGTH;
    reg       msg_body_settle  = 1'b0; // last body pull hasn't landed in gb_data yet
    reg       msg_eop_consumed = 1'b0; // did the last pull consume the packet's eop?
                                        // captured at pull time, checked in MSG_DONE.

    localparam integer GB_BYTES = 16; // 2x 64-bit words of headroom

    reg [8*GB_BYTES-1:0] gb_data;
    reg [GB_BYTES-1:0]   gb_eop; // 1 = this byte is the last byte of its packet (tlast)
    reg [GB_BYTES-1:0]   gb_err; // 1 = this byte's packet was flagged bad (tuser)
    reg [4:0]            gb_count; // 0..16 valid bytes currently buffered

    reg [3:0] gb_rd_len = 0; // FSM-driven: bytes to pop from the front this cycle

    // valid bytes in this beat, from tkeep (0..8)
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

    // inverse of keep2count: byte count -> tkeep
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

    // ---- header-parse helpers ----

    // count field, byte-reversed from gb_data[15:0]
    wire [15:0] hdr_count_val = { gb_data[7:0], gb_data[15:8] };

    // true if count's own 2 bytes include eop (20-byte packet, no messages)
    wire hdr_last_chunk_has_eop = |gb_eop[1:0];

    // true if eop is buffered anywhere - distinguishes "not enough bytes
    // yet" from "no more bytes coming" (truncated), used at both header
    // and message level.
    wire [GB_BYTES-1:0] gb_avail_mask     = ({GB_BYTES{1'b1}} >> (GB_BYTES - gb_count));
    wire                gb_has_eop_buffered = |(gb_eop & gb_avail_mask);

    // WAIT_NEXT's discard pull: min(gb_count, 8) bytes, plus whether that
    // pull includes eop (a variable-width part-select isn't legal, so
    // this masks with a variable shift instead).
    wire [3:0]          wn_pull_len   = (gb_count > 8) ? 4'd8 : gb_count[3:0];
    wire [GB_BYTES-1:0] wn_pull_mask  = ({GB_BYTES{1'b1}} >> (GB_BYTES - wn_pull_len));
    wire                wn_pull_has_eop = |(gb_eop & wn_pull_mask);

    // ---- READ_MESSAGE helpers ----

    // length field, same convention as hdr_count_val
    wire [15:0] msg_length_val = { gb_data[7:0], gb_data[15:8] };

    // bytes to pull this beat: min(gb_count, msgs_bytes_remaining, 8).
    // msgs_bytes_remaining is capped at GB_BYTES here since gb_count is
    // always the binding constraint once a message is longer than that.
    wire [4:0] msg_bytes_remaining_capped = (msgs_bytes_remaining > GB_BYTES[15:0]) ? GB_BYTES[4:0] : msgs_bytes_remaining[4:0];
    wire [4:0] msg_avail                  = (gb_count < msg_bytes_remaining_capped) ? gb_count : msg_bytes_remaining_capped;
    wire [3:0] msg_pull_len               = (msg_avail > 8) ? 4'd8 : msg_avail[3:0];

    // Does THIS pull's K bytes include eop? Must be captured at pull
    // time - once consumed, the eop bit shifts out with its byte, so
    // gb_has_eop_buffered can't see it afterward. Re-deriving it later
    // was the MSG_DONE bug: every well-formed packet read back "no eop"
    // and hung in WAIT_NEXT forever.
    wire [GB_BYTES-1:0] msg_pull_mask       = ({GB_BYTES{1'b1}} >> (GB_BYTES - msg_pull_len));
    wire                msg_body_pull_has_eop = |(gb_eop & msg_pull_mask);

    // true when the body can't be completed: fewer bytes buffered than
    // declared, and eop is already in what IS buffered. Mirrors
    // mold_model.py's "off + 2 + length > len(packet)" check.
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

        // 2) append the new beat after whatever survived the slide
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

        // 3) readiness for the next beat, based on where we'll sit after this settles
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

            // same 1-cycle-lagging pattern the vendored RX modules use;
            // harmless since busy is status-only, not used for control.
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
                    // gb_data, so every real step is followed by a
                    // *_SETTLE step that just waits one cycle before
                    // trusting gb_data again. Skipping this reads stale
                    // bytes - caught this from session[15:0] landing on
                    // the wrong offset.
                    case (hdr_step)

                        // Each step also checks gb_has_eop_buffered: "not
                        // enough bytes yet" is only safe to assume if more
                        // could still arrive. If eop is already buffered,
                        // none are coming - that's a truncated header,
                        // not a pending one.
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

                                // sequence check runs unconditionally, before
                                // count==0/0xFFFF cases, same as mold_model.py's feed()

                                gap_valid <= (seq_num > expected_seq);

                                if (seq_num > expected_seq)
                                    gap_expected_seq <= expected_seq;

                                if (seq_num < expected_seq) begin
                                    // duplicate/old: drop it whole, expected_seq
                                    // untouched. Drain via WAIT_NEXT unless the
                                    // header itself already carried eop.
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

                        // hdr_next_state was already decided in HDR_COUNT,
                        // but the transition itself still waits for that
                        // step's own pull to settle first.
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

                        // A consumer reads m_msg_type the instant it accepts
                        // the hdr handshake, before any body byte streams -
                        // so it has to be correct before MSG_HDR asserts
                        // valid. Peek gb_data[7:0] here without pulling
                        // (gb_rd_len stays 0); MSG_BODY still does the real
                        // pull later. Without this step m_msg_type only
                        // updated on MSG_BODY's first pull - one state too
                        // late, so every message came back tagged with the
                        // previous one's type.
                        MSG_TYPE_PEEK: begin
                            if (msgs_bytes_remaining == 16'd0) begin
                                msg_step <= MSG_HDR; // zero-length message: no body, no type byte to peek
                            end else if (gb_count >= 1) begin
                                m_msg_type <= gb_data[7:0];
                                msg_step    <= MSG_HDR;
                            end else if (gb_has_eop_buffered) begin
                                // body declared non-zero but packet ends first;
                                // MSG_BODY's own truncation check catches this.
                                msg_step <= MSG_HDR;
                            end
                        end

                        MSG_HDR: begin
                            // hold valid until accepted, same convention the vendored modules use
                            m_msg_hdr_valid <= 1'b1;
                            if (m_msg_hdr_valid && m_msg_hdr_ready) begin
                                m_msg_hdr_valid <= 1'b0;
                                // zero-length message: no body, no type byte - straight to MSG_DONE
                                msg_step <= (msgs_bytes_remaining == 16'd0) ? MSG_DONE : MSG_BODY;
                            end
                        end

                        MSG_BODY: begin
                            // Clear tvalid the moment a beat is accepted,
                            // separate from msg_body_settle below - a
                            // consumer samples tvalid&&tready on its own
                            // schedule. Without this a fast consumer could
                            // accept the same beat twice (caught this: a
                            // 1-beat message arrived as two frames).
                            if (m_msg_payload_axis_tvalid && m_msg_payload_axis_tready) begin
                                m_msg_payload_axis_tvalid <= 1'b0;
                            end

                            if (msg_body_settle) begin
                                // last pull hasn't landed yet; nothing else to do this cycle.
                                msg_body_settle <= 1'b0;
                            end else if (m_msg_payload_axis_tvalid && !m_msg_payload_axis_tready) begin
                                // presented beat not yet accepted; hold tdata/tvalid steady
                            end else if (msgs_bytes_remaining == 16'd0) begin
                                // body fully streamed (any final beat was just accepted above)
                                msg_step <= MSG_DONE;
                            end else if (msg_would_truncate) begin
                                // fewer bytes buffered than declared, and eop is
                                // already in what's buffered - no more coming.
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
                            // else: gb_count==0, not truncated - more input still expected, wait
                        end

                        MSG_DONE: begin
                            // No pull issued here or in MSG_HDR's zero-length
                            // path, so gb_data/gb_count are already fresh -
                            // no settle needed before looping to MSG_LENGTH.
                            // IDLE-vs-WAIT_NEXT uses msg_eop_consumed
                            // (captured at pull time), not a fresh
                            // gb_has_eop_buffered check - that bit shifts
                            // out with its byte, so a fresh check always
                            // reads false here and hangs.
                            msgs_remaining <= msgs_remaining - 16'd1;
                            msg_step       <= MSG_LENGTH;
                            if (msgs_remaining == 16'd1) begin
                                state <= msg_eop_consumed ? IDLE : WAIT_NEXT;
                            end
                        end

                    endcase
                end

                WAIT_NEXT: begin

                    // Drains leftover bytes of an abandoned packet (a
                    // dropped duplicate, or after error_truncated) until
                    // it finds eop. Same issue-then-settle alternation as
                    // the header: re-deciding gb_rd_len every cycle without
                    // a settle cycle reads the pre-shift count - caught
                    // this as a real underflow (4-8 wrapped to 28 in 5-bit
                    // unsigned) from issuing two pulls back to back.

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
