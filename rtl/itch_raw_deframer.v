`timescale 1ns / 1ps
`default_nettype none

/*
---------------------------------
itch_raw_deframer: frontend for raw historical NASDAQ ITCH sample
files - a continuous stream of [2-byte length][ITCH message] blocks,
structurally identical to one MoldUDP64 message block minus the
session/sequence/gap machinery that wraps those in a live feed.

Produces the same output shape as moldudp64_deframer.v's message
output, so itch_decoder.v can't tell the two frontends apart. Reuses
that same "pull a length, hold a sideband descriptor until accepted,
stream the body re-aligned to byte 0" logic (moldudp64_deframer.v's
READ_MESSAGE), just promoted to the whole top-level FSM here, looping
forever since nothing bounds how many blocks there are - the file
just ends.

Two signals mean something different here than elsewhere in this
repo, despite sharing names:
  - s_raw_axis_tlast means end of FILE, not end of one block.
  - m_msg_seq_num is a synthetic local block index (0, 1, 2, ... in
    file order), not a real MoldUDP64 sequence number.

m_msg_type is captured from the body's own first byte, same as
moldudp64_deframer.v does.

No IDLE state: pulls length fields immediately and blocks via the
same gb_count/has_eop_buffered checks whenever there's nothing to do
yet. Clean EOF is detected in LENGTH when the buffer is empty and the
input's own eop has arrived; a stray fragment too short for a length
field is error_truncated instead. A truncated body routes through
DRAIN (a copy of moldudp64_deframer.v's WAIT_NEXT) to discard the
dangling remainder before LENGTH looks again.

Settle-cycle discipline applies the same way it does in
moldudp64_deframer.v: gb_rd_len has to drop to 0 the cycle right
after a pull, not one state later, since the gearbox reads it live
every cycle.
---------------------------------
*/

