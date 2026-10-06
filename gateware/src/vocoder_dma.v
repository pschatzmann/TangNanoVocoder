// SDRAM transfers for vocoder_core: `nseg` segments of `words` 32-bit words,
// the first at `first`, each `stride` words after the previous one (for
// example one segment per channel plane). Each segment is split into
// bursts that end at SDRAM row boundaries (256 words) and are at most
// `max_burst` long, issued one at a time on a sdram_ctrl B-style port.
//
// Reads: data comes out on the port's rvalid/rdata, in order; the consumer
// counts words. With `throttle`, a read burst is only issued when `room`
// (free space in the consumer, in words) holds all of it.
// Writes: the port pulls data with wpull; the source must present the next
// word combinationally.
`timescale 1ns / 1ps

module vocoder_dma (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,
    input  wire        we,
    input  wire [20:0] first,
    input  wire [20:0] stride,
    input  wire [7:0]  nseg,
    input  wire [15:0] words,       // per segment, >= 1
    input  wire [8:0]  max_burst,   // 1..256
    input  wire        throttle,
    input  wire [15:0] room,
    output reg         busy,
    output reg  [20:0] seg_base,    // address of the current (after done: last) segment

    output reg         m_req,
    output reg         m_we,
    output reg  [20:0] m_addr,
    output reg  [8:0]  m_len,
    input  wire        m_ack,
    input  wire        m_done
);
  localparam S_IDLE = 2'd0, S_ISSUE = 2'd1, S_ACK = 2'd2, S_DONE = 2'd3;
  reg [1:0]  state;
  reg [20:0] addr;
  reg [15:0] left;      // words left in this segment
  reg [7:0]  segs;      // segments left, this one included
  reg        cfg_we, cfg_throttle;
  reg [20:0] cfg_stride;
  reg [15:0] cfg_words;
  reg [8:0]  cfg_max;

  wire [8:0] to_row = 9'd256 - {1'b0, addr[7:0]};
  wire [15:0] lim1 = left < {7'd0, to_row} ? left : {7'd0, to_row};
  wire [8:0] len = lim1 < {7'd0, cfg_max} ? lim1[8:0] : cfg_max;
  wire room_ok = !cfg_throttle || room >= {7'd0, len};

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state <= S_IDLE;
      busy <= 1'b0;
      m_req <= 1'b0;
    end else begin
      case (state)
        S_IDLE: if (start) begin
          busy <= 1'b1;
          cfg_we <= we;
          cfg_throttle <= throttle;
          cfg_stride <= stride;
          cfg_words <= words;
          cfg_max <= max_burst;
          addr <= first;
          seg_base <= first;
          left <= words;
          segs <= nseg;
          // nothing to move: done (a 0-word burst would wrap the controller's counter)
          if (nseg == 0 || words == 0) segs <= 8'd0;
          state <= (nseg == 0 || words == 0) ? S_DONE : S_ISSUE;
        end
        S_ISSUE: if (room_ok) begin
          m_req <= 1'b1;
          m_we <= cfg_we;
          m_addr <= addr;
          m_len <= len;
          addr <= addr + len;
          left <= left - len;
          state <= S_ACK;
        end
        S_ACK: if (m_ack) begin
          m_req <= 1'b0;
          state <= S_DONE;
        end
        S_DONE: if (m_done || segs == 0) begin
          if (segs == 0) begin
            busy <= 1'b0;
            state <= S_IDLE;
          end else if (left != 0) begin
            state <= S_ISSUE;
          end else if (segs == 8'd1) begin
            busy <= 1'b0;
            state <= S_IDLE;
          end else begin
            segs <= segs - 8'd1;
            seg_base <= seg_base + cfg_stride;
            addr <= seg_base + cfg_stride;
            left <= cfg_words;
            state <= S_ISSUE;
          end
        end
      endcase
    end
  end
endmodule
