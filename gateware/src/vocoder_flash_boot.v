// Program image from the board's SPI flash at power-up: reads it with the
// flash's plain Read Data command (03h) and plays it into the link exactly
// like a host's upload - 'P', u32 byte count, the image, checksum - so the
// link and the core load it as usual (and check the checksum).
//
// Flash layout at OFFSET (written by tools/tnv.py flash-image):
//   "TNVF", u32 byte count (little endian, multiple of 4), the image,
//   u8 checksum (sum of the image bytes mod 256)
// Without the magic (or with an implausible count) it does nothing, and the
// board waits for an upload as before.
//
// SCLK = clk / (4 DIV) in SPI mode 0: MOSI changes while SCLK is low, MISO
// is sampled at the end of the high phase. One byte per 32 DIV clocks - at
// DIV 1 (13.5 MHz at 54 MHz) the same rate as the SPI slave's fastest, which
// the link takes. `active` is high while it loads, including the clock of
// the last byte (vocoder_system.v takes the bytes only while it is high, and
// ignores the host's meanwhile).
//
// The flash's pins are the FPGA's MSPI pins, usable as I/O after
// configuration (Gowin: use_mspi_as_gpio; gateware/build.py sets it).
`timescale 1ns / 1ps

module vocoder_flash_boot #(
    parameter [23:0] OFFSET   = 24'h100000,
    parameter        DIV      = 1,           // SCLK = clk / (4 DIV)
    parameter        START_US = 1000,        // wait after reset: flash ready, SDRAM initialized
    parameter        CLK_HZ   = 54_000_000
) (
    input  wire       clk,
    input  wire       rst_n,
    output reg        active,
    output reg        out_valid,             // one byte of the 'P' packet
    output reg  [7:0] out_data,

    output reg        flash_cs_n,
    output reg        flash_sclk,
    output reg        flash_mosi,
    input  wire       flash_miso
);
  localparam START_CLKS = CLK_HZ / 1_000_000 * START_US;
  localparam S_WAIT = 3'd0, S_CMD = 3'd1, S_HDR = 3'd2, S_EMIT = 3'd3, S_DATA = 3'd4, S_DONE = 3'd5;
  reg [2:0]  state;
  reg [23:0] wait_cnt;

  // ---- bit engine: one bit per 4 DIV clocks
  reg [7:0]  div;
  reg [1:0]  phase;            // 0, 1: SCLK low; 2, 3: SCLK high
  reg        bit_go;           // a bit is in progress
  reg [31:0] sh_out;           // command + address, MSB first
  reg [7:0]  sh_in;
  reg [5:0]  nbits;            // bits left in the current transfer
  wire       tick = div == DIV - 1;

  reg [5:0]  hdr_n;            // header bytes read (8)
  reg [63:0] hdr;              // "TNVF", count
  reg [31:0] left;             // image bytes still to read, then the checksum
  reg [2:0]  emit_n;           // packet head bytes emitted: 'P', count[0..3]
  reg [5:0]  emit_wait;

  wire [31:0] hdr_count = hdr[63:32];
  wire        hdr_ok = hdr[31:0] == 32'h46564E54 &&  // "TNVF", little endian
                       hdr_count[1:0] == 2'b00 && hdr_count >= 32'd64 && hdr_count < 32'h0080_0000;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state <= S_WAIT;
      wait_cnt <= 0;
      active <= 1'b0;
      out_valid <= 1'b0;
      flash_cs_n <= 1'b1;
      flash_sclk <= 1'b0;
      flash_mosi <= 1'b0;
      div <= 0;
      phase <= 0;
      bit_go <= 1'b0;
    end else begin
      out_valid <= 1'b0;
      // ---- bit engine
      if (bit_go) begin
        div <= tick ? 8'd0 : div + 8'd1;
        if (tick) begin
          phase <= phase + 2'd1;
          case (phase)
            2'd0: flash_mosi <= sh_out[31];
            2'd1: flash_sclk <= 1'b1;
            2'd2: ;
            2'd3: begin  // end of the high phase: sample, fall
              flash_sclk <= 1'b0;
              sh_out <= {sh_out[30:0], 1'b0};
              sh_in <= {sh_in[6:0], flash_miso};
              if (nbits == 6'd1) bit_go <= 1'b0;
              nbits <= nbits - 6'd1;
            end
          endcase
        end
      end

      case (state)
        S_WAIT: begin
          if (wait_cnt == START_CLKS) begin
            flash_cs_n <= 1'b0;
            sh_out <= {8'h03, OFFSET};
            nbits <= 6'd32;
            bit_go <= 1'b1;
            div <= 0;
            phase <= 0;
            state <= S_CMD;
          end else begin
            wait_cnt <= wait_cnt + 24'd1;
          end
        end
        S_CMD: if (!bit_go) begin  // command sent: read the 8 header bytes
          hdr_n <= 0;
          sh_out <= 0;
          nbits <= 6'd8;
          bit_go <= 1'b1;
          state <= S_HDR;
        end
        S_HDR: if (!bit_go) begin
          hdr <= {sh_in, hdr[63:8]};
          if (hdr_n == 6'd7) begin
            state <= S_EMIT;
            emit_n <= 0;
            emit_wait <= 0;
            active <= 1'b1;  // decided below, one clock later
          end else begin
            hdr_n <= hdr_n + 6'd1;
            nbits <= 6'd8;
            bit_go <= 1'b1;
          end
        end
        S_EMIT: begin  // 'P' and the count, spaced like the image bytes
          if (emit_n == 0 && emit_wait == 0 && !hdr_ok) begin
            active <= 1'b0;
            flash_cs_n <= 1'b1;
            state <= S_DONE;
          end else if (emit_wait != 6'd0) begin
            emit_wait <= emit_wait - 6'd1;
          end else begin
            out_valid <= 1'b1;
            case (emit_n)
              3'd0: out_data <= 8'h50;  // 'P'
              3'd1: out_data <= hdr_count[7:0];
              3'd2: out_data <= hdr_count[15:8];
              3'd3: out_data <= hdr_count[23:16];
              default: out_data <= hdr_count[31:24];
            endcase
            emit_wait <= 6'd40;
            if (emit_n == 3'd4) begin
              left <= hdr_count + 32'd1;  // the image, then the checksum
              nbits <= 6'd8;
              bit_go <= 1'b1;
              state <= S_DATA;
            end
            emit_n <= emit_n + 3'd1;
          end
        end
        S_DATA: if (!bit_go) begin  // a byte read: pass it on, read the next
          out_valid <= 1'b1;
          out_data <= sh_in;
          if (left == 32'd1) begin  // the checksum: active stays high with it (S_DONE drops it)
            flash_cs_n <= 1'b1;
            state <= S_DONE;
          end else begin
            left <= left - 32'd1;
            nbits <= 6'd8;
            bit_go <= 1'b1;
          end
        end
        default: active <= 1'b0;  // S_DONE: one clock after the last byte
      endcase
    end
  end
endmodule
