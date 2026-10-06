// Vendored from TangNanoGPU (gateware/rtl/sdram_ctrl.v, same author, Apache-2.0).
// The vocoder uses port A for audio playback and port B (through
// vocoder_sdram_arb.v) for the scheduler and the link.
// One change: `rd_lat` selects when read data is sampled. Bits 1:0: 1 is
// the original E+CAS+1 below (what the simulation model does), 0 one clock
// earlier, 2 one later; bit 2 takes the data bus as sampled on the falling
// clock edge before that, half a clock earlier. On a Tang Nano 20K at 54
// MHz, bursts read at the whole-clock points came back as bitwise mixes of
// neighbouring words (single reads happened to work): the data's valid
// window lies between them. The board's sample point is found with the
// link's 'L' command (tools/tnv.py calibrate, docs/gateware.md).
`timescale 1ns / 1ps
`default_nettype none
//
// Row-burst SDR SDRAM controller for the Tang Nano 20K's embedded 64Mbit
// (2K rows x 256 columns x 4 banks x 32 bit) SDRAM.
//
// Written for TangNanoGPU (Apache-2.0). The init sequence, command
// encoding and read-sample timing follow nand2mario's sdram-tang-nano-20k
// controller (Apache-2.0, vendored as TangNanoAI's sdram_controller.v),
// which is confirmed on real hardware: a READ registered at clock edge E
// is sampled at edge E+CAS+1, with SDRAM_CLK = the 180-degree-shifted
// clk_sdram.
//
// What is different: instead of one byte per ~5-cycle access, a request is
// a *burst of 1..256 32-bit words inside one SDRAM row*: one ACTIVATE,
// then one READ or WRITE command per clock to consecutive columns (burst
// length 1 in the mode register, so every command is independent), the
// last one with auto-precharge. A whole 320-pixel framebuffer line
// (160 words) therefore streams at one word per clock. Writes carry a
// per-word byte enable (DQM), so pixels are written two at a time and
// single pixels without read-modify-write.
//
// Word address [20:0] = {bank[1:0], row[10:0], column[7:0]}; a burst must
// not cross a row (callers guarantee this - every framebuffer line is one
// row).
//
// Two request ports, arbitrated only when idle (port A wins ties):
//   A - read only  (scanout; must never starve)
//   B - read/write (drawing engine)
// Handshake per port: hold *_req (and addr/len/we) until *_ack pulses.
// For a B write, present word k on b_wdata/b_wbe; it is consumed in every
// cycle b_wpull is high (combinational), after which word k+1 must be
// presented on the next cycle. Read data arrives on rdata with *_rvalid,
// in order. *_done pulses once after the last word (and after the bank is
// precharged again).
//
// Refresh is internal: one AUTO REFRESH every REFRESH_US microseconds,
// issued between bursts. A maximal burst is ~4.2us at 64.8MHz, so
// REFRESH_US=7 keeps every refresh well inside the chip's 15.6us budget.
//
module sdram_ctrl #(
    parameter integer FREQ       = 64_800_000,
    parameter integer CAS        = 2,
    parameter integer T_RCD      = 2,   // ACTIVATE -> READ/WRITE
    parameter integer T_RP       = 2,   // PRECHARGE -> ACTIVATE
    parameter integer T_RC       = 5,   // ACTIVATE/REFRESH -> ACTIVATE/REFRESH
    parameter integer T_WR       = 2,   // last write data -> precharge
    parameter integer T_MRD      = 2,
    parameter integer REFRESH_US = 7,
    parameter integer INIT_US    = 200
) (
    input  wire        clk,
    input  wire        clk_sdram,  // clk shifted by 180 degrees
    input  wire        rst,
    input  wire [2:0]  rd_lat,
    output reg         ready,      // init sequence finished

    // Port A (read only)
    input  wire        a_req,
    input  wire [20:0] a_addr,
    input  wire [8:0]  a_len,
    output reg         a_ack,
    output wire        a_rvalid,
    output reg         a_done,

    // Port B (read / write)
    input  wire        b_req,
    input  wire        b_we,
    input  wire [20:0] b_addr,
    input  wire [8:0]  b_len,
    input  wire [31:0] b_wdata,
    input  wire [3:0]  b_wbe,
    output reg         b_ack,
    output wire        b_wpull,
    output wire        b_rvalid,
    output reg         b_done,

    output reg  [31:0] rdata,

    // SDRAM pins
    inout  wire [31:0] SDRAM_DQ,
    output reg  [10:0] SDRAM_A,
    output reg  [1:0]  SDRAM_BA,
    output wire        SDRAM_nCS,
    output wire        SDRAM_nWE,
    output wire        SDRAM_nRAS,
    output wire        SDRAM_nCAS,
    output wire        SDRAM_CLK,
    output wire        SDRAM_CKE,
    output reg  [3:0]  SDRAM_DQM
);

  // RAS# CAS# WE#
  localparam [2:0] CMD_MRS = 3'b000, CMD_REF = 3'b001, CMD_PRE = 3'b010,
                   CMD_ACT = 3'b011, CMD_WR  = 3'b100, CMD_RD  = 3'b101,
                   CMD_NOP = 3'b111;

  localparam integer REFRESH_CYCLES = FREQ / 1_000_000 * REFRESH_US;
  localparam integer INIT_CYCLES    = FREQ / 1_000_000 * INIT_US;

  localparam [3:0] S_INIT = 4'd0, S_PRE = 4'd1, S_REF1 = 4'd2, S_REF2 = 4'd3,
                   S_MRS = 4'd4, S_IDLE = 4'd5, S_ACT = 4'd6, S_BURST = 4'd7,
                   S_TAIL = 4'd8, S_WAIT = 4'd9;

  reg [3:0]  state;
  reg [3:0]  after_wait;
  reg [15:0] wcnt;
  reg [2:0]  cmd;

  assign {SDRAM_nRAS, SDRAM_nCAS, SDRAM_nWE} = cmd;
  assign SDRAM_nCS = 1'b0;
  assign SDRAM_CKE = 1'b1;
  assign SDRAM_CLK = clk_sdram;

  reg        dq_oe;
  reg [31:0] dq_out;
  assign SDRAM_DQ = dq_oe ? dq_out : 32'bz;
  wire [31:0] dq_in = SDRAM_DQ;

  // ---- refresh timer ----
  reg [15:0] ref_cnt;
  reg        ref_due;

  // ---- current transaction ----
  reg        own_b;   // 0 = port A, 1 = port B
  reg        is_wr;
  reg [7:0]  col;
  reg [8:0]  remain;

  wire in_burst = (state == S_BURST);
  assign b_wpull = in_burst && own_b && is_wr;
  wire   rd_issue = in_burst && !is_wr;

  // Read-data pipeline: a READ registered at edge E is sampled at E+CAS+1.
  reg [CAS+1:0] rp;
  wire rp_hit = rd_lat[1:0] == 2'd0 ? rp[CAS-1] : (rd_lat[1:0] == 2'd2 ? rp[CAS+1] : rp[CAS]);
  reg [31:0] dq_fall;  // the data bus on the falling edge, half a clock before the rising one
  always @(negedge clk) dq_fall <= SDRAM_DQ;
  reg         rvalid;
  always @(posedge clk) begin
    if (rst) begin
      rp     <= {(CAS+2){1'b0}};
      rvalid <= 1'b0;
    end else begin
      rp     <= {rp[CAS:0], rd_issue};
      rvalid <= rp_hit;
      if (rp_hit) rdata <= rd_lat[2] ? dq_fall : dq_in;
    end
  end
  assign a_rvalid = rvalid && !own_b;
  assign b_rvalid = rvalid && own_b;

  // Tail lengths (cycles after the last READ/WRITE command until done/IDLE)
  localparam integer TAIL_RD = CAS + 2 + T_RP;
  localparam integer TAIL_WR = T_WR + T_RP + 1;

  always @(posedge clk) begin
    cmd    <= CMD_NOP;
    a_ack  <= 1'b0;
    b_ack  <= 1'b0;
    a_done <= 1'b0;
    b_done <= 1'b0;

    if (rst) begin
      state     <= S_INIT;
      wcnt      <= INIT_CYCLES[15:0];
      ready     <= 1'b0;
      dq_oe     <= 1'b0;
      SDRAM_DQM <= 4'b1111;
      SDRAM_A   <= 11'd0;
      SDRAM_BA  <= 2'd0;
      ref_cnt   <= 16'd0;
      ref_due   <= 1'b0;
      own_b     <= 1'b0;
      is_wr     <= 1'b0;
      remain    <= 9'd0;
      col       <= 8'd0;
    end else begin
      // refresh timer (only meaningful once initialised)
      if (ref_cnt >= REFRESH_CYCLES[15:0]) begin
        ref_cnt <= 16'd0;
        ref_due <= ready;
      end else begin
        ref_cnt <= ref_cnt + 16'd1;
      end

      case (state)
        S_INIT: begin
          if (wcnt == 16'd0) state <= S_PRE;
          else wcnt <= wcnt - 16'd1;
        end
        S_PRE: begin
          cmd        <= CMD_PRE;
          SDRAM_A    <= 11'b100_0000_0000;  // all banks
          wcnt       <= T_RP[15:0];
          after_wait <= S_REF1;
          state      <= S_WAIT;
        end
        S_REF1: begin
          cmd        <= CMD_REF;
          wcnt       <= T_RC[15:0];
          after_wait <= S_REF2;
          state      <= S_WAIT;
        end
        S_REF2: begin
          cmd        <= CMD_REF;
          wcnt       <= T_RC[15:0];
          after_wait <= S_MRS;
          state      <= S_WAIT;
        end
        S_MRS: begin
          cmd        <= CMD_MRS;
          // burst length 1, sequential, CAS latency, programmed-burst writes
          SDRAM_A    <= {4'b0000, CAS[2:0], 1'b0, 3'b000};
          SDRAM_BA   <= 2'b00;
          wcnt       <= T_MRD[15:0];
          after_wait <= S_IDLE;
          state      <= S_WAIT;
        end
        S_WAIT: begin
          if (wcnt == 16'd0) begin
            state <= after_wait;
            if (after_wait == S_IDLE) ready <= 1'b1;
          end else begin
            wcnt <= wcnt - 16'd1;
          end
        end

        S_IDLE: begin
          SDRAM_DQM <= 4'b1111;
          if (ref_due) begin
            cmd        <= CMD_REF;
            ref_due    <= 1'b0;
            wcnt       <= T_RC[15:0];
            after_wait <= S_IDLE;
            state      <= S_WAIT;
          end else if (a_req || b_req) begin
            own_b    <= !a_req;
            is_wr    <= a_req ? 1'b0 : b_we;
            cmd      <= CMD_ACT;
            SDRAM_BA <= a_req ? a_addr[20:19] : b_addr[20:19];
            SDRAM_A  <= a_req ? a_addr[18:8]  : b_addr[18:8];
            col      <= a_req ? a_addr[7:0]   : b_addr[7:0];
            remain   <= a_req ? a_len         : b_len;
            a_ack    <= a_req;
            b_ack    <= !a_req;
            wcnt     <= T_RCD[15:0] - 16'd1;
            state    <= S_ACT;
          end
        end

        S_ACT: begin
          if (wcnt == 16'd0) state <= S_BURST;
          else wcnt <= wcnt - 16'd1;
        end

        S_BURST: begin
          cmd          <= is_wr ? CMD_WR : CMD_RD;
          SDRAM_A[10]  <= (remain == 9'd1);  // auto-precharge on the last one
          SDRAM_A[9:8] <= 2'b00;
          SDRAM_A[7:0] <= col;
          col          <= col + 8'd1;
          remain       <= remain - 9'd1;
          if (is_wr) begin
            dq_out    <= b_wdata;
            dq_oe     <= 1'b1;
            SDRAM_DQM <= ~b_wbe;
          end else begin
            SDRAM_DQM <= 4'b0000;
          end
          if (remain == 9'd1) begin
            wcnt  <= is_wr ? TAIL_WR[15:0] : TAIL_RD[15:0];
            state <= S_TAIL;
          end
        end

        S_TAIL: begin
          dq_oe     <= 1'b0;
          SDRAM_DQM <= 4'b1111;
          if (wcnt == 16'd0) begin
            a_done <= !own_b;
            b_done <= own_b;
            state  <= S_IDLE;
          end else begin
            wcnt <= wcnt - 16'd1;
          end
        end

        default: state <= S_INIT;
      endcase
    end
  end

endmodule
`default_nettype wire
