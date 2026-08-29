`timescale 1ns / 1ps
`default_nettype none

/*
------------------------------------------------------------------
itch_decoder: table-driven field extractor for the 9 in-scope NASDAQ
TotalView-ITCH 5.0 message types (System Event 'S', Stock Directory
'R', Add Order 'A', Add Order w/ MPID 'F', Order Executed 'E', Order
Executed With Price 'C', Order Cancel 'X', Order Delete 'D', Order
Replace 'U'). All other ITCH 5.0 message types are out of scope and
are reported via m_dec_error_unknown_type.

Field layouts are transcribed directly from NASDAQ's published
TotalView-ITCH 5.0 Interface Specification (v5.0, 03/06/2015), section
4, and deliberately mirror cpp/itch_model.hpp's tables field-for-field
so a transcription mistake in one implementation is easy to spot
against the other (that C++ model is this module's golden-model
cross-check, run as a subprocess from sim/test_itch.py).

Frontend-agnostic input: this module doesn't know or care whether its
input came from moldudp64_deframer.v (live/transport path) or
itch_raw_deframer.v (raw historical-file path) -- both present the
same s_msg_payload_axis_ / s_msg_hdr_ port shape (body bytes re-aligned
to byte 0, plus a held-until-accepted sideband descriptor with the
declared length and type).

Why table-driven, not state-per-field: the 9 types range from 1 field
(System Event) to 14 fields (Stock Directory) after the shared 11-byte
common header (type + stock_locate + tracking_number + timestamp). A
literal state-per-field FSM would need dozens of states across 9
types. Instead there's a fixed ~9-state FSM: two states pull the
common header, then ONE generic FIELD_PULL/FIELD_SETTLE loop pulls
every per-type field, driven by type_field_count()/field_width()/
field_is_ascii() lookup functions keyed on the message type byte.
Every field in every one of the 9 types is <=8 bytes (verified against
the spec), so -- same constraint moldudp64_deframer.v's own header
parse relies on -- no field ever straddles one gearbox pull.

Each type's total length is FIXED (unlike MoldUDP64 blocks, whose
length is a runtime field) and known purely from the type byte via
type_total_length(). This is checked in IDLE against the frontend's
declared s_msg_length *before* a single body byte is even touched --
a cheap structural validation (m_dec_error_length_mismatch) the
runtime-only truncation check below can't do.

Output field-slot design: dedicated ports for the always-meaningful
common-header fields (m_dec_msg_type/stock_locate/tracking_number/
timestamp, plus a seq_num passthrough), and N_SLOTS generic 64-bit
slots (m_dec_field_data, flattened into one packed vector -- matches
the Verilog-2001 style used throughout this repo, no port-array
sugar) for the per-type tail. Slot k lives at
m_dec_field_data[(k+1)*64-1 -: 64]. N_SLOTS=14 exactly fits the widest
type (Stock Directory); 64 bits/slot exactly fits the widest single
field (8-byte order references, match numbers, stock symbols).
Per-type slot meanings (in field_idx order, matching cpp/itch_model.hpp):

  S (12B): [0]=Event Code
  R (39B): [0]=Stock [1]=Market Category [2]=Financial Status Indicator
           [3]=Round Lot Size [4]=Round Lots Only [5]=Issue Classification
           [6]=Issue Sub-Type [7]=Authenticity [8]=Short Sale Threshold Indicator
           [9]=IPO Flag [10]=LULD Reference Price Tier [11]=ETP Flag
           [12]=ETP Leverage Factor [13]=Inverse Indicator
  A (36B): [0]=Order Reference Number [1]=Buy/Sell Indicator [2]=Shares
           [3]=Stock [4]=Price
  F (40B): same as A, plus [5]=Attribution (MPID)
  E (31B): [0]=Order Reference Number [1]=Executed Shares [2]=Match Number
  C (36B): same as E, plus [3]=Printable [4]=Execution Price
  X (23B): [0]=Order Reference Number [1]=Cancelled Shares
  D (19B): [0]=Order Reference Number
  U (35B): [0]=Original Order Reference Number [1]=New Order Reference Number
           [2]=Shares [3]=Price

ASCII fields (Stock, MPID, single-char flags, ...) are packed in
arrival order, left-justified in their slot, zero-padded above their
width -- exactly the wire's own left-justified/space-padded
convention, no byte reversal. Integer fields are big-endian on the
wire and are byte-reversed into a right-justified unsigned value,
same convention moldudp64_deframer.v uses for its own header fields.

Errors are tagged PER DECODED-MESSAGE EVENT (alongside that message's
own m_dec_valid pulse), not a coarse module-level status bit like the
deframer's error_truncated -- possible here because the input is
already message-framed by the upstream module (its own tlast always
lands on a message boundary), so there's never ambiguity about which
message an error belongs to. Three flags, checked in this order:
m_dec_error_unknown_type (type byte not in the table),
m_dec_error_length_mismatch (declared length doesn't match the type's
fixed length), m_dec_error_truncated (declared length was fine, but
the byte stream ended early). All three route through DRAIN -- a copy
of moldudp64_deframer.v's WAIT_NEXT/capture-at-pull-time pattern --
so every input message, success or error, produces exactly one
m_dec_valid event, keeping the 1:1 contract uniform for testbench
diffing.

Both hard-won patterns from moldudp64_deframer.v apply here exactly
where they applied there: settle-cycle (every gb_rd_len pull needs one
full clock before its result is trusted -- HDR_A/HDR_B/FIELD_PULL each
have a dedicated *_SETTLE step, matching the gearbox's own contract)
and capture-at-pull-time (DRAIN's eop tracking is a register captured
at issue time, drain_eop_consumed, never a live has_eop_buffered
re-check after the fact -- that re-check bug is exactly what deadlocked
the deframer's own MSG_DONE before it was fixed there).
------------------------------------------------------------------
*/

