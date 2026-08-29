`timescale 1ns / 1ps
`default_nettype none

/*
------------------------------------------------------------------
feed_parser_top: the live/transport path, wired end to end --
rtl/vendor/udp_rx_top.v (Ethernet -> IPv4 -> UDP) -> moldudp64_deframer.v
(de-block, re-align, sequence/gap-detect) -> itch_decoder.v (per-type
field extraction). No logic of its own beyond port connections, same
role udp_rx_top.v itself already plays one layer down.

itch_decoder.v is frontend-agnostic -- this is one of two places it's
instantiated; rtl/itch_raw_pipeline_top.v is the other, wiring the
same decoder to itch_raw_deframer.v for the raw-historical-file path
instead. Both present the same m_msg_payload_axis_/m_msg_hdr_ shape to
the decoder, so it can't tell them apart.
------------------------------------------------------------------
*/

module feed_parser_top #(
    parameter N_SLOTS = 14
) (
    input wire clk,
    input wire rst,

    // Raw Ethernet frame in
    input  wire [63:0] s_axis_tdata,
    input  wire [7:0 ] s_axis_tkeep,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,
    input  wire        s_axis_tuser,

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

    // Per-stage status, passed straight through
    output wire rx_error_invalid_header,
    output wire rx_error_invalid_checksum,
    output wire rx_error_eth_header_early_termination,
    output wire rx_error_ip_header_early_termination,
    output wire rx_error_ip_payload_early_termination,
    output wire rx_error_udp_header_early_termination,
    output wire rx_error_udp_payload_early_termination,

    output wire        mold_gap_valid,
    output wire [63:0] mold_gap_expected_seq,
    output wire        mold_error_truncated,
    output wire        mold_end_of_session,
    output wire        mold_busy,

    output wire dec_busy
);

    // udp_rx_top's UDP header outputs are unused downstream (moldudp64_deframer
    // only needs the payload stream) but must still be given somewhere to
    // land and a ready so the RX chain doesn't stall waiting on it.
    wire        udp_hdr_valid;
    wire [15:0] udp_source_port;
    wire [15:0] udp_dest_port;
    wire [15:0] udp_length;
    wire [15:0] udp_checksum;

    wire [63:0] udp_payload_tdata;
    wire [7:0 ] udp_payload_tkeep;
    wire        udp_payload_tvalid;
    wire        udp_payload_tready;
    wire        udp_payload_tlast;
    wire        udp_payload_tuser;

    udp_rx_top rx (
        .clk (clk),
        .rst (rst),

        .s_axis_tdata  (s_axis_tdata),
        .s_axis_tkeep  (s_axis_tkeep),
        .s_axis_tvalid (s_axis_tvalid),
        .s_axis_tready (s_axis_tready),
        .s_axis_tlast  (s_axis_tlast),
        .s_axis_tuser  (s_axis_tuser),

        .m_udp_hdr_valid   (udp_hdr_valid),
        .m_udp_hdr_ready   (1'b1), // not consumed -- moldudp64_deframer only needs the payload stream
        .m_udp_source_port (udp_source_port),
        .m_udp_dest_port   (udp_dest_port),
        .m_udp_length      (udp_length),
        .m_udp_checksum    (udp_checksum),

        .m_udp_payload_axis_tdata  (udp_payload_tdata),
        .m_udp_payload_axis_tkeep  (udp_payload_tkeep),
        .m_udp_payload_axis_tvalid (udp_payload_tvalid),
        .m_udp_payload_axis_tready (udp_payload_tready),
        .m_udp_payload_axis_tlast  (udp_payload_tlast),
        .m_udp_payload_axis_tuser  (udp_payload_tuser),

        .error_invalid_header                 (rx_error_invalid_header),
        .error_invalid_checksum               (rx_error_invalid_checksum),
        .error_eth_header_early_termination   (rx_error_eth_header_early_termination),
        .error_ip_header_early_termination    (rx_error_ip_header_early_termination),
        .error_ip_payload_early_termination   (rx_error_ip_payload_early_termination),
        .error_udp_header_early_termination   (rx_error_udp_header_early_termination),
        .error_udp_payload_early_termination  (rx_error_udp_payload_early_termination)
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

    moldudp64_deframer mold (
        .clk (clk),
        .rst (rst),

        .s_udp_payload_axis_tdata  (udp_payload_tdata),
        .s_udp_payload_axis_tkeep  (udp_payload_tkeep),
        .s_udp_payload_axis_tvalid (udp_payload_tvalid),
        .s_udp_payload_axis_tready (udp_payload_tready),
        .s_udp_payload_axis_tlast  (udp_payload_tlast),
        .s_udp_payload_axis_tuser  (udp_payload_tuser),

        .m_msg_payload_axis_tdata  (msg_tdata),
        .m_msg_payload_axis_tkeep  (msg_tkeep),
        .m_msg_payload_axis_tvalid (msg_tvalid),
        .m_msg_payload_axis_tready (msg_tready),
        .m_msg_payload_axis_tlast  (msg_tlast),

        .m_msg_hdr_valid (hdr_valid),
        .m_msg_hdr_ready (hdr_ready),
        .m_msg_seq_num   (hdr_seq_num),
        .m_msg_length    (hdr_length),
        .m_msg_type      (hdr_type),

        .gap_valid        (mold_gap_valid),
        .gap_expected_seq (mold_gap_expected_seq),
        .error_truncated  (mold_error_truncated),
        .end_of_session   (mold_end_of_session),
        .busy             (mold_busy)
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
