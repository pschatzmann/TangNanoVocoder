// Layer test: vocoder_conv_engine + vocoder_post against the phase 1 model's
// golden vectors, bit for bit. Test data comes from prep_layer_test.py;
// run_layer_tests.sh runs a set of layers.
//
//   vvp tb_conv_layer.vvp +dir=TESTDIR
//
// cfg.hex: kind, cin_log2, cout, k, dilation, stride_log2, padding, tin,
//          tile_base, t0, count, residual, number of act.hex lines
`timescale 1ns / 1ps

module tb_conv_layer;

  localparam TBITS = 20, ACCW = 36, ABITS = 10, WABITS = 15;

  reg clk = 1'b0;
  always #5 clk = ~clk;
  reg rst_n = 1'b0;

  reg [31:0] cfg [0:12];
  reg [15:0] wmem [0:(1 << WABITS) - 1];
  reg [31:0] bias_mem [0:127];
  reg [15:0] mult_mem [0:127];
  reg [7:0]  shift_mem [0:127];
  reg [15:0] res_mem [0:(1 << 17) - 1];
  reg [15:0] exp_mem [0:(1 << 17) - 1];
  reg        seen [0:(1 << 17) - 1];

  // ---- activation banks and their loader port
  reg              wr_en = 1'b0;
  reg [3:0]        wr_bank;
  reg [ABITS-1:0]  wr_addr;
  reg [15:0]       wr_data;
  wire             act_en;
  wire [16*ABITS-1:0] act_addr;
  wire [16*16-1:0] act_data;

  vocoder_act_banks #(.LANES(16), .ABITS(ABITS)) u_banks (
      .clk(clk), .wr_en(wr_en), .wr_bank(wr_bank), .wr_addr(wr_addr), .wr_data(wr_data),
      .wr2_en(1'b0), .wr2_bank(4'd0), .wr2_addr({ABITS{1'b0}}), .wr2_data(16'd0),
      .rd_en(act_en), .rd_addr(act_addr), .rd_data(act_data));

  // ---- weights
  wire              w_en;
  wire [WABITS-1:0] w_addr;
  reg  [15:0]       w_data;
  always @(posedge clk) if (w_en) w_data <= wmem[w_addr];

  // ---- engine
  reg start = 1'b0;
  wire busy;
  wire res_valid;
  wire [6:0] res_co;
  wire [TBITS-1:0] res_t;
  wire signed [ACCW-1:0] res_acc;

  vocoder_conv_engine #(.ABITS(ABITS), .WABITS(WABITS), .TBITS(TBITS), .ACCW(ACCW)) u_engine (
      .clk(clk), .rst_n(rst_n), .start(start), .busy(busy),
      .cfg_transposed(cfg[0] == 1), .cfg_cin_log2(cfg[1][2:0]), .cfg_co0(7'd0), .cfg_cocount(cfg[2][6:0]),
      .cfg_k(cfg[3][4:0]), .cfg_dil(cfg[4][2:0]), .cfg_stride_log2(cfg[5][2:0]), .cfg_pad(cfg[6][5:0]),
      .cfg_tin(cfg[7][TBITS-1:0]), .cfg_tile_base(cfg[8][TBITS-1:0]), .cfg_t0(cfg[9][TBITS-1:0]),
      .cfg_tcount(cfg[10][TBITS-1:0]),
      .act_en(act_en), .act_addr(act_addr), .act_data(act_data),
      .w_en(w_en), .w_addr(w_addr), .w_data(w_data),
      .res_valid(res_valid), .res_ready(1'b1), .res_co(res_co), .res_t(res_t), .res_acc(res_acc));

  // ---- post
  wire [6:0] prm_co;
  reg signed [31:0] prm_bias;
  reg [15:0] prm_mult;
  reg [5:0]  prm_shift;
  // the residual stream: the value for the engine's current output
  wire signed [15:0] res_data = res_mem[(res_t - cfg[9]) * cfg[2] + res_co];
  wire out_valid;
  wire [6:0] out_co;
  wire [TBITS-1:0] out_t;
  wire signed [15:0] out_y;

  always @(posedge clk) begin
    prm_bias <= bias_mem[prm_co];
    prm_mult <= mult_mem[prm_co];
    prm_shift <= shift_mem[prm_co][5:0];
  end

  vocoder_post #(.TBITS(TBITS), .ACCW(ACCW), .TANH_FILE(`TANH_FILE)) u_post (
      .clk(clk), .rst_n(rst_n), .cfg_residual(cfg[11][0]), .cfg_output(cfg[0] == 3),
      .in_valid(res_valid), .in_co(res_co), .in_t(res_t), .in_acc(res_acc), .in_res(res_data),
      .prm_co(prm_co), .prm_bias(prm_bias), .prm_mult(prm_mult), .prm_shift(prm_shift),
      .out_valid(out_valid), .out_co(out_co), .out_t(out_t), .out_y(out_y));

  // ---- checking
  integer errors = 0, outputs = 0, idx;
  always @(posedge clk) begin
    if (out_valid) begin
      idx = (out_t - cfg[9]) * cfg[2] + out_co;
      outputs = outputs + 1;
      if (out_t < cfg[9] || out_t >= cfg[9] + cfg[10] || out_co >= cfg[2]) begin
        if (errors < 10) $display("  output out of range: t=%0d co=%0d", out_t, out_co);
        errors = errors + 1;
      end else begin
        if (seen[idx]) begin
          if (errors < 10) $display("  duplicate output t=%0d co=%0d", out_t, out_co);
          errors = errors + 1;
        end
        seen[idx] = 1'b1;
        if (out_y !== $signed(exp_mem[idx])) begin
          if (errors < 10)
            $display("  mismatch t=%0d co=%0d: got %0d, expected %0d", out_t, out_co, out_y, $signed(exp_mem[idx]));
          errors = errors + 1;
        end
      end
    end
  end

  // ---- test sequence
  reg [8*256-1:0] dir;
  integer fd, n, i, cycles, b, a, d, expected;
  reg [31:0] pb, pm, ps;
  initial begin
    if (!$value$plusargs("dir=%s", dir)) begin
      $display("usage: +dir=TESTDIR");
      $finish;
    end
    $readmemh($sformatf("%0s/cfg.hex", dir), cfg);
    $readmemh($sformatf("%0s/weights.hex", dir), wmem);
    $readmemh($sformatf("%0s/res.hex", dir), res_mem);
    $readmemh($sformatf("%0s/expect.hex", dir), exp_mem);
    fd = $fopen($sformatf("%0s/params.hex", dir), "r");
    for (i = 0; i < cfg[2]; i = i + 1) begin
      n = $fscanf(fd, "%h %h %h\n", pb, pm, ps);
      bias_mem[i] = pb;
      mult_mem[i] = pm[15:0];
      shift_mem[i] = ps[7:0];
    end
    $fclose(fd);
    expected = cfg[10] * cfg[2];
    for (i = 0; i < expected; i = i + 1) seen[i] = 1'b0;

    repeat (3) @(posedge clk);
    rst_n = 1'b1;

    // load the tile through the banks' write port, one value per clock
    fd = $fopen($sformatf("%0s/act.hex", dir), "r");
    for (i = 0; i < cfg[12]; i = i + 1) begin
      n = $fscanf(fd, "%h %h %h\n", b, a, d);
      @(negedge clk);
      wr_en = 1'b1;
      wr_bank = b;
      wr_addr = a;
      wr_data = d;
    end
    @(negedge clk);
    wr_en = 1'b0;
    $fclose(fd);

    @(negedge clk);
    start = 1'b1;
    @(negedge clk);
    start = 1'b0;
    cycles = 1;
    while (busy) begin
      @(negedge clk);
      cycles = cycles + 1;
    end
    repeat (10) @(posedge clk);  // drain vocoder_post

    if (outputs != expected) begin
      $display("  got %0d outputs, expected %0d", outputs, expected);
      errors = errors + 1;
    end
    $display("%s: %0d outputs, %0d cycles, %0d errors", errors == 0 ? "PASS" : "FAIL", outputs, cycles, errors);
    $finish;
  end

endmodule
