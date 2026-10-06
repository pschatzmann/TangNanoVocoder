// Post-processing of vocoder_conv_engine's accumulators, one value per
// clock, fully pipelined (no back-pressure: whoever consumes `out_*` must
// take a value every clock, as the engine can deliver one).
//
// Bit-exact with the phase 1 model (model/src/Executors.h, IntExecutor):
//   v = ((acc + bias[co]) * mult[co] + 2^(shift[co]-1)) >> shift[co]
//   conv:      y = clamp(v)
//   residual:  y = clamp(clamp(v) + res)
//   output:    y = tanh_q15(clamp(v)), v being Q12 here (vocoder_tanh.hex)
// clamp = +-32767 (16-bit activations). The rounding shift is computed as
// (p >> s) + bit s-1 of p, which is the same value without a wide adder.
//
// Per-channel parameters come from an external synchronous RAM addressed
// by `prm_co` (data one clock later); the residual arrives with the input
// (`in_res`, the next value of the residual stream).
`timescale 1ns / 1ps

module vocoder_post #(
    parameter TBITS = 20,
    parameter ACCW  = 36,
    parameter TANH_FILE = "vocoder_tanh.hex"
) (
    input  wire                    clk,
    input  wire                    rst_n,

    input  wire                    cfg_residual,
    input  wire                    cfg_output,     // conv_post: tanh to PCM

    input  wire                    in_valid,
    input  wire [6:0]              in_co,
    input  wire [TBITS-1:0]        in_t,
    input  wire signed [ACCW-1:0]  in_acc,
    input  wire signed [15:0]      in_res,

    // per output channel parameters, 1-clock read latency
    output wire [6:0]              prm_co,
    input  wire signed [31:0]      prm_bias,
    input  wire [15:0]             prm_mult,
    input  wire [5:0]              prm_shift,      // must be 15..32 (the image export checks)

    output reg                     out_valid,
    output reg  [6:0]              out_co,
    output reg  [TBITS-1:0]        out_t,
    output reg  signed [15:0]      out_y
);

  localparam PRODW = 52;  // 36-bit sum x 16-bit multiplier

  assign prm_co = in_co;

  reg [15:0] tanh_lut [0:256];
  initial $readmemh(TANH_FILE, tanh_lut);

  // P1: parameters arrive
  reg                    p1_valid;
  reg [6:0]              p1_co;
  reg [TBITS-1:0]        p1_t;
  reg signed [ACCW-1:0]  p1_acc;
  reg signed [15:0]      p1_res;
  // P2: sum (saturated to 36 bits: the model never comes close)
  reg                    p2_valid;
  reg [6:0]              p2_co;
  reg [TBITS-1:0]        p2_t;
  reg signed [35:0]      p2_sum;
  reg [15:0]             p2_mult;
  reg [5:0]              p2_shift;
  reg signed [15:0]      p2_res;
  // P3: product
  reg                    p3_valid;
  reg [6:0]              p3_co;
  reg [TBITS-1:0]        p3_t;
  reg signed [PRODW-1:0] p3_prod;
  reg [5:0]              p3_shift;
  reg signed [15:0]      p3_res;
  // P4: requantized and clamped
  reg                    p4_valid;
  reg [6:0]              p4_co;
  reg [TBITS-1:0]        p4_t;
  reg signed [15:0]      p4_y;
  reg signed [15:0]      p4_res;
  // P5: residual added, or tanh table read
  reg                    p5_valid;
  reg [6:0]              p5_co;
  reg [TBITS-1:0]        p5_t;
  reg signed [15:0]      p5_y;
  reg                    p5_neg;
  reg [6:0]              p5_frac;
  reg signed [16:0]      p5_lut0, p5_lut1;

  wire signed [ACCW:0] p1_sum = p1_acc + prm_bias;
  wire signed [35:0] p1_sum_sat = p1_sum > 37'sh7_FFFF_FFFF ? 36'sh7_FFFF_FFFF :
                                  p1_sum < -37'sh8_0000_0000 ? -36'sh8_0000_0000 : p1_sum[35:0];

  wire signed [71:0] prod;
  vocoder_mul36 u_mul (.clk(clk), .a(p2_sum), .b({20'd0, p2_mult}), .p(prod));

  // q = prod >> (shift - 1) as a fixed shift by 14 and a variable one of
  // 0..17; then v = (q >> 1) + q[0]
  wire signed [PRODW-15:0] p3_pre = p3_prod >>> 14;
  wire signed [PRODW-15:0] p3_q   = p3_pre >>> (p3_shift - 6'd15);
  wire signed [PRODW-15:0] p3_v   = (p3_q >>> 1) + $signed({1'b0, p3_q[0]});
  wire signed [15:0]      p3_y       = p3_v > 32767 ? 16'sd32767 : (p3_v < -32767 ? -16'sd32767 : p3_v[15:0]);

  wire signed [16:0] p4_with_res = p4_y + p4_res;
  wire signed [15:0] p4_res_y = p4_with_res > 32767 ? 16'sd32767 :
                                (p4_with_res < -32767 ? -16'sd32767 : p4_with_res[15:0]);
  wire [15:0] p4_abs = p4_y[15] ? -p4_y : p4_y;
  wire signed [24:0] p5_step = (p5_lut1 - p5_lut0) * $signed({1'b0, p5_frac});
  wire signed [16:0] p5_tanh = p5_lut0 + ((p5_step + 25'sd64) >>> 7);

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      p1_valid <= 1'b0;
      p2_valid <= 1'b0;
      p3_valid <= 1'b0;
      p4_valid <= 1'b0;
      p5_valid <= 1'b0;
      out_valid <= 1'b0;
    end else begin
      p1_valid <= in_valid;
      p1_co <= in_co;
      p1_t <= in_t;
      p1_acc <= in_acc;
      p1_res <= cfg_residual ? in_res : 16'sd0;

      p2_valid <= p1_valid;
      p2_co <= p1_co;
      p2_t <= p1_t;
      p2_sum <= p1_sum_sat;
      p2_mult <= prm_mult;
      p2_shift <= prm_shift;
      p2_res <= p1_res;

      p3_valid <= p2_valid;
      p3_co <= p2_co;
      p3_t <= p2_t;
      p3_prod <= prod[PRODW-1:0];
      p3_shift <= p2_shift;
      p3_res <= p2_res;

      p4_valid <= p3_valid;
      p4_co <= p3_co;
      p4_t <= p3_t;
      p4_y <= p3_y;
      p4_res <= p3_res;

      p5_valid <= p4_valid;
      p5_co <= p4_co;
      p5_t <= p4_t;
      p5_y <= p4_res_y;
      p5_neg <= p4_y[15];
      p5_frac <= p4_abs[6:0];
      p5_lut0 <= {1'b0, tanh_lut[p4_abs[15:7]]};
      p5_lut1 <= {1'b0, tanh_lut[p4_abs[15:7] + 9'd1]};

      out_valid <= p5_valid;
      out_co <= p5_co;
      out_t <= p5_t;
      out_y <= cfg_output ? (p5_neg ? -p5_tanh[15:0] : p5_tanh[15:0]) : p5_y;
    end
  end

endmodule
