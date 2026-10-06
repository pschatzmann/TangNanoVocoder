// Link protocol between the ESP32 (or a PC) and the vocoder, the same byte
// stream over SPI (vocoder_spi_slave.v) or UART (vocoder_uart.v). Details
// and the host side: docs/gateware.md, "Link".
//
// Host -> FPGA:
//   'P' (0x50), u32 byte count (multiple of 4), the program image
//       (vocoder_model --export-hw), checksum: written to SDRAM from word 0.
//       Only while nothing is being computed or played.
//   'S' (0x53), u16 frames, frames x 32 int16 (frame by frame, channel
//       0..31, the image's z scale), checksum: one sentence, written to the
//       free latent slot. At most the image's max_frames frames.
//   '?' (0x3F): status reply.   'T' (0x54): status + statistics reply.
//   'D' (0x44): debug reply (scheduler state, header as read; vocoder_system.v).
//   'L' (0x4C), u8: SDRAM read sample point (sdram_ctrl.v rd_lat: 0-2, +4 half a clock earlier).
//   'R' (0x52), u32 word address, u16 word count: read SDRAM back (for
//       example a sentence's PCM), replied as count x 4 bytes.
//   Checksum = sum of the payload bytes mod 256. Other bytes are ignored.
//
// FPGA -> host: status byte 0x80 | flags
//   bit0 READY      a sentence may be sent now
//   bit1 RECEIVING  inside a packet
//   bit2 BUSY       computing or playing
//   bit3 ERROR      last packet dropped (bad checksum, too long, not ready);
//                   cleared by the next 'P' or 'S'
//   bit4 PROGRAM    a program is loaded
// Replies ('?', 'T') are produced by vocoder_system.v.
//
// SDRAM writes go out through a small FIFO as single-word writes on a
// sdram_ctrl B-style port; `sent`/`prog_done` pulse only once the last one
// is written.
`timescale 1ns / 1ps

module vocoder_link (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        rx_valid,
    input  wire [7:0]  rx_data,

    // layout, from the loaded image's header
    input  wire        hdr_valid,
    input  wire [12:0] max_frames,
    input  wire [20:0] lat_base0, lat_base1, lat_plane,
    input  wire        slot_free,       // the latent slot `wslot` can take a sentence
    input  wire        busy,            // computing / playing
    output reg         wslot,           // latent slot the next sentence goes to

    output reg         sent,            // pulse: a sentence is complete in slot `sent_slot`
    output reg         sent_slot,
    output reg  [12:0] sent_frames,
    output reg         prog_done,       // pulse: a program was uploaded (load the header)
    output reg         prog_valid,
    output wire [7:0]  status,
    output reg         query,           // pulse: '?'
    output reg         query_stats,     // pulse: 'T'
    output reg         query_debug,     // pulse: 'D'
    output reg         lat_set,         // pulse: 'L', value in lat_value
    output reg  [2:0]  lat_value,
    output reg         readback,        // pulse: 'R'
    output reg  [20:0] rb_addr,
    output reg  [15:0] rb_count,

    output wire        m_req,
    output wire        m_we,
    output wire [20:0] m_addr,
    output wire [8:0]  m_len,
    input  wire        m_ack,
    input  wire        m_wpull,
    output wire [31:0] m_wdata,
    output wire [3:0]  m_wbe,
    input  wire        m_done
);
  localparam S_IDLE = 4'd0, S_LEN = 4'd1, S_PDATA = 4'd2, S_LO = 4'd3, S_HI = 4'd4, S_SUM = 4'd5,
             S_FLUSH = 4'd6, S_RARG = 4'd7, S_LARG = 4'd8;
  reg [2:0]  rarg_i;
  reg [47:0] rarg;
  reg [3:0]  state;
  reg        is_prog;
  reg [1:0]  len_i;     // length byte index
  reg [31:0] len;       // 'P': bytes, 'S': frames
  reg [7:0]  sum, lo;
  reg [23:0] pword;     // 'P': bytes of the word being assembled
  reg [1:0]  pbyte;
  reg [31:0] left;      // bytes ('P') or values ('S') still to come
  reg [20:0] waddr;     // 'P': next word address
  reg        drop, error;
  reg [4:0]  ch;        // 'S': channel of the next value
  reg [12:0] frame;
  reg [20:0] ch_addr;   // word address of (frame, ch)
  wire [20:0] slot_base = wslot ? lat_base1 : lat_base0;

  wire ready = prog_valid && hdr_valid && slot_free && state == S_IDLE;
  assign status = {3'b100, prog_valid, error, busy, state != S_IDLE, ready};

  // ---- write FIFO: {addr, data, byte enables}
  reg [56:0] wf [0:15];
  reg [4:0]  wf_wp, wf_rp;
  wire       wf_empty = wf_wp == wf_rp;
  reg        wf_push;
  reg [56:0] wf_in;
  wire [56:0] wf_head = wf[wf_rp[3:0]];
  reg        wr_active;
  assign m_req   = !wf_empty && !wr_active;
  assign m_we    = 1'b1;
  assign m_addr  = wf_head[56:36];
  assign m_len   = 9'd1;
  assign m_wdata = wf_head[35:4];
  assign m_wbe   = wf_head[3:0];
  always @(posedge clk) if (wf_push) wf[wf_wp[3:0]] <= wf_in;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state <= S_IDLE;
      error <= 1'b0;
      prog_valid <= 1'b0;
      wslot <= 1'b0;
      sent <= 1'b0;
      prog_done <= 1'b0;
      query <= 1'b0;
      query_stats <= 1'b0;
      readback <= 1'b0;
      query_debug <= 1'b0;
      lat_set <= 1'b0;
      wf_wp <= 5'd0;
      wf_rp <= 5'd0;
      wf_push <= 1'b0;
      wr_active <= 1'b0;
    end else begin
      sent <= 1'b0;
      prog_done <= 1'b0;
      query <= 1'b0;
      query_stats <= 1'b0;
      readback <= 1'b0;
      query_debug <= 1'b0;
      lat_set <= 1'b0;
      wf_push <= 1'b0;
      if (wf_push) wf_wp <= wf_wp + 5'd1;

      // SDRAM side: one word per request, popped when written
      if (m_ack) wr_active <= 1'b1;
      if (m_done) begin
        wr_active <= 1'b0;
        wf_rp <= wf_rp + 5'd1;
      end

      if (state == S_FLUSH) begin
        if (wf_empty && !wf_push && !wr_active) begin
          state <= S_IDLE;
          if (is_prog) begin
            prog_valid <= 1'b1;
            prog_done <= 1'b1;
          end else begin
            sent <= 1'b1;
            sent_slot <= wslot;
            sent_frames <= len[12:0];
            wslot <= !wslot;
          end
        end
      end else if (rx_valid) begin
        case (state)
          S_IDLE: begin
            if (rx_data == 8'h50 || rx_data == 8'h53) begin
              is_prog <= rx_data == 8'h50;
              // a program only while idle; a sentence only when ready
              drop <= rx_data == 8'h50 ? busy : !ready;
              error <= 1'b0;
              len_i <= 2'd0;
              len <= 32'd0;
              state <= S_LEN;
            end
            if (rx_data == 8'h3F) query <= 1'b1;
            if (rx_data == 8'h54) query_stats <= 1'b1;
            if (rx_data == 8'h44) query_debug <= 1'b1;
            if (rx_data == 8'h4C) state <= S_LARG;
            if (rx_data == 8'h52) begin
              rarg_i <= 3'd0;
              state <= S_RARG;
            end
          end
          S_LARG: begin
            lat_value <= rx_data[2:0];
            lat_set <= 1'b1;
            state <= S_IDLE;
          end
          S_RARG: begin
            rarg[{rarg_i, 3'b000} +: 8] <= rx_data;
            rarg_i <= rarg_i + 3'd1;
            if (rarg_i == 3'd5) begin
              rb_addr <= rarg[20:0];
              rb_count <= {rx_data, rarg[39:32]};
              readback <= 1'b1;
              state <= S_IDLE;
            end
          end
          S_LEN: begin
            len[{len_i, 3'b000} +: 8] <= rx_data;
            len_i <= len_i + 2'd1;
            sum <= 8'd0;
            if (is_prog && len_i == 2'd3) begin
              left <= {rx_data, len[23:0]};
              waddr <= 21'd0;
              pbyte <= 2'd0;
              if (!drop) prog_valid <= 1'b0;  // being overwritten
              state <= {rx_data, len[23:0]} == 32'd0 ? S_SUM : S_PDATA;
            end else if (!is_prog && len_i == 2'd1) begin
              left <= {11'd0, rx_data, len[7:0], 5'd0};  // frames x 32 values
              if ({rx_data, len[7:0]} > {3'd0, max_frames} || {rx_data, len[7:0]} == 16'd0) drop <= 1'b1;
              ch <= 5'd0;
              frame <= 13'd0;
              ch_addr <= slot_base;
              state <= {rx_data, len[7:0]} == 16'd0 ? S_SUM : S_LO;
            end
          end
          S_PDATA: begin
            sum <= sum + rx_data;
            left <= left - 32'd1;
            pbyte <= pbyte + 2'd1;
            if (pbyte == 2'd3) begin
              wf_in <= {waddr, rx_data, pword, 4'b1111};
              wf_push <= !drop;
              waddr <= waddr + 21'd1;
            end else begin
              pword[{pbyte[1:0], 3'b000} +: 8] <= rx_data;
            end
            if (left == 32'd1) state <= S_SUM;
          end
          S_LO: begin
            lo <= rx_data;
            sum <= sum + rx_data;
            state <= S_HI;
          end
          S_HI: begin
            sum <= sum + rx_data;
            wf_in <= {ch_addr + {8'd0, frame[12:1]}, rx_data, lo, rx_data, lo, frame[0] ? 4'b1100 : 4'b0011};
            wf_push <= !drop;
            left <= left - 32'd1;
            ch <= ch + 5'd1;
            if (ch == 5'd31) begin
              frame <= frame + 13'd1;
              ch_addr <= slot_base;
            end else begin
              ch_addr <= ch_addr + lat_plane;
            end
            state <= left == 32'd1 ? S_SUM : S_LO;
          end
          S_SUM: begin
            if (drop || rx_data != sum) begin
              error <= 1'b1;
              state <= S_IDLE;
            end else begin
              state <= S_FLUSH;
            end
          end
          default: state <= S_IDLE;
        endcase
      end
    end
  end
endmodule
