`timescale 1ns / 1ps
`default_nettype none

/*
itch_raw_pipeline_top: itch_raw_deframer.v -> itch_decoder.v wired
back to back. Exists so cocotb has one bindable hierarchy, same reason
rtl/vendor/udp_rx_top.v exists. No logic of its own.

The raw-historical-file path: feed s_raw_axis_* a [2-byte length][ITCH
message] block stream (no MoldUDP64 wrapper), get decoded messages out
on m_dec_*. Exercises the same itch_decoder that serves the live path
in rtl/feed_parser_top.v.
*/

module itch_raw_pipeline_top #(
    parameter N_SLOTS = 14
) (
    input wire clk,
    input wire rst,

    // Raw file byte stream in (s_raw_axis_tlast = EOF, not one block's end)
    input  wire [63:0] s_raw_axis_tdata,
    input  wire [7:0 ] s_raw_axis_tkeep,
    input  wire        s_raw_axis_tvalid,
    output wire        s_raw_axis_tready,
    input  wire        s_raw_axis_tlast,

    // Decoded-message descriptor out
    output wire                   m_dec_valid,
    input  wire                   m_dec_ready,
    output wire [63:0]            m_dec_seq_num,
    output wire [7:0 ]            m_dec_msg_type,
    output wire [15:0]            m_dec_stock_locate,
    output wire [15:0]            m_dec_tracking_number,
    output wire [47:0]            m_dec_timestamp,
    output wire [3:0]             m_dec_field_count,
    output wire [N_SLOTS*64-1:0]  m_dec_field_data,

    output wire m_dec_error_unknown_type,
    output wire m_dec_error_length_mismatch,
    output wire m_dec_error_truncated,

    // Frontend status
    output wire raw_error_truncated,
    output wire raw_end_of_input,
    output wire raw_busy,
    output wire dec_busy
);

    wire [63:0] msg_tdata;
    wire [7:0 ] msg_tkeep;
    wire        msg_tvalid;
    wire        msg_tready;
    wire        msg_tlast;

    wire        hdr_valid;
    wire        hdr_ready;
    wire [63:0] hdr_seq_num;
    wire [15:0] hdr_length;
    wire [7:0 ] hdr_type;

    itch_raw_deframer raw (
        .clk (clk),
        .rst (rst),

        .s_raw_axis_tdata  (s_raw_axis_tdata),
        .s_raw_axis_tkeep  (s_raw_axis_tkeep),
        .s_raw_axis_tvalid (s_raw_axis_tvalid),
        .s_raw_axis_tready (s_raw_axis_tready),
        .s_raw_axis_tlast  (s_raw_axis_tlast),

        .m_msg_payload_axis_tdata  (msg_tdata),
        .m_msg_payload_axis_tkeep  (msg_tkeep),
        .m_msg_payload_axis_tvalid (msg_tvalid),
        .m_msg_payload_axis_tready (msg_tready),
        .m_msg_payload_axis_tlast  (msg_tlast),

        .m_msg_hdr_valid  (hdr_valid),
        .m_msg_hdr_ready  (hdr_ready),
        .m_msg_seq_num    (hdr_seq_num),
        .m_msg_length     (hdr_length),
        .m_msg_type       (hdr_type),

        .error_truncated (raw_error_truncated),
        .end_of_input    (raw_end_of_input),
        .busy            (raw_busy)
    );

    itch_decoder #(
        .N_SLOTS(N_SLOTS)
    ) dec (
        .clk (clk),
        .rst (rst),

        .s_msg_payload_axis_tdata  (msg_tdata),
        .s_msg_payload_axis_tkeep  (msg_tkeep),
        .s_msg_payload_axis_tvalid (msg_tvalid),
        .s_msg_payload_axis_tready (msg_tready),
        .s_msg_payload_axis_tlast  (msg_tlast),

        .s_msg_hdr_valid (hdr_valid),
        .s_msg_hdr_ready (hdr_ready),
        .s_msg_seq_num   (hdr_seq_num),
        .s_msg_length    (hdr_length),
        .s_msg_type      (hdr_type),

        .m_dec_valid           (m_dec_valid),
        .m_dec_ready           (m_dec_ready),
        .m_dec_seq_num         (m_dec_seq_num),
        .m_dec_msg_type        (m_dec_msg_type),
        .m_dec_stock_locate    (m_dec_stock_locate),
        .m_dec_tracking_number (m_dec_tracking_number),
        .m_dec_timestamp       (m_dec_timestamp),
        .m_dec_field_count     (m_dec_field_count),
        .m_dec_field_data      (m_dec_field_data),

        .m_dec_error_unknown_type    (m_dec_error_unknown_type),
        .m_dec_error_length_mismatch (m_dec_error_length_mismatch),
        .m_dec_error_truncated       (m_dec_error_truncated),
        .busy                        (dec_busy)
    );

endmodule
