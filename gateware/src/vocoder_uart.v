// 8N1 UART receiver and transmitter for the vocoder's serial link (see
// vocoder_link.v for the protocol). On the Tang Nano 20K the default pins
// 69/70 go to the onboard BL616 USB bridge, so a PC can stream latents
// over the programming cable; any GPIO pair works for an ESP32.
`timescale 1ns / 1ps

module vocoder_uart_rx #(
    parameter CLK_HZ = 54_000_000,
    parameter BAUD   = 921_600
) (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       rx,
    output reg        valid,  // one-clock pulse per received byte
    output reg  [7:0] data,
    output reg        frame_error
);
  localparam integer DIV = (CLK_HZ + BAUD / 2) / BAUD;

  reg [2:0] sync = 3'b111;
  always @(posedge clk) sync <= {sync[1:0], rx};
  wire rxd = sync[2];

  reg        active;
  reg [15:0] cnt;
  reg [3:0]  bitn;
  reg [7:0]  shift;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      active <= 1'b0;
      valid <= 1'b0;
      frame_error <= 1'b0;
    end else begin
      valid <= 1'b0;
      if (!active) begin
        if (!rxd) begin  // start bit edge: sample bits in their middle
          active <= 1'b1;
          cnt <= DIV + DIV / 2 - 1;
          bitn <= 4'd0;
        end
      end else if (cnt != 0) begin
        cnt <= cnt - 16'd1;
      end else if (bitn < 8) begin
        shift <= {rxd, shift[7:1]};
        bitn <= bitn + 4'd1;
        cnt <= DIV - 1;
      end else begin  // stop bit
        active <= 1'b0;
        frame_error <= !rxd;
        if (rxd) begin
          valid <= 1'b1;
          data <= shift;
        end
      end
    end
  end
endmodule

module vocoder_uart_tx #(
    parameter CLK_HZ = 54_000_000,
    parameter BAUD   = 921_600
) (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       start,  // ignored while busy
    input  wire [7:0] data,
    output wire       busy,
    output reg        tx
);
  localparam integer DIV = (CLK_HZ + BAUD / 2) / BAUD;

  reg [8:0]  shift;  // data bits, then the stop bit
  reg [3:0]  left;
  reg [15:0] cnt;    // clocks left of the bit on the line
  assign busy = left != 0 || cnt != 0;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      tx <= 1'b1;
      left <= 4'd0;
      cnt <= 16'd0;
    end else if (cnt != 0) begin
      cnt <= cnt - 16'd1;
    end else if (left != 0) begin
      tx <= shift[0];
      shift <= {1'b1, shift[8:1]};
      left <= left - 4'd1;
      cnt <= DIV - 1;
    end else if (start) begin
      tx <= 1'b0;  // start bit
      shift <= {1'b1, data};
      left <= 4'd9;
      cnt <= DIV - 1;
    end
  end
endmodule
