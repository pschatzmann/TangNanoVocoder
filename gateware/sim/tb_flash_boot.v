// vocoder_flash_boot against a SPI NOR flash model (Read Data, 03h): with a
// valid "TNVF" record at the offset, the bytes it plays into the link must be
// exactly a host's 'P' upload ('P', u32 count, image, checksum); with no
// record (erased flash, 0xFF), nothing, and it must let go of the link.
//   run_flash_boot_test.sh
`timescale 1ns / 1ps

module flash_model #(parameter BYTES = 1 << 21) (
    input  wire cs_n,
    input  wire sclk,
    input  wire mosi,
    output reg  miso
);
  reg [7:0]  mem [0:BYTES-1];
  reg [31:0] cmd;
  reg [5:0]  nin;
  reg [23:0] addr;
  reg [2:0]  bitn;
  reg        reading;
  initial miso = 1'b1;
  always @(negedge cs_n) begin
    nin = 0;
    reading = 0;
    bitn = 0;
  end
  // command and address in on rising edges
  always @(posedge sclk) if (!cs_n && !reading) begin
    cmd = {cmd[30:0], mosi};
    nin = nin + 1;
    if (nin == 32) begin
      if (cmd[31:24] != 8'h03) $display("flash: unexpected command %02h", cmd[31:24]);
      addr = cmd[23:0];
      reading = 1;
      bitn = 0;
    end
  end
  // data out on falling edges (the first bit right after the address)
  always @(negedge sclk) if (!cs_n && reading) begin
    miso <= mem[addr][7 - bitn];
    if (bitn == 7) addr = addr + 1;
    bitn = bitn + 1;
  end
endmodule

module tb_flash_boot;
  reg clk = 1'b0;
  always #9.259 clk = ~clk;
  reg rst_n = 1'b0;

  wire active, out_valid;
  wire [7:0] out_data;
  wire cs_n, sclk, mosi, miso;
  vocoder_flash_boot #(.OFFSET(24'h100000), .START_US(1)) dut (
      .clk(clk), .rst_n(rst_n), .active(active), .out_valid(out_valid), .out_data(out_data),
      .flash_cs_n(cs_n), .flash_sclk(sclk), .flash_mosi(mosi), .flash_miso(miso));
  flash_model flash (.cs_n(cs_n), .sclk(sclk), .mosi(mosi), .miso(miso));

  localparam N = 1000;  // image bytes (multiple of 4)
  reg [7:0] want [0:N+5];
  integer n_out = 0, errors = 0, i, k, min_gap = 1 << 30, last_t = 0;
  reg [7:0] sum;
  always @(posedge clk) if (out_valid) begin
    // vocoder_system.v takes the bytes only while `active` is high
    if (!active) begin
      if (errors < 5) $display("  byte %0d while not active (the link would miss it)", n_out);
      errors = errors + 1;
    end
    if (n_out > 0 && $time - last_t < min_gap) min_gap = $time - last_t;
    last_t = $time;
    if (n_out <= N + 5 && out_data !== want[n_out]) begin
      if (errors < 5) $display("  byte %0d: %02h, expected %02h", n_out, out_data, want[n_out]);
      errors = errors + 1;
    end
    n_out = n_out + 1;
  end

  initial begin
    // ---- valid record: "TNVF", N, image, checksum
    for (i = 0; i < (1 << 21); i = i + 1) flash.mem[i] = 8'hFF;
    flash.mem[24'h100000] = "T"; flash.mem[24'h100001] = "N";
    flash.mem[24'h100002] = "V"; flash.mem[24'h100003] = "F";
    for (k = 0; k < 4; k = k + 1) flash.mem[24'h100004 + k] = (N >> (8 * k)) & 8'hFF;
    sum = 0;
    for (i = 0; i < N; i = i + 1) begin
      flash.mem[24'h100008 + i] = (i * 37 + 11) & 8'hFF;
      sum = sum + ((i * 37 + 11) & 8'hFF);
    end
    flash.mem[24'h100008 + N] = sum;
    // the 'P' packet the link must see
    want[0] = 8'h50;
    for (k = 0; k < 4; k = k + 1) want[1 + k] = (N >> (8 * k)) & 8'hFF;
    for (i = 0; i < N; i = i + 1) want[5 + i] = (i * 37 + 11) & 8'hFF;
    want[5 + N] = sum;

    repeat (5) @(posedge clk);
    rst_n = 1'b1;
    wait (active);
    wait (!active);
    repeat (100) @(posedge clk);
    $display("valid record: %0d bytes out (expected %0d), %0d wrong, closest bytes %0d ns apart, CS %s",
             n_out, N + 6, errors, min_gap, cs_n ? "released" : "STILL LOW");
    if (n_out != N + 6 || !cs_n || min_gap < 32 * 18) errors = errors + 1;

    // ---- erased flash: nothing out, active never stays high
    rst_n = 1'b0;
    for (i = 0; i < 16; i = i + 1) flash.mem[24'h100000 + i] = 8'hFF;
    n_out = 0;
    repeat (5) @(posedge clk);
    rst_n = 1'b1;
    repeat (20000) @(posedge clk);
    $display("erased flash: %0d bytes out, active %b, CS %s", n_out, active, cs_n ? "released" : "STILL LOW");
    if (n_out != 0 || active || !cs_n) errors = errors + 1;

    $display("%s: %0d errors", errors == 0 ? "PASS" : "FAIL", errors);
    $finish;
  end
endmodule
