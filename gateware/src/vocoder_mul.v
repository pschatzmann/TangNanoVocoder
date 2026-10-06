// Signed multipliers on the GW2AR-18's DSP blocks. yosys has no DSP
// inference for Gowin that this design can rely on, so synthesis
// instantiates the primitives, with their internal registers off (the
// setting arduino-tangnano20k verified on hardware).
//
// ASIGN/BSIGN come from a register (0 in the first cycle after
// configuration, then 1), not from a constant: on a Tang Nano 20K, signed
// mode with constant sign inputs came out unsigned on 11 of the engine's 16
// lane multipliers (it depended on where they were placed), while
// register-driven sign inputs work on all of them (test/dsp16_test_top.v).
`timescale 1ns / 1ps

module vocoder_mul18 (
    input  wire               clk,
    input  wire signed [17:0] a,
    input  wire signed [17:0] b,
    output wire signed [35:0] p   // combinational
);
`ifdef SYNTHESIS
  reg sgn = 1'b0;
  always @(posedge clk) sgn <= 1'b1;
  MULT18X18 #(
      .AREG(1'b0), .BREG(1'b0), .OUT_REG(1'b0), .PIPE_REG(1'b0),
      .ASIGN_REG(1'b0), .BSIGN_REG(1'b0), .SOA_REG(1'b0)
  ) u_mult (
      .A(a), .B(b), .SIA(18'd0), .SIB(18'd0), .ASIGN(sgn), .BSIGN(sgn), .ASEL(1'b0), .BSEL(1'b0),
      .CE(1'b1), .CLK(clk), .RESET(1'b0), .DOUT(p), .SOA(), .SOB()
  );
`else
  assign p = a * b;
`endif
endmodule

module vocoder_mul36 (
    input  wire               clk,
    input  wire signed [35:0] a,
    input  wire signed [35:0] b,
    output wire signed [71:0] p   // combinational
);
`ifdef SYNTHESIS
  reg sgn = 1'b0;
  always @(posedge clk) sgn <= 1'b1;
  MULT36X36 #(
      .AREG(1'b0), .BREG(1'b0), .OUT0_REG(1'b0), .OUT1_REG(1'b0), .PIPE_REG(1'b0),
      .ASIGN_REG(1'b0), .BSIGN_REG(1'b0)
  ) u_mult (
      .A(a), .B(b), .ASIGN(sgn), .BSIGN(sgn), .CE(1'b1), .CLK(clk), .RESET(1'b0), .DOUT(p)
  );
`else
  assign p = a * b;
`endif
endmodule
