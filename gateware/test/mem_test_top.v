// Hardware check of vocoder_act_banks (16 block RAMs, mapped like the
// vocoder's other memories) on a Tang Nano 20K over the USB serial port:
//   'W' bank addr(2) data(2)                       write through port 1
//   'V' bank addr(2) data(2) bank addr(2) data(2)  ports 1 and 2 in the same clock
//   'R' addr(2)                                    read all banks at addr with
//       rd_en for one clock, then 3 clocks with rd_en low and another address
//       (the outputs must hold); reply 16 x 2 bytes
// tools/mem_test.py drives it.
`timescale 1ns / 1ps
module mem_test_top (
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

  reg        wr_en, wr2_en, rd_en;
  reg [3:0]  wr_bank, wr2_bank;
  reg [9:0]  wr_addr, wr2_addr, rd_a;
  reg [15:0] wr_data, wr2_data;
  wire [16*16-1:0] rd_data;
  vocoder_act_banks #(.LANES(16), .ABITS(10)) u_banks (
      .clk(clk), .wr_en(wr_en), .wr_bank(wr_bank), .wr_addr(wr_addr), .wr_data(wr_data),
      .wr2_en(wr2_en), .wr2_bank(wr2_bank), .wr2_addr(wr2_addr), .wr2_data(wr2_data),
      .rd_en(rd_en), .rd_addr({16{rd_a}}), .rd_data(rd_data));

  reg [7:0]  cmd;
  reg [3:0]  need, got;
  reg [79:0] arg;
  reg [255:0] out;
  reg [5:0]  n_out;
  reg [2:0]  step;
  wire tx_busy;
  reg tx_start;
  vocoder_uart_tx #(.CLK_HZ(27_000_000), .BAUD(921_600)) u_tx (
      .clk(clk), .rst_n(rst_n), .start(tx_start), .data(out[7:0]), .busy(tx_busy), .tx(usb_uart_tx));

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      cmd <= 0; need <= 0; got <= 0; n_out <= 0; step <= 0; tx_start <= 0;
      wr_en <= 0; wr2_en <= 0; rd_en <= 0;
    end else begin
      wr_en <= 0; wr2_en <= 0; rd_en <= 0; tx_start <= 0;
      if (rx_valid && step == 0 && n_out == 0) begin
        if (cmd == 0) begin
          cmd <= rx_data;
          need <= rx_data == 8'h57 ? 4'd5 : rx_data == 8'h56 ? 4'd10 : rx_data == 8'h52 ? 4'd2 : 4'd0;
          got <= 0;
        end else begin
          arg <= {rx_data, arg[79:8]};
          got <= got + 1'b1;
          if (got + 1'b1 == need) step <= 1;
        end
      end
      case (step)
        1: begin  // arg: the last `need` bytes in arg[79 -: 8*need]
          if (cmd == 8'h57) begin
            wr_en <= 1; wr_bank <= arg[43:40]; wr_addr <= arg[57:48]; wr_data <= arg[79:64];
            step <= 0; cmd <= 0;
          end else if (cmd == 8'h56) begin
            wr_en <= 1; wr_bank <= arg[3:0]; wr_addr <= arg[17:8]; wr_data <= arg[39:24];
            wr2_en <= 1; wr2_bank <= arg[43:40]; wr2_addr <= arg[57:48]; wr2_data <= arg[79:64];
            step <= 0; cmd <= 0;
          end else begin  // 'R'
            rd_en <= 1; rd_a <= arg[73:64];
            step <= 2;
          end
        end
        2: begin rd_a <= ~rd_a; step <= 3; end  // rd_en low from here: outputs must hold
        3: step <= 4;
        4: begin out <= rd_data; n_out <= 6'd32; step <= 0; cmd <= 0; end
      endcase
      if (n_out != 0 && step == 0) begin
        if (!tx_busy && !tx_start) tx_start <= 1'b1;
        else if (tx_start) begin out <= {8'd0, out[255:8]}; n_out <= n_out - 1'b1; end
      end
    end
  end
  assign leds = ~{2'b0, step, rd_en};
endmodule
