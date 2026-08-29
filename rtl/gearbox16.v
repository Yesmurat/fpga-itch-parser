`timescale 1ns / 1ps
`default_nettype none

/*
------------------------------------------------------------------
gearbox16: byte-addressable realignment buffer.

Extracted from rtl/moldudp64_deframer.v's own inline gearbox (same
gb_data/gb_eop/gb_err/gb_count storage, same keep2count lookup, same
slide-then-append always @(*) block) so itch_decoder.v and
itch_raw_deframer.v don't each carry their own copy. moldudp64_deframer.v
itself is untouched -- it keeps its inline copy, so this extraction
carries zero regression risk to its own already-passing tests.

The problem this solves: input arrives up to 8 bytes/cycle, but a
consumer's fields (MoldUDP64 header/message fields, ITCH message
fields, ...) can start at any byte offset, because that depends on how
much variable-length data came before it. This buffer turns "field
straddles a 64-bit word boundary" into "read K<=8 bytes starting at
byte 0 of a buffer":

  - Write side: whenever s_axis_tvalid && s_axis_tready, appends this
    cycle's input beat -- however many bytes tkeep marks valid (via
    keep2count) -- onto the tail of whatever's already buffered.
  - Read side: the consumer FSM drives rd_len to say "consume this
    many bytes from the front this cycle" (0..8). The buffer slides
    down by that many bytes on the *next* clock -- a gb_rd_len pull
    takes one full clock to land in data/count (the shift is
    registered, same as everything else here). Any consumer that
    issues a pull must wait one full clock before trusting
    data/count/eop/err again (a dedicated settle step, or an
    equivalent hold flag) -- reading them on the very same cycle a
    pull is issued reads stale, pre-shift bytes. This "settle-cycle"
    requirement bit three times independently while building
    moldudp64_deframer.v; it applies identically here.
  - s_axis_tready is computed one cycle ahead, based on where the
    buffer will actually sit after this cycle's slide-and-append
    settle, so it correctly stalls the upstream source once the
    buffer fills.
  - eop/err ride alongside the data, one bit per buffered byte, so
    tlast/tuser survive realignment: whichever byte was the final
    valid byte of a tlast (or tlast+tuser) input beat keeps that
    marker as it slides through the buffer, so a consumer finds out
    "the input ended here" at the same byte granularity as everything
    else, however that end lands relative to the last 8-byte word.
  - has_eop_buffered is a whole-buffer lookahead only ("is eop present
    ANYWHERE among the currently buffered bytes") -- every consumer of
    the deframer's own equivalent signal uses it this way, to
    distinguish "not enough bytes buffered yet, keep waiting" from
    "never going to get enough bytes -- truncated" before issuing a
    pull. It is NOT safe to re-check after a pull to ask "did the pull
    I just issued consume the eop" -- once a byte is consumed, its eop
    bit shifts out of the buffer with it. A consumer that needs that
    answer must build a candidate-pull-length mask from eop itself
    and capture the result into a register at the moment the pull is
    issued (exactly as moldudp64_deframer.v's msg_body_pull_has_eop /
    msg_eop_consumed do) -- deliberately not something this module
    tries to do on a consumer's behalf.

Sized at 2 words (16 bytes, GB_BYTES) of storage by default: worst case
going into a write, the buffer holds up to 7 bytes (one short of
satisfying an 8-byte read); one more 8-byte input word brings that to
15, which still fits in 16. This holds for any consumer whose reads
never exceed 8 bytes -- true for every field in both itch_decoder.v's
per-type tables and itch_raw_deframer.v's length/body pulls.
------------------------------------------------------------------
*/

