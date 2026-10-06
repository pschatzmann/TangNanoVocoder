// Signed multipliers on the GW2AR-18's DSP blocks. yosys has no DSP
// inference for Gowin that this design can rely on, so synthesis
// instantiates the primitives, with their internal registers off (the
// setting arduino-tangnano20k verified on hardware).
//
// The DSPs run UNSIGNED, and the signed product is recovered arithmetically:
// with au = a + 2^(n-1) and bu = b + 2^(n-1) (the sign bit flipped, so
// both are non-negative), a * b = au * bu - 2^(n-1) * (au + bu) + 2^(2n-2),
// exact modulo 2^(2n). Reason: on a Tang Nano 20K, signed mode (ASIGN/BSIGN
// = 1) came out unsigned on 11 of the engine's 16 lane multipliers - it
// depended on where they were placed - while unsigned mode works everywhere.
// Simulation computes the same formula with `*` for the unsigned product, so
// the tests cover the correction too.
`timescale 1ns / 1ps

module vocoder_mul18 (
    input  wire               clk,
    input  wire signed [17:0] a,
    input  wire signed [17:0] b,
    output wire signed [35:0] p   // combinational
);
  wire [17:0] au = {~a[17], a[16:0]};
  wire [17:0] bu = {~b[17], b[16:0]};
  wire [35:0] pu;
`ifdef SYNTHESIS
  MULT18X18 #(
      .AREG(1'b0), .BREG(1'b0), .OUT_REG(1'b0), .PIPE_REG(1'b0),
      .ASIGN_REG(1'b0), .BSIGN_REG(1'b0), .SOA_REG(1'b0)
  ) u_mult (
      .A(au), .B(bu), .SIA(18'd0), .SIB(18'd0), .ASIGN(1'b0), .BSIGN(1'b0), .ASEL(1'b0), .BSEL(1'b0),
      .CE(1'b1), .CLK(clk), .RESET(1'b0), .DOUT(pu), .SOA(), .SOB()
  );
`else
  assign pu = au * bu;
`endif
  wire [18:0] usum = {1'b0, au} + {1'b0, bu};
  assign p = pu - {usum, 17'd0} + (36'd1 << 34);
endmodule

module vocoder_mul36 (
    input  wire               clk,
    input  wire signed [35:0] a,
    input  wire signed [35:0] b,
    output wire signed [71:0] p   // combinational
);
  wire [35:0] au = {~a[35], a[34:0]};
  wire [35:0] bu = {~b[35], b[34:0]};
  wire [71:0] pu;
`ifdef SYNTHESIS
  MULT36X36 #(
      .AREG(1'b0), .BREG(1'b0), .OUT0_REG(1'b0), .OUT1_REG(1'b0), .PIPE_REG(1'b0),
      .ASIGN_REG(1'b0), .BSIGN_REG(1'b0)
  ) u_mult (
      .A(au), .B(bu), .ASIGN(1'b0), .BSIGN(1'b0), .CE(1'b1), .CLK(clk), .RESET(1'b0), .DOUT(pu)
  );
`else
  assign pu = au * bu;
`endif
  wire [36:0] usum = {1'b0, au} + {1'b0, bu};
  assign p = pu - {usum, 35'd0} + (72'd1 << 70);
endmodule
