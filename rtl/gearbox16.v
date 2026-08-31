`timescale 1ns / 1ps
`default_nettype none
// gearbox16: byte-addressable realignment buffer.

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

    // read side: rd_len pops bytes off the front; the shift is registered,
    // so pulled data lands one clock later
    input  wire [3:0]              rd_len,
    output wire [8*GB_BYTES-1:0]   data,              // byte 0 = oldest unconsumed byte
    output wire [GB_BYTES-1:0]     eop,               // per-byte tlast marker
    output wire [GB_BYTES-1:0]     err,               // per-byte tuser marker
    output wire [4:0]              count,             // 0..GB_BYTES valid bytes buffered
    output wire                    has_eop_buffered   // eop present among buffered bytes
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

        // 2) append the new beat after whatever survived the slide
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

        // 3) readiness for the next beat, based on where we'll sit after this settles
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