module itch_raw_deframer (
    input wire clk,
    input wire rst,

    // Raw file byte stream in. s_raw_axis_tlast marks EOF, not one block's end.
    input  wire [63:0] s_raw_axis_tdata,
    input  wire [7:0 ] s_raw_axis_tkeep,
    input  wire        s_raw_axis_tvalid,
    output wire        s_raw_axis_tready,
    input  wire        s_raw_axis_tlast,

    // Same shape as moldudp64_deframer.v's message output.
    output reg  [63:0] m_msg_payload_axis_tdata,
    output reg  [7:0 ] m_msg_payload_axis_tkeep,
    output reg         m_msg_payload_axis_tvalid,
    input  wire        m_msg_payload_axis_tready,
    output reg         m_msg_payload_axis_tlast,

    output reg          m_msg_hdr_valid,
    input  wire         m_msg_hdr_ready,
    output reg  [63:0]  m_msg_seq_num, // synthetic local block index - see doc comment
    output reg  [15:0]  m_msg_length,
    output reg  [7:0 ]  m_msg_type,    // body[0]; valid when length > 0

    output reg  error_truncated,
    output reg  end_of_input,
    output reg  busy
);

    localparam integer GB_BYTES = 16;

    localparam [2:0]
        LENGTH        = 3'd0,
        LENGTH_SETTLE = 3'd1,
        TYPE_PEEK     = 3'd2,
        HDR           = 3'd3,
        BODY          = 3'd4,
        DRAIN         = 3'd5;

    reg [2:0]  state = LENGTH;
    reg [15:0] msgs_bytes_remaining = 0;
    reg [63:0] local_seq = 0;

    reg msg_body_settle = 1'b0; // last BODY pull hasn't landed in gb_data yet

    reg drain_settle       = 1'b0; // last DRAIN pull hasn't landed yet
    reg drain_eop_consumed = 1'b0; // captured at pull time - see moldudp64_deframer.v's WAIT_NEXT note

    // Sticky, OR-accumulated across every pull (LENGTH/BODY/DRAIN): has the
    // file's real EOF byte been consumed yet. Needed because by the time
    // BODY's last pull consumes the final byte, its eop bit has already
    // shifted out of the buffer - a live re-check in LENGTH afterward
    // always reads false. Same bug class as moldudp64_deframer.v's
    // MSG_DONE deadlock; without this, LENGTH never detected EOF after a
    // block streamed to completion and every pipeline test hung.
    reg input_eop_consumed = 1'b0;

    // byte count -> tkeep, local copy like moldudp64_deframer.v keeps its own
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
        .s_axis_tdata  (s_raw_axis_tdata),
        .s_axis_tkeep  (s_raw_axis_tkeep),
        .s_axis_tvalid (s_raw_axis_tvalid),
        .s_axis_tready (s_raw_axis_tready),
        .s_axis_tlast  (s_raw_axis_tlast),
        .s_axis_tuser  (1'b0), // raw files carry no per-byte error marker
        .rd_len            (gb_rd_len),
        .data              (gb_data),
        .eop               (gb_eop),
        .count             (gb_count),
        .has_eop_buffered  (gb_has_eop_buffered)
    );

    // length field, same convention as moldudp64_deframer.v
    wire [15:0] len_val = { gb_data[7:0], gb_data[15:8] };

    // BODY's per-beat pull: min(gb_count, msgs_bytes_remaining, 8).
    wire [4:0] msg_bytes_remaining_capped = (msgs_bytes_remaining > GB_BYTES[15:0]) ? GB_BYTES[4:0] : msgs_bytes_remaining[4:0];
    wire [4:0] msg_avail                  = (gb_count < msg_bytes_remaining_capped) ? gb_count : msg_bytes_remaining_capped;
    wire [3:0] msg_pull_len               = (msg_avail > 8) ? 4'd8 : msg_avail[3:0];

    wire msg_would_truncate = (gb_count < msgs_bytes_remaining) && gb_has_eop_buffered;

    // does THIS pull's bytes include the file's eop - captured at issue time
    wire [GB_BYTES-1:0] msg_pull_mask         = ({GB_BYTES{1'b1}} >> (GB_BYTES - msg_pull_len));
    wire                msg_body_pull_has_eop = |(gb_eop & msg_pull_mask);

    // DRAIN's per-cycle discard pull - identical shape to moldudp64_deframer.v's WAIT_NEXT.
    wire [3:0]          drain_pull_len     = (gb_count > 8) ? 4'd8 : gb_count[3:0];
    wire [GB_BYTES-1:0] drain_pull_mask    = ({GB_BYTES{1'b1}} >> (GB_BYTES - drain_pull_len));
    wire                drain_pull_has_eop = |(gb_eop & drain_pull_mask);

    integer mi; // BODY's byte-packing loop variable

    always @(posedge clk) begin

        if (rst) begin

            state                      <= LENGTH;
            msgs_bytes_remaining       <= 16'd0;
            local_seq                  <= 64'd0;
            msg_body_settle            <= 1'b0;
            drain_settle               <= 1'b0;
            drain_eop_consumed         <= 1'b0;
            input_eop_consumed         <= 1'b0;
            gb_rd_len                  <= 4'd0;
            m_msg_payload_axis_tdata   <= 64'd0;
            m_msg_payload_axis_tkeep   <= 8'd0;
            m_msg_payload_axis_tvalid  <= 1'b0;
            m_msg_payload_axis_tlast   <= 1'b0;
            m_msg_hdr_valid            <= 1'b0;
            m_msg_seq_num              <= 64'd0;
            m_msg_length               <= 16'd0;
            m_msg_type                 <= 8'd0;
            error_truncated            <= 1'b0;
            end_of_input                <= 1'b0;
            busy                        <= 1'b0;

        end else begin

            // No IDLE here - the only truly idle moment is LENGTH with an
            // empty buffer and nothing in flight.
            busy <= !(state == LENGTH && gb_count == 5'd0);

            case (state)

                LENGTH: begin

                    gb_rd_len <= 4'd0;

                    if (gb_count >= 2) begin

                        // Set here, not in HDR's accept branch - setting it
                        // on the same edge the accept condition fires is a
                        // race, since a downstream read sees the value from
                        // before that edge, never a same-edge write. Caught
                        // this: with the assignment in HDR, every message
                        // decoded with the PREVIOUS message's seq_num.
                        m_msg_length       <= len_val;
                        m_msg_seq_num      <= local_seq;
                        local_seq          <= local_seq + 64'd1;
                        input_eop_consumed <= input_eop_consumed | (|gb_eop[1:0]);
                        gb_rd_len           <= 4'd2;
                        state                <= LENGTH_SETTLE;

                    end else if (gb_has_eop_buffered || input_eop_consumed) begin
                        if (gb_count == 5'd0) end_of_input   <= 1'b1; // clean EOF between blocks
                        else                  error_truncated <= 1'b1; // stray byte(s), not a full length field
                    end

                end

                LENGTH_SETTLE: begin
                    gb_rd_len <= 4'd0; // settle-cycle: reset happens here, not deferred to TYPE_PEEK
                    state     <= TYPE_PEEK;
                end

                // itch_decoder.v reads m_msg_type the instant it accepts the
                // hdr handshake, before any body streams - so it has to be
                // correct before HDR asserts valid. Peek gb_data[7:0] here
                // without pulling; BODY still does the real pull later.
                // Without this, m_msg_type only updated on BODY's first
                // pull - one state too late.
                TYPE_PEEK: begin

                    gb_rd_len <= 4'd0;

                    if (m_msg_length == 16'd0) begin
                        state <= HDR; // zero-length block: no body, no type byte to peek
                    end else if (gb_count >= 1) begin
                        m_msg_type <= gb_data[7:0];
                        state       <= HDR;
                    end else if (gb_has_eop_buffered) begin
                        // body declared non-zero but file ends first;
                        // BODY's own truncation check catches this.
                        state <= HDR;
                    end

                end

                HDR: begin

                    gb_rd_len <= 4'd0;

                    // Hold-until-accepted, same convention as moldudp64_deframer.v's MSG_HDR.
                    m_msg_hdr_valid <= 1'b1;

                    if (m_msg_hdr_valid && m_msg_hdr_ready) begin

                        m_msg_hdr_valid       <= 1'b0;
                        msgs_bytes_remaining  <= m_msg_length;

                        // zero-length block: no body, no type byte - same skip MSG_HDR does
                        state <= (m_msg_length == 16'd0) ? LENGTH : BODY;

                    end

                end

                BODY: begin

                    gb_rd_len <= 4'd0;

                    // clear tvalid the instant a beat is accepted - same double-accept fix as MSG_BODY
                    if (m_msg_payload_axis_tvalid && m_msg_payload_axis_tready) begin
                        m_msg_payload_axis_tvalid <= 1'b0;
                    end

                    if (msg_body_settle) begin
                        msg_body_settle <= 1'b0;
                    end else if (m_msg_payload_axis_tvalid && !m_msg_payload_axis_tready) begin
                        // presented beat not yet accepted; hold tdata/tvalid steady
                    end else if (msgs_bytes_remaining == 16'd0) begin
                        state <= LENGTH; // this block fully streamed; no settle needed, nothing pulled here
                    end else if (msg_would_truncate) begin
                        error_truncated           <= 1'b1;
                        m_msg_payload_axis_tvalid <= 1'b0;
                        state                      <= DRAIN;
                    end else if (gb_count > 0) begin
                        for (mi = 0; mi < 8; mi = mi + 1) begin
                            if (mi < msg_pull_len) m_msg_payload_axis_tdata[mi*8 +: 8] <= gb_data[mi*8 +: 8];
                            else                    m_msg_payload_axis_tdata[mi*8 +: 8] <= 8'd0;
                        end
                        m_msg_payload_axis_tkeep  <= count2keep(msg_pull_len);
                        m_msg_payload_axis_tvalid <= 1'b1;
                        m_msg_payload_axis_tlast  <= (msgs_bytes_remaining == {12'd0, msg_pull_len});
                        if (msgs_bytes_remaining == m_msg_length) m_msg_type <= gb_data[7:0]; // first body byte
                        msgs_bytes_remaining <= msgs_bytes_remaining - {12'd0, msg_pull_len};
                        input_eop_consumed   <= input_eop_consumed | msg_body_pull_has_eop;
                        gb_rd_len             <= msg_pull_len;
                        msg_body_settle       <= 1'b1;
                    end
                    // else: gb_count==0, not truncated - more input still expected, wait

                end

                DRAIN: begin

                    gb_rd_len <= 4'd0;

                    if (drain_settle) begin

                        drain_settle <= 1'b0;

                        if (drain_eop_consumed) state <= LENGTH;

                    end else if (gb_count > 0) begin

                        gb_rd_len           <= drain_pull_len;
                        drain_eop_consumed  <= drain_pull_has_eop;
                        input_eop_consumed  <= input_eop_consumed | drain_pull_has_eop;
                        drain_settle         <= 1'b1;

                    end

                end

                default: state <= LENGTH;

            endcase

        end

    end

endmodule
