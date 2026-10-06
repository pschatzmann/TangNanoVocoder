// Hardware check: 16 MULT18X18 in signed mode with ASIGN/BSIGN driven by a
// register (1 after reset) instead of a constant - does signed mode then work
// on every instance? 'M', a (3 bytes), b (3 bytes) -> 16 products of
// (a + i) x (b - 3 i), 5 bytes each.
`timescale 1ns / 1ps
module dsp16_test_top (
    input  wire clk_27m,
    input  wire usb_uart_rx,
    output wire usb_uart_tx,
    output wire [5:0] leds
);
  wire clk = clk_27m;
  reg [3:0] rst_cnt = 0;
  always @(posedge clk) if (rst_cnt != 4'hF) rst_cnt <= rst_cnt + 1'b1;
  wire rst_n = rst_cnt == 4'hF;
  reg sgn;  // 0 in reset, then 1: a net, not a constant
  always @(posedge clk or negedge rst_n) if (!rst_n) sgn <= 1'b0; else sgn <= 1'b1;

  wire rx_valid;
  wire [7:0] rx_data;
  vocoder_uart_rx #(.CLK_HZ(27_000_000), .BAUD(921_600)) u_rx (
      .clk(clk), .rst_n(rst_n), .rx(usb_uart_rx), .valid(rx_valid), .data(rx_data), .frame_error());

  reg [47:0] in_bytes;
  reg [2:0]  n_in;
  reg        have_cmd;
  reg [16*18-1:0] a, b;  // flat vectors: arrays on ports make yosys re-elaborate
  wire [16*36-1:0] p;
  genvar g;
  generate
    for (g = 0; g < 16; g = g + 1) begin : m
      MULT18X18 #(.AREG(1'b0), .BREG(1'b0), .OUT_REG(1'b0), .PIPE_REG(1'b0), .ASIGN_REG(1'b0), .BSIGN_REG(1'b0),
                  .SOA_REG(1'b0)) u (
          .A(a[g*18 +: 18]), .B(b[g*18 +: 18]), .SIA(18'd0), .SIB(18'd0), .ASIGN(sgn), .BSIGN(sgn), .ASEL(1'b0), .BSEL(1'b0),
          .CE(1'b1), .CLK(clk), .RESET(1'b0), .DOUT(p[g*36 +: 36]), .SOA(), .SOB());
    end
  endgenerate

  reg [16*40-1:0] out_bytes;
  reg [7:0] n_out;
  wire tx_busy;
  reg tx_start;
  vocoder_uart_tx #(.CLK_HZ(27_000_000), .BAUD(921_600)) u_tx (
      .clk(clk), .rst_n(rst_n), .start(tx_start), .data(out_bytes[7:0]), .busy(tx_busy), .tx(usb_uart_tx));
  reg [1:0] phase;
  integer k;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      n_in <= 0; have_cmd <= 0; n_out <= 0; tx_start <= 0; phase <= 0;
    end else begin
      tx_start <= 0;
      if (rx_valid) begin
        if (!have_cmd) begin
          if (rx_data == 8'h4D) begin have_cmd <= 1; n_in <= 0; end
        end else begin
          in_bytes <= {rx_data, in_bytes[47:8]};
          n_in <= n_in + 1'b1;
          if (n_in == 3'd5) begin have_cmd <= 0; phase <= 1; end
        end
      end
      if (phase == 1) begin
        for (k = 0; k < 16; k = k + 1) begin
          a[k*18 +: 18] <= in_bytes[17:0] + k;
          b[k*18 +: 18] <= in_bytes[41:24] - 3 * k;
        end
        phase <= 2;
      end else if (phase == 2) begin
        for (k = 0; k < 16; k = k + 1) out_bytes[k*40 +: 40] <= {4'd0, p[k*36 +: 36]};
        n_out <= 8'd80;
        phase <= 3;
      end else if (phase == 3) begin
        if (n_out == 0) phase <= 0;
        else if (!tx_busy && !tx_start) tx_start <= 1'b1;
        else if (tx_start) begin out_bytes <= {8'd0, out_bytes[16*40-1:8]}; n_out <= n_out - 1'b1; end
      end
    end
  end
  assign leds = ~{4'd0, phase};
endmodule
