// vocoder_flow_unit against the model (vocoder_model --flow --flow-dump):
// the first LayerNorm and the first attention (both heads) of a sentence,
// with the inputs placed in the banks and the weight RAM exactly as
// vocoder_core does. Every output is compared.
//   run_flow_unit_test.sh
`timescale 1ns / 1ps

module tb_flow_unit;
  reg clk = 1'b0;
  always #9.259 clk = ~clk;
  reg rst_n = 1'b0;

  // ---- memories
  reg              wr_en = 1'b0;
  reg [3:0]        wr_bank;
  reg [9:0]        wr_addr;
  reg [15:0]       wr_data;
  wire             bank_en;
  wire [9:0]       bank_addr;
  wire [16*16-1:0] bank_data;
  vocoder_act_banks #(.LANES(16), .ABITS(10)) u_banks (
      .clk(clk), .wr_en(wr_en), .wr_bank(wr_bank), .wr_addr(wr_addr), .wr_data(wr_data),
      .wr2_en(1'b0), .wr2_bank(4'd0), .wr2_addr(10'd0), .wr2_data(16'd0),
      .rd_en(bank_en), .rd_addr({16{bank_addr}}), .rd_data(bank_data));

  reg [15:0] w_even [0:4095];
  reg [15:0] w_odd [0:4095];
  wire       q_en;
  wire [11:0] q_addr;
  reg [15:0] q_even, q_odd;
  always @(posedge clk) if (q_en) begin
    q_even <= w_even[q_addr];
    q_odd <= w_odd[q_addr];
  end

  reg signed [31:0] g_ram [0:31];
  reg signed [31:0] b_ram [0:31];
  wire [6:0] prm_c;
  reg signed [31:0] prm_g, prm_b;
  always @(posedge clk) begin
    prm_g <= g_ram[prm_c[4:0]];
    prm_b <= b_ram[prm_c[4:0]];
  end

  // ---- unit
  reg        ln_start = 1'b0, att_start = 1'b0;
  wire       busy;
  reg [8:0]  frames;
  reg [31:0] eps;
  reg [5:0]  gs;
  reg [15:0] score_mult, merge_mult;
  reg [7:0]  score_shift, merge_shift;
  wire       out_valid;
  wire [5:0] out_c;
  wire [8:0] out_t;
  wire signed [15:0] out_y;

  vocoder_flow_unit #(.EXP_FILE(`EXP_FILE), .RSQRT_FILE(`RSQRT_FILE)) dut (
      .clk(clk), .rst_n(rst_n), .ln_start(ln_start), .att_start(att_start), .busy(busy),
      .ln_frames(frames), .ln_eps(eps), .ln_gs(gs), .prm_c(prm_c), .prm_g(prm_g), .prm_b(prm_b),
      .att_frames(frames), .score_mult(score_mult), .score_shift(score_shift), .merge_mult(merge_mult),
      .merge_shift(merge_shift),
      .bank_en(bank_en), .bank_addr(bank_addr), .bank_data(bank_data),
      .q_en(q_en), .q_addr(q_addr), .q_even(q_even), .q_odd(q_odd),
      .out_valid(out_valid), .out_c(out_c), .out_t(out_t), .out_y(out_y));

  // ---- golden data
  reg [15:0] ln_in [0:16383];
  reg [15:0] ln_out [0:16383];
  reg [31:0] ln_p [0:66];
  reg [15:0] aq [0:16383];
  reg [15:0] ak [0:16383];
  reg [15:0] av [0:16383];
  reg [15:0] aout [0:16383];
  reg [31:0] ap [0:292];

  integer T, errors = 0, outputs = 0, head, i, c, t;
  reg [15:0] expect_v;
  reg checking_ln;
  always @(posedge clk) if (out_valid) begin
    outputs = outputs + 1;
    if (checking_ln) expect_v = ln_out[out_t * 32 + out_c];
    else expect_v = aout[out_t * 32 + head * 16 + out_c];
    if (out_y !== $signed(expect_v)) begin
      if (errors < 10)
        $display("  %0s frame %0d channel %0d: %0d, expected %0d", checking_ln ? "LayerNorm" : "attention", out_t,
                 out_c, out_y, $signed(expect_v));
      errors = errors + 1;
    end
  end

  task bank_write(input [3:0] b, input [9:0] a, input [15:0] d);
    begin
      @(negedge clk);
      wr_en = 1'b1;
      wr_bank = b;
      wr_addr = a;
      wr_data = d;
      @(negedge clk);
      wr_en = 1'b0;
    end
  endtask

  initial begin
    $readmemh("build/flow/ln_in.hex", ln_in);
    $readmemh("build/flow/ln_out.hex", ln_out);
    $readmemh("build/flow/ln_params.hex", ln_p);
    $readmemh("build/flow/att_q.hex", aq);
    $readmemh("build/flow/att_k.hex", ak);
    $readmemh("build/flow/att_v.hex", av);
    $readmemh("build/flow/att_out.hex", aout);
    $readmemh("build/flow/att_params.hex", ap);
    T = ln_p[0];
    repeat (5) @(posedge clk);
    rst_n = 1'b1;

    // ---------------- LayerNorm: frame l in bank l % 16, channel c at (l / 16) * 32 + c
    checking_ln = 1'b1;
    for (t = 0; t < T; t = t + 1)
      for (c = 0; c < 32; c = c + 1) bank_write(t % 16, (t / 16) * 32 + c, ln_in[t * 32 + c]);
    for (c = 0; c < 32; c = c + 1) begin
      g_ram[c] = ln_p[3 + c];
      b_ram[c] = ln_p[35 + c];
    end
    frames = T;
    eps = ln_p[1];
    gs = ln_p[2];
    @(negedge clk);
    ln_start = 1'b1;
    @(negedge clk);
    ln_start = 1'b0;
    @(negedge clk);
    while (busy) @(negedge clk);
    repeat (5) @(negedge clk);
    $display("LayerNorm: %0d outputs, %0d errors", outputs, errors);
    if (outputs != T * 32) begin
      $display("  expected %0d outputs", T * 32);
      errors = errors + 1;
    end

    // ---------------- attention, per head
    checking_ln = 1'b0;
    score_mult = ap[1];
    score_shift = ap[2];
    merge_mult = ap[3];
    merge_shift = ap[4];
    // relative tables: row d, channel c in bank c at 496 + d (k) and 1008 + d (v)
    for (i = 0; i < 144; i = i + 1) begin
      bank_write(i % 16, 496 + i / 16, ap[5 + i]);
      bank_write(i % 16, 1008 + i / 16, ap[5 + 144 + i]);
    end
    for (head = 0; head < 2; head = head + 1) begin
      outputs = 0;
      for (t = 0; t < T; t = t + 1)
        for (c = 0; c < 16; c = c + 1) begin
          // k at t, v at 512 + t, channel c in bank (c + 8 (t & 1)) % 16
          bank_write((c + 8 * (t % 2)) % 16, t, ak[t * 32 + head * 16 + c]);
          bank_write((c + 8 * (t % 2)) % 16, 512 + t, av[t * 32 + head * 16 + c]);
          // q_t[c] in bank (t + c) & 1 at word t * 8 + c / 2
          if ((t + c) % 2) w_odd[t * 8 + c / 2] = aq[t * 32 + head * 16 + c];
          else w_even[t * 8 + c / 2] = aq[t * 32 + head * 16 + c];
        end
      @(negedge clk);
      att_start = 1'b1;
      @(negedge clk);
      att_start = 1'b0;
      @(negedge clk);
      while (busy) @(negedge clk);
      repeat (5) @(negedge clk);
      $display("attention head %0d: %0d outputs, %0d errors so far", head, outputs, errors);
      if (outputs != T * 16) begin
        $display("  expected %0d outputs", T * 16);
        errors = errors + 1;
      end
    end
    $display("%s: %0d errors", errors == 0 ? "PASS" : "FAIL", errors);
    $finish;
  end
endmodule
