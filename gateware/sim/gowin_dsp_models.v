// Behavioural models of the Gowin DSP primitives vocoder_mul.v uses, with
// their internal registers off (as instantiated there), for gate-level
// simulation. yosys ships them only as blackboxes. Both checked on hardware
// with gateware/test/dsp_test_top.v.
`timescale 1ns / 1ps
module MULT18X18 (input [17:0] A, SIA, input [17:0] B, SIB, input ASIGN, BSIGN, ASEL, BSEL, CE, CLK, RESET,
                  output [35:0] DOUT, output [17:0] SOA, SOB);
  parameter AREG = 1'b0, BREG = 1'b0, OUT_REG = 1'b0, PIPE_REG = 1'b0, ASIGN_REG = 1'b0, BSIGN_REG = 1'b0,
            SOA_REG = 1'b0, MULT_RESET_MODE = "SYNC";
  assign DOUT = $signed({ASIGN & A[17], A}) * $signed({BSIGN & B[17], B});
  assign SOA = A;
  assign SOB = B;
endmodule
module MULT36X36 (input [35:0] A, input [35:0] B, input ASIGN, BSIGN, CE, CLK, RESET, output [71:0] DOUT);
  parameter AREG = 1'b0, BREG = 1'b0, OUT0_REG = 1'b0, OUT1_REG = 1'b0, PIPE_REG = 1'b0, ASIGN_REG = 1'b0,
            BSIGN_REG = 1'b0, MULT_RESET_MODE = "SYNC";
  assign DOUT = $signed({ASIGN & A[35], A}) * $signed({BSIGN & B[35], B});
endmodule
