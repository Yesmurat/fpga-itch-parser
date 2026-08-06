`resetall
`timescale 1ns / 1ps
`default_nettype none

/*
 * RX-only wrapper: Ethernet -> IPv4 -> UDP, 64-bit AXI-Stream datapath.
 * Chains eth_axis_rx -> ip_eth_rx_64 -> udp_ip_rx_64 (all vendored, unmodified).
 * Sim-only: no MAC/PHY, no board constraints.
 */
module udp_rx_top (
    input  wire        clk,
    input  wire        rst,

    /*
     * Raw Ethernet frame AXI stream input
     */
    input  wire [63:0] s_axis_tdata,
    input  wire [7:0]  s_axis_tkeep,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,
    input  wire        s_axis_tuser,

    /*
     * UDP header output (parallel fields, strobed by m_udp_hdr_valid).
     * m_udp_hdr_ready is the one external header ready the TB controls;
     * every other inter-stage hdr_ready is wired stage-to-stage below.
     */
    output wire        m_udp_hdr_valid,
    input  wire        m_udp_hdr_ready,
    output wire [15:0] m_udp_source_port,
    output wire [15:0] m_udp_dest_port,
    output wire [15:0] m_udp_length,
    output wire [15:0] m_udp_checksum,

    /*
     * UDP payload AXI stream output
     */
    output wire [63:0] m_udp_payload_axis_tdata,
    output wire [7:0]  m_udp_payload_axis_tkeep,
    output wire        m_udp_payload_axis_tvalid,
    input  wire        m_udp_payload_axis_tready,
    output wire        m_udp_payload_axis_tlast,
    output wire        m_udp_payload_axis_tuser,

    /*
     * Status / error signals
     */
    output wire        error_invalid_header,
    output wire        error_invalid_checksum,
    output wire        error_eth_header_early_termination,
    output wire        error_ip_header_early_termination,
    output wire        error_ip_payload_early_termination,
    output wire        error_udp_header_early_termination,
    output wire        error_udp_payload_early_termination
);

// ----------------------------------------------------------------------
// Stage 1 (eth_axis_rx) -> Stage 2 (ip_eth_rx_64): Ethernet frame handoff
// ----------------------------------------------------------------------
wire        eth_hdr_valid;
wire        eth_hdr_ready;
wire [47:0] eth_dest_mac;
wire [47:0] eth_src_mac;
wire [15:0] eth_type;

wire [63:0] eth_payload_axis_tdata;
wire [7:0]  eth_payload_axis_tkeep;
wire        eth_payload_axis_tvalid;
wire        eth_payload_axis_tready;
wire        eth_payload_axis_tlast;
wire        eth_payload_axis_tuser;

// ----------------------------------------------------------------------
// Stage 2 (ip_eth_rx_64) -> Stage 3 (udp_ip_rx_64): IP frame handoff
// ----------------------------------------------------------------------
wire        ip_hdr_valid;
wire        ip_hdr_ready;
wire [47:0] ip_eth_dest_mac;
wire [47:0] ip_eth_src_mac;
wire [15:0] ip_eth_type;
wire [3:0]  ip_version;
wire [3:0]  ip_ihl;
wire [5:0]  ip_dscp;
wire [1:0]  ip_ecn;
wire [15:0] ip_length;
wire [15:0] ip_identification;
wire [2:0]  ip_flags;
wire [12:0] ip_fragment_offset;
wire [7:0]  ip_ttl;
wire [7:0]  ip_protocol;
wire [15:0] ip_header_checksum;
wire [31:0] ip_source_ip;
wire [31:0] ip_dest_ip;

wire [63:0] ip_payload_axis_tdata;
wire [7:0]  ip_payload_axis_tkeep;
wire        ip_payload_axis_tvalid;
wire        ip_payload_axis_tready;
wire        ip_payload_axis_tlast;
wire        ip_payload_axis_tuser;

// ----------------------------------------------------------------------
// Stage 1: Ethernet frame receiver
// ----------------------------------------------------------------------
eth_axis_rx #(
    .DATA_WIDTH(64)
) eth_axis_rx_inst (
    .clk(clk),
    .rst(rst),

    .s_axis_tdata(s_axis_tdata),
    .s_axis_tkeep(s_axis_tkeep),
    .s_axis_tvalid(s_axis_tvalid),
    .s_axis_tready(s_axis_tready),
    .s_axis_tlast(s_axis_tlast),
    .s_axis_tuser(s_axis_tuser),

    .m_eth_hdr_valid(eth_hdr_valid),
    .m_eth_hdr_ready(eth_hdr_ready),
    .m_eth_dest_mac(eth_dest_mac),
    .m_eth_src_mac(eth_src_mac),
    .m_eth_type(eth_type),
    .m_eth_payload_axis_tdata(eth_payload_axis_tdata),
    .m_eth_payload_axis_tkeep(eth_payload_axis_tkeep),
    .m_eth_payload_axis_tvalid(eth_payload_axis_tvalid),
    .m_eth_payload_axis_tready(eth_payload_axis_tready),
    .m_eth_payload_axis_tlast(eth_payload_axis_tlast),
    .m_eth_payload_axis_tuser(eth_payload_axis_tuser),

    .busy(),
    .error_header_early_termination(error_eth_header_early_termination)
);

// ----------------------------------------------------------------------
// Stage 2: IP frame receiver
// ----------------------------------------------------------------------
ip_eth_rx_64 ip_eth_rx_64_inst (
    .clk(clk),
    .rst(rst),

    .s_eth_hdr_valid(eth_hdr_valid),
    .s_eth_hdr_ready(eth_hdr_ready),
    .s_eth_dest_mac(eth_dest_mac),
    .s_eth_src_mac(eth_src_mac),
    .s_eth_type(eth_type),
    .s_eth_payload_axis_tdata(eth_payload_axis_tdata),
    .s_eth_payload_axis_tkeep(eth_payload_axis_tkeep),
    .s_eth_payload_axis_tvalid(eth_payload_axis_tvalid),
    .s_eth_payload_axis_tready(eth_payload_axis_tready),
    .s_eth_payload_axis_tlast(eth_payload_axis_tlast),
    .s_eth_payload_axis_tuser(eth_payload_axis_tuser),

    .m_ip_hdr_valid(ip_hdr_valid),
    .m_ip_hdr_ready(ip_hdr_ready),
    .m_eth_dest_mac(ip_eth_dest_mac),
    .m_eth_src_mac(ip_eth_src_mac),
    .m_eth_type(ip_eth_type),
    .m_ip_version(ip_version),
    .m_ip_ihl(ip_ihl),
    .m_ip_dscp(ip_dscp),
    .m_ip_ecn(ip_ecn),
    .m_ip_length(ip_length),
    .m_ip_identification(ip_identification),
    .m_ip_flags(ip_flags),
    .m_ip_fragment_offset(ip_fragment_offset),
    .m_ip_ttl(ip_ttl),
    .m_ip_protocol(ip_protocol),
    .m_ip_header_checksum(ip_header_checksum),
    .m_ip_source_ip(ip_source_ip),
    .m_ip_dest_ip(ip_dest_ip),
    .m_ip_payload_axis_tdata(ip_payload_axis_tdata),
    .m_ip_payload_axis_tkeep(ip_payload_axis_tkeep),
    .m_ip_payload_axis_tvalid(ip_payload_axis_tvalid),
    .m_ip_payload_axis_tready(ip_payload_axis_tready),
    .m_ip_payload_axis_tlast(ip_payload_axis_tlast),
    .m_ip_payload_axis_tuser(ip_payload_axis_tuser),

    .busy(),
    .error_header_early_termination(error_ip_header_early_termination),
    .error_payload_early_termination(error_ip_payload_early_termination),
    .error_invalid_header(error_invalid_header),
    .error_invalid_checksum(error_invalid_checksum)
);

// ----------------------------------------------------------------------
// Stage 3: UDP frame receiver
// ----------------------------------------------------------------------
udp_ip_rx_64 udp_ip_rx_64_inst (
    .clk(clk),
    .rst(rst),

    .s_ip_hdr_valid(ip_hdr_valid),
    .s_ip_hdr_ready(ip_hdr_ready),
    .s_eth_dest_mac(ip_eth_dest_mac),
    .s_eth_src_mac(ip_eth_src_mac),
    .s_eth_type(ip_eth_type),
    .s_ip_version(ip_version),
    .s_ip_ihl(ip_ihl),
    .s_ip_dscp(ip_dscp),
    .s_ip_ecn(ip_ecn),
    .s_ip_length(ip_length),
    .s_ip_identification(ip_identification),
    .s_ip_flags(ip_flags),
    .s_ip_fragment_offset(ip_fragment_offset),
    .s_ip_ttl(ip_ttl),
    .s_ip_protocol(ip_protocol),
    .s_ip_header_checksum(ip_header_checksum),
    .s_ip_source_ip(ip_source_ip),
    .s_ip_dest_ip(ip_dest_ip),
    .s_ip_payload_axis_tdata(ip_payload_axis_tdata),
    .s_ip_payload_axis_tkeep(ip_payload_axis_tkeep),
    .s_ip_payload_axis_tvalid(ip_payload_axis_tvalid),
    .s_ip_payload_axis_tready(ip_payload_axis_tready),
    .s_ip_payload_axis_tlast(ip_payload_axis_tlast),
    .s_ip_payload_axis_tuser(ip_payload_axis_tuser),

    .m_udp_hdr_valid(m_udp_hdr_valid),
    .m_udp_hdr_ready(m_udp_hdr_ready),
    .m_eth_dest_mac(),
    .m_eth_src_mac(),
    .m_eth_type(),
    .m_ip_version(),
    .m_ip_ihl(),
    .m_ip_dscp(),
    .m_ip_ecn(),
    .m_ip_length(),
    .m_ip_identification(),
    .m_ip_flags(),
    .m_ip_fragment_offset(),
    .m_ip_ttl(),
    .m_ip_protocol(),
    .m_ip_header_checksum(),
    .m_ip_source_ip(),
    .m_ip_dest_ip(),
    .m_udp_source_port(m_udp_source_port),
    .m_udp_dest_port(m_udp_dest_port),
    .m_udp_length(m_udp_length),
    .m_udp_checksum(m_udp_checksum),
    .m_udp_payload_axis_tdata(m_udp_payload_axis_tdata),
    .m_udp_payload_axis_tkeep(m_udp_payload_axis_tkeep),
    .m_udp_payload_axis_tvalid(m_udp_payload_axis_tvalid),
    .m_udp_payload_axis_tready(m_udp_payload_axis_tready),
    .m_udp_payload_axis_tlast(m_udp_payload_axis_tlast),
    .m_udp_payload_axis_tuser(m_udp_payload_axis_tuser),

    .busy(),
    .error_header_early_termination(error_udp_header_early_termination),
    .error_payload_early_termination(error_udp_payload_early_termination)
);

endmodule

`resetall
