// Whole-system test: vocoder_system with the SDRAM model, against the
// phase 1 model's PCM, bit for bit. run_system_test.sh prepares the files:
//
//   image.hex     the hardware image (vocoder_model --export-hw), one word per line
//   sentence.hex  the sentence packet (--sentence-out), one byte per line
//   pcm.hex       expected PCM (golden conv_post output), one sample per line
//
// The image is put into the SDRAM model directly (an upload over the link
// would take most of the simulation time), then the first 64 of its words
// are uploaded again through the real 'P' path, which also makes the core
// read the header. The sentence goes over the USB UART (fast baud for
// simulation), the PCM is checked in SDRAM and as played (into the audio
// FIFO), and 'T' reads back the cycle counts.
`timescale 1ns / 1ps

module tb_system;
  localparam CLK_HZ = 54_000_000, BAUD = 3_375_000;  // 16 clocks per bit
  localparam integer BIT_NS = 1_000_000_000 / BAUD;
  localparam IMAGE_WORDS = 1 << 18, MAX_SENTENCE = 4096, MAX_PCM = 1 << 16;

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

  vocoder_system #(.CLK_HZ(CLK_HZ), .BAUD(BAUD), .INIT_US(2), .TANH_FILE(`TANH_FILE)) dut (
      .clk(clk), .clk_sdram(~clk), .rst_n(rst_n),
      .spi_sclk(1'b0), .spi_mosi(1'b0), .spi_cs_n(1'b1), .spi_miso(miso),
      .uart_rx(1'b1), .uart_tx(utx), .usb_uart_rx(usb_rx), .usb_uart_tx(usb_tx),
      .i2s_bclk(bclk), .i2s_ws(ws), .i2s_din(din), .pwm_out(pwm), .state_leds(leds),
      .SDRAM_DQ(dq), .SDRAM_A(sa), .SDRAM_BA(ba), .SDRAM_nCS(ncs), .SDRAM_nWE(nwe), .SDRAM_nRAS(nras),
      .SDRAM_nCAS(ncas), .SDRAM_CLK(sclk_sd), .SDRAM_CKE(cke), .SDRAM_DQM(dqm));

  sdram_model #(.CAS(2), .MEM_AW(21)) u_mem (
      .SDRAM_DQ(dq), .SDRAM_A(sa), .SDRAM_BA(ba), .SDRAM_nCS(ncs), .SDRAM_nWE(nwe), .SDRAM_nRAS(nras),
      .SDRAM_nCAS(ncas), .SDRAM_CLK(sclk_sd), .SDRAM_CKE(cke), .SDRAM_DQM(dqm));

  reg [31:0] image [0:IMAGE_WORDS - 1];
  reg [7:0]  sentence [0:MAX_SENTENCE - 1];
  reg [15:0] pcm [0:MAX_PCM - 1];
  integer errors = 0;

  // ---- UART to the DUT, and its replies
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
  reg [7:0] reply [0:15];
  integer replies = 0;
  always @(posedge clk) if (r_valid) begin
    if (replies < 16) reply[replies] = r_data;
    replies = replies + 1;
  end

  // ---- played samples
  integer played = 0;
  always @(posedge clk) if (dut.pb_valid && dut.pb_ready) begin
    if (played < MAX_PCM && dut.pb_sample !== pcm[played]) begin
      if (errors < 10) $display("  played sample %0d: %0d, expected %0d", played, $signed(dut.pb_sample), $signed(pcm[played]));
      errors = errors + 1;
    end
    played = played + 1;
  end

  integer i, n_sentence, n_pcm, frames, pcm_base, t, run_cycles, mac_cycles;
  reg [7:0] sum;
  reg [31:0] w;
  reg [15:0] got;
  integer fd, c;
  initial begin
    $readmemh("build/system/image.hex", image);
    fd = $fopen("build/system/sentence.hex", "r");
    n_sentence = 0;
    while (!$feof(fd) && n_sentence < MAX_SENTENCE) begin
      c = $fscanf(fd, "%h\n", sentence[n_sentence]);
      if (c == 1) n_sentence = n_sentence + 1;
    end
    $fclose(fd);
    fd = $fopen("build/system/pcm.hex", "r");
    n_pcm = 0;
    while (!$feof(fd) && n_pcm < MAX_PCM) begin
      c = $fscanf(fd, "%h\n", pcm[n_pcm]);
      if (c == 1) n_pcm = n_pcm + 1;
    end
    $fclose(fd);
    for (i = 0; i < (1 << 21); i = i + 1) u_mem.mem[i] = 32'd0;
    for (i = 0; i < IMAGE_WORDS; i = i + 1) if (image[i] !== 32'bx) u_mem.mem[i] = image[i];
    frames = {sentence[2], sentence[1]};
    pcm_base = image[8];
    $display("sentence: %0d frames, %0d samples expected", frames, n_pcm);

    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    wait (dut.sdram_ready);

    $display("upload: first 64 image words through 'P'");
    send_byte(8'h50);
    send_byte(8'h00);
    send_byte(8'h01);  // 256 bytes
    send_byte(8'h00);
    send_byte(8'h00);
    sum = 8'd0;
    for (i = 0; i < 64; i = i + 1) begin
      w = image[i];
      send_byte(w[7:0]);
      send_byte(w[15:8]);
      send_byte(w[23:16]);
      send_byte(w[31:24]);
      sum = sum + w[7:0] + w[15:8] + w[23:16] + w[31:24];
    end
    send_byte(sum);
    wait (dut.hdr_valid);
    @(posedge clk);
    if (!dut.prog_valid) begin
      $display("  FAIL: program not valid after upload");
      errors = errors + 1;
    end
    if (dut.status !== 8'h91) begin
      $display("  FAIL: status %02h after upload, expected 91", dut.status);
      errors = errors + 1;
    end

    $display("sentence over the USB UART");
    for (i = 0; i < n_sentence; i = i + 1) send_byte(sentence[i]);
    wait (dut.core_busy);
    $display("computing ...");
    wait (dut.core_done);
    run_cycles = dut.run_cycles;
    mac_cycles = dut.mac_cycles;
    $display("done after %0d cycles (%0d with the engine busy)", run_cycles, mac_cycles);

    // PCM in SDRAM
    for (t = 0; t < n_pcm; t = t + 1) begin
      w = u_mem.mem[pcm_base + t / 2];
      got = t[0] ? w[31:16] : w[15:0];
      if (got !== pcm[t]) begin
        if (errors < 10) $display("  PCM sample %0d in SDRAM: %0d, expected %0d", t, $signed(got), $signed(pcm[t]));
        errors = errors + 1;
      end
    end

    wait (played >= n_pcm);
    repeat (100) @(posedge clk);
    if (played != n_pcm) begin
      $display("  FAIL: played %0d samples, expected %0d", played, n_pcm);
      errors = errors + 1;
    end
    wait (!dut.pb_busy);

    send_byte(8'h54);
    wait (replies >= 9);
    $display("'T' reply: status %02h, run %0d cycles, engine %0d cycles", reply[0],
             {reply[4], reply[3], reply[2], reply[1]}, {reply[8], reply[7], reply[6], reply[5]});
    if ({reply[4], reply[3], reply[2], reply[1]} != run_cycles || {reply[8], reply[7], reply[6], reply[5]} != mac_cycles ||
        reply[0] !== 8'h91) begin
      $display("  FAIL: 'T' reply");
      errors = errors + 1;
    end
    if (u_mem.errors != 0) begin
      $display("  FAIL: %0d SDRAM protocol errors", u_mem.errors);
      errors = errors + 1;
    end
    $display("audio: %0d samples = %0f s, computed in %0f s at 54 MHz (%0.2fx real time)", n_pcm,
             n_pcm / 44100.0, run_cycles / 54.0e6, (run_cycles / 54.0e6) / (n_pcm / 44100.0));
    if (dut.u_core.busy) begin
      $display("  FAIL: the core started again without a new sentence");
      errors = errors + 1;
    end
    $display("%s: %0d errors", errors == 0 ? "PASS" : "FAIL", errors);
    $finish;
  end

  // progress
  integer last_op = -1;
  always @(posedge clk) if (dut.u_core.op_i != last_op && dut.core_busy) begin
    last_op = dut.u_core.op_i;
    if (last_op % 10 == 0) $display("  op %0d at %0t", last_op, $time);
  end
endmodule