module itch_decoder #(
    parameter N_SLOTS = 14
) (
    input wire clk,
    input wire rst,

    // From either frontend (moldudp64_deframer.v or itch_raw_deframer.v) -- identical shape
    input  wire [63:0] s_msg_payload_axis_tdata,
    input  wire [7:0 ] s_msg_payload_axis_tkeep,
    input  wire        s_msg_payload_axis_tvalid,
    output wire        s_msg_payload_axis_tready,
    input  wire        s_msg_payload_axis_tlast,

    input  wire        s_msg_hdr_valid,
    output wire         s_msg_hdr_ready,
    input  wire [63:0] s_msg_seq_num,
    input  wire [15:0] s_msg_length,
    input  wire [7:0 ] s_msg_type,

    // Decoded-message descriptor (hold-until-accepted, same convention as
    // the frontends' own m_msg_hdr_valid)
    output reg                     m_dec_valid,
    input  wire                    m_dec_ready,
    output reg  [63:0]             m_dec_seq_num,       // passthrough
    output reg  [7:0 ]             m_dec_msg_type,
    output reg  [15:0]             m_dec_stock_locate,
    output reg  [15:0]             m_dec_tracking_number,
    output reg  [47:0]             m_dec_timestamp,
    output reg  [3:0]              m_dec_field_count,   // 0..N_SLOTS: meaningful slots below
    output reg  [N_SLOTS*64-1:0]   m_dec_field_data,    // slot k at [(k+1)*64-1 -: 64]

    output reg          m_dec_error_unknown_type,
    output reg          m_dec_error_length_mismatch,
    output reg          m_dec_error_truncated,
    output reg          busy
);

    localparam integer GB_BYTES = 16;

    localparam [3:0]
        IDLE          = 4'd0,
        HDR_A         = 4'd1,  // pull 5B: type + stock_locate + tracking_number
        HDR_A_SETTLE  = 4'd2,
        HDR_B         = 4'd3,  // pull 6B: timestamp
        HDR_B_SETTLE  = 4'd4,
        FIELD_PULL    = 4'd5,  // pull field[field_idx]'s width; loops until field_idx==m_dec_field_count
        FIELD_SETTLE  = 4'd6,
        DRAIN         = 4'd7,  // WAIT_NEXT-style discard-to-eop, for every error path
        DONE          = 4'd8;  // hold m_dec_valid until m_dec_ready, success and error paths alike

    reg [3:0] state = IDLE;
    reg [3:0] field_idx = 4'd0;

    reg       drain_settle       = 1'b0; // last DRAIN pull hasn't landed yet
    reg       drain_eop_consumed = 1'b0; // captured at pull time, not re-derived after

    // ------------------------------------------------------------------
    // Per-type field tables. Total length and field count are keyed only
    // on the type byte; field width/encoding additionally need the field
    // index. See the module doc comment above for the field-by-field
    // meaning of each (type, k) pair -- transcribed 1:1 from
    // cpp/itch_model.hpp's TypeSpec tables.
    // ------------------------------------------------------------------

    function [15:0] type_total_length;
        input [7:0] t;
        case (t)
            "S": type_total_length = 16'd12;
            "R": type_total_length = 16'd39;
            "A": type_total_length = 16'd36;
            "F": type_total_length = 16'd40;
            "E": type_total_length = 16'd31;
            "C": type_total_length = 16'd36;
            "X": type_total_length = 16'd23;
            "D": type_total_length = 16'd19;
            "U": type_total_length = 16'd35;
            default: type_total_length = 16'd0; // unrecognized type
        endcase
    endfunction

    function [3:0] type_field_count;
        input [7:0] t;
        case (t)
            "S": type_field_count = 4'd1;
            "R": type_field_count = 4'd14;
            "A": type_field_count = 4'd5;
            "F": type_field_count = 4'd6;
            "E": type_field_count = 4'd3;
            "C": type_field_count = 4'd5;
            "X": type_field_count = 4'd2;
            "D": type_field_count = 4'd1;
            "U": type_field_count = 4'd4;
            default: type_field_count = 4'd0;
        endcase
    endfunction

    // field K's width in bytes (K < type_field_count(t)).
    function [3:0] field_width;
        input [7:0] t;
        input [3:0] k;
        case (t)
            "S": field_width = 4'd1; // [0] Event Code
            "R": case (k)
                    4'd0:  field_width = 4'd8; // Stock
                    4'd1:  field_width = 4'd1; // Market Category
                    4'd2:  field_width = 4'd1; // Financial Status Indicator
                    4'd3:  field_width = 4'd4; // Round Lot Size
                    4'd4:  field_width = 4'd1; // Round Lots Only
                    4'd5:  field_width = 4'd1; // Issue Classification
                    4'd6:  field_width = 4'd2; // Issue Sub-Type
                    4'd7:  field_width = 4'd1; // Authenticity
                    4'd8:  field_width = 4'd1; // Short Sale Threshold Indicator
                    4'd9:  field_width = 4'd1; // IPO Flag
                    4'd10: field_width = 4'd1; // LULD Reference Price Tier
                    4'd11: field_width = 4'd1; // ETP Flag
                    4'd12: field_width = 4'd4; // ETP Leverage Factor
                    4'd13: field_width = 4'd1; // Inverse Indicator
                    default: field_width = 4'd0;
                 endcase
            "A": case (k)
                    4'd0: field_width = 4'd8; // Order Reference Number
                    4'd1: field_width = 4'd1; // Buy/Sell Indicator
                    4'd2: field_width = 4'd4; // Shares
                    4'd3: field_width = 4'd8; // Stock
                    4'd4: field_width = 4'd4; // Price
                    default: field_width = 4'd0;
                 endcase
            "F": case (k)
                    4'd0: field_width = 4'd8;
                    4'd1: field_width = 4'd1;
                    4'd2: field_width = 4'd4;
                    4'd3: field_width = 4'd8;
                    4'd4: field_width = 4'd4;
                    4'd5: field_width = 4'd4; // Attribution (MPID)
                    default: field_width = 4'd0;
                 endcase
            "E": case (k)
                    4'd0: field_width = 4'd8; // Order Reference Number
                    4'd1: field_width = 4'd4; // Executed Shares
                    4'd2: field_width = 4'd8; // Match Number
                    default: field_width = 4'd0;
                 endcase
            "C": case (k)
                    4'd0: field_width = 4'd8;
                    4'd1: field_width = 4'd4;
                    4'd2: field_width = 4'd8;
                    4'd3: field_width = 4'd1; // Printable
                    4'd4: field_width = 4'd4; // Execution Price
                    default: field_width = 4'd0;
                 endcase
            "X": case (k)
                    4'd0: field_width = 4'd8; // Order Reference Number
                    4'd1: field_width = 4'd4; // Cancelled Shares
                    default: field_width = 4'd0;
                 endcase
            "D": field_width = 4'd8; // [0] Order Reference Number
            "U": case (k)
                    4'd0: field_width = 4'd8; // Original Order Reference Number
                    4'd1: field_width = 4'd8; // New Order Reference Number
                    4'd2: field_width = 4'd4; // Shares
                    4'd3: field_width = 4'd4; // Price
                    default: field_width = 4'd0;
                 endcase
            default: field_width = 4'd0;
        endcase
    endfunction

    // field K's encoding: 1 = ASCII (preserve arrival order), 0 = big-endian unsigned integer (reverse on pack).
    function field_is_ascii;
        input [7:0] t;
        input [3:0] k;
        case (t)
            "S": field_is_ascii = 1'b1; // Event Code
            "R": case (k)
                    4'd3, 4'd12: field_is_ascii = 1'b0; // Round Lot Size, ETP Leverage Factor
                    default:     field_is_ascii = 1'b1; // everything else is ASCII
                 endcase
            "A": case (k)
                    4'd1, 4'd3: field_is_ascii = 1'b1; // Buy/Sell Indicator, Stock
                    default:    field_is_ascii = 1'b0;
                 endcase
            "F": case (k)
                    4'd1, 4'd3, 4'd5: field_is_ascii = 1'b1; // Buy/Sell Indicator, Stock, Attribution
                    default:          field_is_ascii = 1'b0;
                 endcase
            "E": field_is_ascii = 1'b0; // all 3 fields are integers
            "C": field_is_ascii = (k == 4'd3); // only Printable is ASCII
            "X": field_is_ascii = 1'b0;
            "D": field_is_ascii = 1'b0;
            "U": field_is_ascii = 1'b0;
            default: field_is_ascii = 1'b0;
        endcase
    endfunction

    wire [15:0] hdr_type_total_length = type_total_length(s_msg_type);
    wire [3:0]  hdr_type_field_count  = type_field_count(s_msg_type);

    wire [3:0] cur_field_width    = field_width(m_dec_msg_type, field_idx);
    wire       cur_field_is_ascii = field_is_ascii(m_dec_msg_type, field_idx);

    // ------------------------------------------------------------------
    // Gearbox (extracted, shared with itch_raw_deframer.v -- see rtl/gearbox16.v)
    // ------------------------------------------------------------------

    reg [3:0] gb_rd_len;

    wire [8*GB_BYTES-1:0] gb_data;
    wire [GB_BYTES-1:0]   gb_eop;
    wire [4:0]            gb_count;
    wire                  gb_has_eop_buffered;

    gearbox16 #(
        .GB_BYTES(GB_BYTES)
    ) gb (
        .clk           (clk),
        .rst           (rst),
        .s_axis_tdata  (s_msg_payload_axis_tdata),
        .s_axis_tkeep  (s_msg_payload_axis_tkeep),
        .s_axis_tvalid (s_msg_payload_axis_tvalid),
        .s_axis_tready (s_msg_payload_axis_tready),
        .s_axis_tlast  (s_msg_payload_axis_tlast),
        .s_axis_tuser  (1'b0), // no error/tuser concept on this bus -- only tlast marks message end
        .rd_len            (gb_rd_len),
        .data              (gb_data),
        .eop               (gb_eop),
        .count             (gb_count),
        .has_eop_buffered  (gb_has_eop_buffered)
    );

    // DRAIN's per-cycle discard pull: consume min(gb_count, 8) bytes, and
    // report whether that pull's own bytes include the eop bit -- must be
    // captured into drain_eop_consumed at issue time (see doc comment).
    wire [3:0]           drain_pull_len     = (gb_count > 8) ? 4'd8 : gb_count[3:0];
    wire [GB_BYTES-1:0]  drain_pull_mask    = ({GB_BYTES{1'b1}} >> (GB_BYTES - drain_pull_len));
    wire                 drain_pull_has_eop = |(gb_eop & drain_pull_mask);

    assign s_msg_hdr_ready = (state == IDLE);

    integer fi; // FIELD_PULL's byte-packing loop variable

    always @(posedge clk) begin

        if (rst) begin

            state                        <= IDLE;
            field_idx                    <= 4'd0;
            drain_settle                 <= 1'b0;
            drain_eop_consumed           <= 1'b0;
            gb_rd_len                    <= 4'd0;
            m_dec_valid                  <= 1'b0;
            m_dec_seq_num                <= 64'd0;
            m_dec_msg_type               <= 8'd0;
            m_dec_stock_locate           <= 16'd0;
            m_dec_tracking_number        <= 16'd0;
            m_dec_timestamp              <= 48'd0;
            m_dec_field_count            <= 4'd0;
            m_dec_field_data             <= 0;
            m_dec_error_unknown_type     <= 1'b0;
            m_dec_error_length_mismatch  <= 1'b0;
            m_dec_error_truncated        <= 1'b0;
            busy                         <= 1'b0;

        end else begin

            // Same 1-cycle-lagging busy pattern moldudp64_deframer.v and
            // the vendored RX modules use.
            busy <= (state != IDLE);

            case (state)

                IDLE: begin

                    gb_rd_len <= 4'd0;

                    if (s_msg_hdr_valid && s_msg_hdr_ready) begin

                        m_dec_seq_num  <= s_msg_seq_num;
                        m_dec_msg_type <= s_msg_type;

                        if (hdr_type_total_length == 16'd0) begin
                            m_dec_error_unknown_type <= 1'b1;
                            drain_settle              <= 1'b0;
                            state                      <= DRAIN;
                        end else if (s_msg_length != hdr_type_total_length) begin
                            m_dec_error_length_mismatch <= 1'b1;
                            drain_settle                 <= 1'b0;
                            state                         <= DRAIN;
                        end else begin
                            m_dec_field_count <= hdr_type_field_count;
                            field_idx         <= 4'd0;
                            state             <= HDR_A;
                        end

                    end

                end

                HDR_A: begin // type(1) + stock_locate(2) + tracking_number(2)

                    gb_rd_len <= 4'd0;

                    if (gb_count >= 5) begin

                        m_dec_stock_locate    <= { gb_data[15:8],  gb_data[23:16] };
                        m_dec_tracking_number <= { gb_data[31:24], gb_data[39:32] };
                        gb_rd_len              <= 4'd5;
                        state                   <= HDR_A_SETTLE;

                    end else if (gb_has_eop_buffered) begin
                        m_dec_error_truncated <= 1'b1;
                        drain_settle           <= 1'b0;
                        state                   <= DRAIN;
                    end

                end

                HDR_A_SETTLE: begin
                    // gb_rd_len must stay at the value HDR_A just issued for
                    // this ENTIRE cycle (the gearbox's combinational slide
                    // reads it live, every cycle, and only lands the result
                    // at the edge leaving this cycle) -- so the reset to 0
                    // happens here, in this same branch, not deferred to
                    // HDR_B. Deferring it one state further would leave
                    // gb_rd_len at 5 for a second cycle too, double-shifting
                    // the buffer -- confirmed by hand-tracing the timing
                    // against moldudp64_deframer.v's own (correct) settle
                    // states before writing this.
                    gb_rd_len <= 4'd0;
                    state     <= HDR_B;
                end

                HDR_B: begin // timestamp(6)

                    gb_rd_len <= 4'd0;

                    if (gb_count >= 6) begin

                        m_dec_timestamp <= { gb_data[7:0],   gb_data[15:8],  gb_data[23:16],
                                              gb_data[31:24], gb_data[39:32], gb_data[47:40] };
                        gb_rd_len        <= 4'd6;
                        state             <= HDR_B_SETTLE;

                    end else if (gb_has_eop_buffered) begin
                        m_dec_error_truncated <= 1'b1;
                        drain_settle           <= 1'b0;
                        state                   <= DRAIN;
                    end

                end

                HDR_B_SETTLE: begin
                    gb_rd_len <= 4'd0; // see HDR_A_SETTLE's comment
                    state     <= FIELD_PULL;
                end

                FIELD_PULL: begin

                    gb_rd_len <= 4'd0;

                    if (gb_count >= {1'b0, cur_field_width}) begin

                        for (fi = 0; fi < 8; fi = fi + 1) begin
                            if (fi < cur_field_width) begin
                                if (cur_field_is_ascii)
                                    m_dec_field_data[(field_idx*64) + fi*8 +: 8] <= gb_data[fi*8 +: 8];
                                else
                                    m_dec_field_data[(field_idx*64) + fi*8 +: 8] <=
                                        gb_data[(cur_field_width - 1 - fi)*8 +: 8];
                            end else begin
                                m_dec_field_data[(field_idx*64) + fi*8 +: 8] <= 8'd0;
                            end
                        end

                        gb_rd_len <= cur_field_width;
                        field_idx <= field_idx + 4'd1;
                        state     <= FIELD_SETTLE;

                    end else if (gb_has_eop_buffered) begin
                        m_dec_error_truncated <= 1'b1;
                        drain_settle           <= 1'b0;
                        state                   <= DRAIN;
                    end

                end

                FIELD_SETTLE: begin
                    gb_rd_len <= 4'd0; // see HDR_A_SETTLE's comment
                    state     <= (field_idx == m_dec_field_count) ? DONE : FIELD_PULL;
                end

                DRAIN: begin

                    // Same issue-then-settle alternation, and the same
                    // capture-at-pull-time reasoning, as
                    // moldudp64_deframer.v's WAIT_NEXT -- see doc comment.
                    gb_rd_len <= 4'd0;

                    if (drain_settle) begin

                        drain_settle <= 1'b0;

                        if (drain_eop_consumed) begin
                            m_dec_field_count <= 4'd0; // no valid fields on any error path
                            state              <= DONE;
                        end

                    end else if (gb_count > 0) begin

                        gb_rd_len           <= drain_pull_len;
                        drain_eop_consumed  <= drain_pull_has_eop;
                        drain_settle         <= 1'b1;

                    end

                end

                DONE: begin

                    gb_rd_len   <= 4'd0; // defensive; already 0 by construction on every path in
                    m_dec_valid <= 1'b1;

                    if (m_dec_valid && m_dec_ready) begin
                        m_dec_valid                  <= 1'b0;
                        m_dec_error_unknown_type     <= 1'b0;
                        m_dec_error_length_mismatch  <= 1'b0;
                        m_dec_error_truncated        <= 1'b0;
                        state                         <= IDLE;
                    end

                end

                default: state <= IDLE;

            endcase

        end

    end

endmodule
