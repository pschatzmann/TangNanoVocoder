// SPI slave (mode 0: sample MOSI on SCLK rising, shift MISO on falling,
// MSB first) for the vocoder's link from the ESP32 (protocol in
// vocoder_link.v). SCLK, MOSI and CS are oversampled by `clk`, so SCLK
// must stay below clk / 4 (13.5MHz at 54MHz - the link needs well under
// 1MHz).
//
// Every byte the master clocks in, it gets `tx_data` back on MISO: the
// value sampled at the start of that byte (the link's status byte).
`timescale 1ns / 1ps

module vocoder_spi_slave (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       sclk,
    input  wire       mosi,
    input  wire       cs_n,
    output wire       miso,
    output reg        valid,  // one-clock pulse per received byte
    output reg  [7:0] data,
    input  wire [7:0] tx_data
);
  reg [2:0] sclk_s, cs_s;
  reg [1:0] mosi_s;
  always @(posedge clk) begin
    sclk_s <= {sclk_s[1:0], sclk};
    cs_s <= {cs_s[1:0], cs_n};
    mosi_s <= {mosi_s[0], mosi};
  end
  wire selected = !cs_s[1];
  wire rise = sclk_s[2:1] == 2'b01;
  wire fall = sclk_s[2:1] == 2'b10;

  reg [2:0] bitn;
  reg [7:0] rx, tx;
  assign miso = tx[7];

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      valid <= 1'b0;
      bitn <= 3'd0;
      tx <= 8'd0;
    end else begin
      valid <= 1'b0;
      if (!selected) begin
        bitn <= 3'd0;
        tx <= tx_data;  // first bit is on MISO as soon as CS falls
      end else if (rise) begin
        rx <= {rx[6:0], mosi_s[1]};
        bitn <= bitn + 3'd1;
        if (bitn == 3'd7) begin
          valid <= 1'b1;
          data <= {rx[6:0], mosi_s[1]};
        end
      end else if (fall) begin
        tx <= bitn == 3'd0 ? tx_data : {tx[6:0], 1'b0};
      end
    end
  end
endmodule
