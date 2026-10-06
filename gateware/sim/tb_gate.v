// Port-only system test, for the RTL or yosys's synthesized netlist of
// vocoder_system (gate-level: run_gate_test.sh). Ops are limited by the
// image header (word 2); the output buffer of the last op that ran is
// compared with the model's golden vectors in the SDRAM model.
//
// Files in build/gate/: image.hex (header op count already patched), the
// sentence (sentence.hex), and expect.hex: "base plane channels frames"
// on the first line, then the expected values [channel][frame].
`timescale 1ns / 1ps

module tb_gate;
  localparam CLK_HZ = 54_000_000, BAUD = 3_375_000;
  localparam integer BIT_NS = 1_000_000_000 / BAUD;

  reg clk = 1'b0;
  always #9.259 clk = ~clk;
  reg rst_n = 1'b0;

  wire [31:0] dq;
  wire [10:0] sa;
  wire [1:0]  ba;
  wire ncs, nwe, nras, ncas, sclk_sd, cke;
  wire [3:0] dqm;
  reg  usb_rx = 1'b1;
  wire usb_tx, miso, bclk, ws, din, pwm, utx;
  wire [5:0] leds;

`ifdef GATE
  vocoder_system dut (
`else
  vocoder_system #(.CLK_HZ(CLK_HZ), .BAUD(BAUD), .INIT_US(2), .FLASH_BOOT(0), .TANH_FILE(`TANH_FILE), .EXP_FILE(`EXP_FILE),
                   .RSQRT_FILE(`RSQRT_FILE)) dut (
`endif
      .clk(clk), .clk_sdram(~clk), .rst_n(rst_n),
      .spi_sclk(1'b0), .spi_mosi(1'b0), .spi_cs_n(1'b1), .spi_miso(miso),
      .uart_rx(1'b1), .uart_tx(utx), .usb_uart_rx(usb_rx), .usb_uart_tx(usb_tx),
      .i2s_bclk(bclk), .i2s_ws(ws), .i2s_din(din), .pwm_out(pwm), .state_leds(leds),
      .SDRAM_DQ(dq), .SDRAM_A(sa), .SDRAM_BA(ba), .SDRAM_nCS(ncs), .SDRAM_nWE(nwe), .SDRAM_nRAS(nras),
      .SDRAM_nCAS(ncas), .SDRAM_CLK(sclk_sd), .SDRAM_CKE(cke), .SDRAM_DQM(dqm),
      .flash_cs_n(), .flash_sclk(), .flash_mosi(), .flash_miso(1'b1));  // flash boot off (FLASH_BOOT 0)

  sdram_model #(.CAS(2), .MEM_AW(21)) u_mem (
      .SDRAM_DQ(dq), .SDRAM_A(sa), .SDRAM_BA(ba), .SDRAM_nCS(ncs), .SDRAM_nWE(nwe), .SDRAM_nRAS(nras),
      .SDRAM_nCAS(ncas), .SDRAM_CLK(sclk_sd), .SDRAM_CKE(cke), .SDRAM_DQM(dqm));

  task send_byte(input [7:0] b);
    integer i;
    begin
      usb_rx = 1'b0;
      #(BIT_NS);
      for (i = 0; i < 8; i = i + 1) begin
        usb_rx = b[i];
        #(BIT_NS);
      end
      usb_rx = 1'b1;
      #(BIT_NS);
    end
  endtask

  wire r_valid;
  wire [7:0] r_data;
  vocoder_uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_reply (
      .clk(clk), .rst_n(rst_n), .rx(usb_tx), .valid(r_valid), .data(r_data), .frame_error());
  reg [7:0] last_reply = 8'h00;
  integer replies = 0;
  always @(posedge clk) if (r_valid) begin
    last_reply = r_data;
    replies = replies + 1;
  end

  task status(output [7:0] s);
    integer n;
    begin
      n = replies;
      send_byte(8'h3F);
      wait (replies > n);
      s = last_reply;
    end
  endtask

  reg [31:0] image [0:(1 << 20) - 1];
  reg [7:0]  sentence [0:4095];
  reg [15:0] expect_v [0:65535];
  integer base, plane, chans, frames, n_sentence, i, c, t, fd, r, errors = 0;
  reg [7:0] st, sum;
  reg [31:0] w;
  reg [15:0] got;
  initial begin
    $readmemh({`GATE_DIR, "/image.hex"}, image);
    fd = $fopen({`GATE_DIR, "/sentence.hex"}, "r");
    n_sentence = 0;
    while (!$feof(fd)) begin
      r = $fscanf(fd, "%h\n", sentence[n_sentence]);
      if (r == 1) n_sentence = n_sentence + 1;
    end
    $fclose(fd);
    fd = $fopen({`GATE_DIR, "/expect.hex"}, "r");
    r = $fscanf(fd, "%d %d %d %d\n", base, plane, chans, frames);
    for (i = 0; i < chans * frames; i = i + 1) r = $fscanf(fd, "%h\n", expect_v[i]);
    $fclose(fd);
    for (i = 0; i < (1 << 21); i = i + 1) u_mem.mem[i] = 32'd0;
    for (i = 0; i < (1 << 20); i = i + 1) if (image[i] !== 32'bx) u_mem.mem[i] = image[i];

    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (2000) @(posedge clk);  // SDRAM init (INIT_US 2)

    // the header again through 'P' (makes the core read it)
    send_byte(8'h50);
    send_byte(8'd64); send_byte(8'd0); send_byte(8'd0); send_byte(8'd0);
    sum = 0;
    for (i = 0; i < 16; i = i + 1) begin
      w = image[i];
      send_byte(w[7:0]); send_byte(w[15:8]); send_byte(w[23:16]); send_byte(w[31:24]);
      sum = sum + w[7:0] + w[15:8] + w[23:16] + w[31:24];
    end
    send_byte(sum);
    repeat (200) @(posedge clk);
    status(st);
    $display("status after upload: %02h", st);

    for (i = 0; i < n_sentence; i = i + 1) send_byte(sentence[i]);
    repeat (200) @(posedge clk);
    status(st);
    while (st[2]) begin  // busy
      repeat (20000) @(posedge clk);
      status(st);
      $display("  %0t: status %02h", $time, st);
      $fflush;
    end

    for (c = 0; c < chans; c = c + 1)
      for (t = 0; t < frames; t = t + 1) begin
        w = u_mem.mem[base + c * plane + t / 2];
        got = t[0] ? w[31:16] : w[15:0];
        if (got !== expect_v[c * frames + t]) begin
          if (errors < 8) $display("  channel %0d frame %0d: %0d, expected %0d", c, t, $signed(got),
                                   $signed(expect_v[c * frames + t]));
          errors = errors + 1;
        end
      end
    $display("%s: %0d of %0d values differ", errors == 0 ? "PASS" : "FAIL", errors, chans * frames);
    $finish;
  end
endmodule
