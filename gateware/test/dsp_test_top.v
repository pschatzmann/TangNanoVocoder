// Hardware check of vocoder_mul.v's DSP multipliers on a Tang Nano 20K: the
// PC sends 'M', a (18-bit), b (18-bit), A (36-bit), B (36-bit) over the USB
// serial port (little endian, 3 + 3 + 5 + 5 bytes); the reply is a * b
// (5 bytes) and A * B (9 bytes) from the MULT18X18 and MULT36X36 the
// vocoder uses. tools/dsp_test.py drives it.
`timescale 1ns / 1ps
module dsp_test_top (
    input  wire clk_27m,
    input  wire usb_uart_rx,
    output wire usb_uart_tx,
    output wire [5:0] leds
);
  wire clk = clk_27m;
  reg [3:0] rst_cnt = 0;
  always @(posedge clk) if (rst_cnt != 4'hF) rst_cnt <= rst_cnt + 1'b1;
  wire rst_n = rst_cnt == 4'hF;

  wire rx_valid;
  wire [7:0] rx_data;
  vocoder_uart_rx #(.CLK_HZ(27_000_000), .BAUD(921_600)) u_rx (
      .clk(clk), .rst_n(rst_n), .rx(usb_uart_rx), .valid(rx_valid), .data(rx_data), .frame_error());

  reg [127:0] in_bytes;
  reg [4:0] n_in;
  reg have_cmd;
  reg [17:0] a, b;
  reg [35:0] A, B;
  wire [35:0] p18;
  wire [71:0] p36;
  vocoder_mul18 u_m18 (.clk(clk), .a(a), .b(b), .p(p18));
  vocoder_mul36 u_m36 (.clk(clk), .a(A), .b(B), .p(p36));

  reg [111:0] out_bytes;
  reg [4:0] n_out;
  wire tx_busy;
  reg tx_start;
  vocoder_uart_tx #(.CLK_HZ(27_000_000), .BAUD(921_600)) u_tx (
      .clk(clk), .rst_n(rst_n), .start(tx_start), .data(out_bytes[7:0]), .busy(tx_busy), .tx(usb_uart_tx));

  reg [1:0] phase;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      n_in <= 0; have_cmd <= 0; n_out <= 0; tx_start <= 0; phase <= 0;
    end else begin
      tx_start <= 0;
      if (rx_valid) begin
        if (!have_cmd) begin
          if (rx_data == 8'h4D) begin have_cmd <= 1; n_in <= 0; end
        end else begin
          in_bytes <= {rx_data, in_bytes[127:8]};
          n_in <= n_in + 1'b1;
          if (n_in == 5'd15) begin have_cmd <= 0; phase <= 1; end
        end
      end
      if (phase == 1) begin  // in_bytes[127:0] = the 16 operand bytes
        a <= in_bytes[17:0];
        b <= in_bytes[41:24];
        A <= in_bytes[83:48];
        B <= in_bytes[123:88];
        phase <= 2;
      end else if (phase == 2) begin
        out_bytes <= {p36, 4'd0, p18};
        n_out <= 5'd14;
        phase <= 3;
      end else if (phase == 3) begin
        if (n_out == 0) phase <= 0;
        else if (!tx_busy && !tx_start) begin
          tx_start <= 1'b1;
        end else if (tx_start) begin
          out_bytes <= {8'd0, out_bytes[111:8]};
          n_out <= n_out - 1'b1;
        end
      end
    end
  end
  assign leds = ~{phase, n_out[3:0]};
endmodule
