// Vendored unchanged from TangNanoGPU (gateware/tb/sdram_model.v, same author, Apache-2.0).
`timescale 1ns / 1ps
//
// Behavioural SDR SDRAM model for simulation (64Mbit, 4 banks x 2K rows x
// 256 columns x 32 bit), sufficient to verify sdram_ctrl.v:
//   - commands sampled on SDRAM_CLK's rising edge
//   - per-bank open-row tracking; READ/WRITE to a closed bank, or ACTIVATE
//     on an open bank, is reported as a protocol error
//   - auto-precharge via A10 on READ/WRITE, PRECHARGE (A10 = all banks)
//   - burst length 1, CAS latency CAS: read data is driven for one clock
//     starting CAS rising edges after the READ, so back-to-back READs
//     stream one word per clock
//   - DQM byte masking on writes (reads always drive all lanes)
//
// The memory array is sparse-ish: only MEM_WORDS words are stored, and
// higher addresses alias. Testbenches only touch a few rows per bank.
//
module sdram_model #(
    parameter integer CAS       = 2,
    parameter integer MEM_AW    = 21    // full 2M-word array by default
) (
    inout  wire [31:0] SDRAM_DQ,
    input  wire [10:0] SDRAM_A,
    input  wire [1:0]  SDRAM_BA,
    input  wire        SDRAM_nCS,
    input  wire        SDRAM_nWE,
    input  wire        SDRAM_nRAS,
    input  wire        SDRAM_nCAS,
    input  wire        SDRAM_CLK,
    input  wire        SDRAM_CKE,
    input  wire [3:0]  SDRAM_DQM
);

  reg [31:0] mem [0:(1<<MEM_AW)-1];

  reg        open_q [0:3];
  reg [10:0] row_q  [0:3];

  // read pipeline: slot i holds data to drive i clocks from now
  reg [31:0] rd_data [0:7];
  reg        rd_v    [0:7];

  reg [31:0] dq_drv;
  reg        dq_en;
  assign SDRAM_DQ = dq_en ? dq_drv : 32'bz;

  integer errors = 0;
  integer reads = 0, writes = 0, acts = 0, refs = 0;
  integer i;

  initial begin
    for (i = 0; i < 4; i = i + 1) begin
      open_q[i] = 1'b0;
      row_q[i]  = 11'd0;
    end
    for (i = 0; i < 8; i = i + 1) begin
      rd_v[i]    = 1'b0;
      rd_data[i] = 32'd0;
    end
    dq_en  = 1'b0;
    dq_drv = 32'd0;
  end

  wire [2:0] cmd = {SDRAM_nRAS, SDRAM_nCAS, SDRAM_nWE};
  wire [20:0] waddr = {SDRAM_BA, row_q[SDRAM_BA], SDRAM_A[7:0]};

  always @(posedge SDRAM_CLK) begin
    // advance the read pipeline
    for (i = 0; i < 7; i = i + 1) begin
      rd_v[i]    <= rd_v[i+1];
      rd_data[i] <= rd_data[i+1];
    end
    rd_v[7] <= 1'b0;
    // output: what was scheduled for "now"
    dq_en  <= #1 rd_v[0];
    dq_drv <= #1 rd_data[0];

    if (!SDRAM_nCS && SDRAM_CKE) begin
      case (cmd)
        3'b011: begin // ACTIVATE
          if (open_q[SDRAM_BA]) begin
            $display("%t SDRAM ERROR: ACTIVATE on open bank %0d", $time, SDRAM_BA);
            errors = errors + 1;
          end
          open_q[SDRAM_BA] <= 1'b1;
          row_q[SDRAM_BA]  <= SDRAM_A;
          acts = acts + 1;
        end
        3'b101: begin // READ
          if (!open_q[SDRAM_BA]) begin
            $display("%t SDRAM ERROR: READ on closed bank %0d", $time, SDRAM_BA);
            errors = errors + 1;
          end
          // CAS-1 because slot 0 is driven at the *next* edge
          rd_v[CAS-1]    <= 1'b1;
          rd_data[CAS-1] <= mem[waddr[MEM_AW-1:0]];
          if (SDRAM_A[10]) open_q[SDRAM_BA] <= 1'b0;
          reads = reads + 1;
        end
        3'b100: begin // WRITE
          if (!open_q[SDRAM_BA]) begin
            $display("%t SDRAM ERROR: WRITE on closed bank %0d", $time, SDRAM_BA);
            errors = errors + 1;
          end
          if (!SDRAM_DQM[0]) mem[waddr[MEM_AW-1:0]][7:0]   <= SDRAM_DQ[7:0];
          if (!SDRAM_DQM[1]) mem[waddr[MEM_AW-1:0]][15:8]  <= SDRAM_DQ[15:8];
          if (!SDRAM_DQM[2]) mem[waddr[MEM_AW-1:0]][23:16] <= SDRAM_DQ[23:16];
          if (!SDRAM_DQM[3]) mem[waddr[MEM_AW-1:0]][31:24] <= SDRAM_DQ[31:24];
          if (SDRAM_A[10]) open_q[SDRAM_BA] <= 1'b0;
          writes = writes + 1;
        end
        3'b010: begin // PRECHARGE
          if (SDRAM_A[10]) begin
            for (i = 0; i < 4; i = i + 1) open_q[i] <= 1'b0;
          end else begin
            open_q[SDRAM_BA] <= 1'b0;
          end
        end
        3'b001: begin // AUTO REFRESH
          for (i = 0; i < 4; i = i + 1)
            if (open_q[i]) begin
              $display("%t SDRAM ERROR: REFRESH with bank %0d open", $time, i);
              errors = errors + 1;
            end
          refs = refs + 1;
        end
        default: ;
      endcase
    end
  end

endmodule