module gearbox16 #(
    parameter GB_BYTES = 16
) (
    input wire clk,
    input wire rst,

    // write side -- standard 64-bit AXI4-Stream in
    input  wire [63:0] s_axis_tdata,
    input  wire [7:0 ] s_axis_tkeep,
    input  wire        s_axis_tvalid,
    output reg         s_axis_tready,
    input  wire        s_axis_tlast,
    input  wire        s_axis_tuser,

    // read side -- pull-based (see doc comment above for the settle-cycle contract)
    input  wire [3:0]              rd_len,
    output wire [8*GB_BYTES-1:0]   data,             // byte 0 = oldest unconsumed byte
    output wire [GB_BYTES-1:0]     eop,               // per-byte tlast marker
    output wire [GB_BYTES-1:0]     err,               // per-byte tuser marker
    output wire [4:0]              count,             // 0..GB_BYTES valid bytes buffered
    output wire                    has_eop_buffered   // lookahead only -- see doc comment
);

    reg [8*GB_BYTES-1:0] gb_data;
    reg [GB_BYTES-1:0]   gb_eop;
    reg [GB_BYTES-1:0]   gb_err;
    reg [4:0]            gb_count;

    assign data  = gb_data;
    assign eop   = gb_eop;
    assign err   = gb_err;
    assign count = gb_count;

    wire [GB_BYTES-1:0] gb_avail_mask = ({GB_BYTES{1'b1}} >> (GB_BYTES - gb_count));
    assign has_eop_buffered = |(gb_eop & gb_avail_mask);

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

    wire [3:0] gb_in_count = keep2count(s_axis_tkeep);

    // next-state (combinational)
    reg [8*GB_BYTES-1:0] gb_data_next;
    reg [GB_BYTES-1:0]   gb_eop_next;
    reg [GB_BYTES-1:0]   gb_err_next;
    reg [4:0]            gb_count_next;
    reg                  s_axis_tready_next;

    integer gi;

    always @(*) begin

        // 1) slide down by whatever the consumer is pulling this cycle
        for (gi = 0; gi < GB_BYTES; gi = gi + 1) begin

            if (gi + rd_len < gb_count) begin

                gb_data_next[gi*8 +: 8] = gb_data[(gi + rd_len)*8 +: 8];
                gb_eop_next[gi]         = gb_eop[gi + rd_len];
                gb_err_next[gi]         = gb_err[gi + rd_len];

            end

            else begin

                gb_data_next[gi*8 +: 8] = 8'd0;
                gb_eop_next[gi]         = 1'b0;
                gb_err_next[gi]         = 1'b0;

            end

        end

        gb_count_next = gb_count - rd_len;

        // 2) append this cycle's accepted input beat right after whatever is left post-slide
        if (s_axis_tvalid && s_axis_tready) begin

            for (gi = 0; gi < 8; gi = gi + 1) begin

                if (gi < gb_in_count) begin

                    gb_data_next[(gb_count_next + gi)*8 +: 8] = s_axis_tdata[gi*8 +: 8];

                    gb_eop_next[gb_count_next + gi]           = s_axis_tlast && (gi == gb_in_count - 1);

                    gb_err_next[gb_count_next + gi]           = s_axis_tuser &&
                                                                s_axis_tlast &&
                                                                (gi == gb_in_count - 1);

                end

            end

            gb_count_next = gb_count_next + gb_in_count;

        end

        // 3) decide readiness for the *next* beat based on where the
        //    buffer will actually sit once this cycle's slide+append settle
        s_axis_tready_next = (gb_count_next + 5'd8 <= GB_BYTES[4:0]);

    end

    always @(posedge clk) begin

        if (rst) begin

            gb_data       <= 0;
            gb_eop        <= 0;
            gb_err        <= 0;
            gb_count      <= 0;
            s_axis_tready <= 1; // empty buffer has room for a full word

        end

        else begin

            gb_data       <= gb_data_next;
            gb_eop        <= gb_eop_next;
            gb_err        <= gb_err_next;
            gb_count      <= gb_count_next;
            s_axis_tready <= s_axis_tready_next;

        end

    end

endmodule
