// Activation tile memory for vocoder_conv_engine: LANES banks of DEPTH x 16
// bit, interleaved by frame. Local frame l (frame - tile base), channel ci
// lives in bank l % LANES at address (l / LANES) * cin + ci, so any LANES
// consecutive frames of one channel come from LANES different banks and can
// be read in one clock (one block RAM per bank).
//
// Two write ports (the tile loader writes the two values of an SDRAM word
// at once; they are consecutive frames, so always different banks - port B
// is ignored for a bank port A writes) and one synchronous read per bank. The
// read address/enable come from the engine; `rd_en` low holds the outputs,
// which is how the engine's pipeline stalls.
`timescale 1ns / 1ps

module vocoder_act_banks #(
    parameter LANES = 16,
    parameter ABITS = 10
) (
    input  wire                     clk,
    // write ports
    input  wire                     wr_en,
    input  wire [3:0]               wr_bank,
    input  wire [ABITS-1:0]         wr_addr,
    input  wire [15:0]              wr_data,
    input  wire                     wr2_en,
    input  wire [3:0]               wr2_bank,
    input  wire [ABITS-1:0]         wr2_addr,
    input  wire [15:0]              wr2_data,
    // read: one address per bank, one 16-bit word per bank
    input  wire                     rd_en,
    input  wire [LANES*ABITS-1:0]   rd_addr,
    output wire [LANES*16-1:0]      rd_data
);

  genvar b;
  generate
    for (b = 0; b < LANES; b = b + 1) begin : bank
      (* ram_style = "block" *) reg [15:0] mem [0:(1 << ABITS) - 1];
      reg [15:0] dout;
      // one write port per bank: the two writers never target the same bank
      wire             we   = (wr_en && wr_bank == b) || (wr2_en && wr2_bank == b);
      wire             sel1 = wr_en && wr_bank == b;
      wire [ABITS-1:0] wa   = sel1 ? wr_addr : wr2_addr;
      wire [15:0]      wd   = sel1 ? wr_data : wr2_data;
      always @(posedge clk) begin
        if (we) mem[wa] <= wd;
        if (rd_en) dout <= mem[rd_addr[b*ABITS +: ABITS]];
      end
      assign rd_data[b*16 +: 16] = dout;
    end
  endgenerate

endmodule
