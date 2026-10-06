// Streams a sentence's PCM (16-bit samples, two per word, from SDRAM word
// `base`) into vocoder_audio_out, through sdram_ctrl's read-only,
// prioritized port A. Bursts of up to 128 words go into a 512-word FIFO
// whenever it has room; the FIFO feeds the audio FIFO one sample per clock.
`timescale 1ns / 1ps

module vocoder_playback (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,
    input  wire [20:0] base,
    input  wire [18:0] samples,   // even
    output reg         busy,      // until the last sample is in the audio FIFO

    output reg         a_req,
    output reg  [20:0] a_addr,
    output reg  [8:0]  a_len,
    input  wire        a_ack,
    input  wire        a_rvalid,
    input  wire [31:0] a_rdata,
    input  wire        a_done,

    output wire        out_valid,
    input  wire        out_ready,
    output wire [15:0] out_sample
);
  // ---- word FIFO (block RAM, registered read: `head` valid once `head_ok`)
  (* ram_style = "block" *) reg [31:0] mem [0:511];
  reg [9:0]  wp, rp;
  wire [9:0] level = wp - rp;
  reg [31:0] head;
  reg        head_ok;
  reg        half;
  wire       take = head_ok && out_ready;           // a sample leaves
  wire       pop  = take && half;                   // ... and with it the word
  always @(posedge clk) begin
    if (a_rvalid) mem[wp[8:0]] <= a_rdata;
    head <= mem[rp[8:0]];
  end
  assign out_valid  = head_ok;
  assign out_sample = half ? head[31:16] : head[15:0];

  // ---- reader
  reg [17:0] words_left;   // still to request
  reg [18:0] samples_left; // still to hand out
  reg        in_flight;
  wire [8:0] to_row = 9'd256 - {1'b0, a_addr[7:0]};
  wire [17:0] lim = words_left < 18'd128 ? words_left : 18'd128;
  wire [8:0] len = lim < {9'd0, to_row} ? lim[8:0] : to_row;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      busy <= 1'b0;
      a_req <= 1'b0;
      in_flight <= 1'b0;
      wp <= 10'd0;
      rp <= 10'd0;
      head_ok <= 1'b0;
      half <= 1'b0;
      words_left <= 18'd0;
      samples_left <= 19'd0;
    end else begin
      if (a_rvalid) wp <= wp + 10'd1;
      head_ok <= level != 0 && !pop;
      if (take) begin
        half <= !half;
        samples_left <= samples_left - 19'd1;
        if (half) rp <= rp + 10'd1;
      end
      if (busy && samples_left == 0 && !in_flight && !a_req) busy <= 1'b0;

      if (start && !busy) begin
        busy <= 1'b1;
        a_addr <= base;
        words_left <= samples[18:1];
        samples_left <= samples;
      end else if (!in_flight && !a_req && words_left != 0 && level <= 10'd512 - 10'd128) begin
        a_req <= 1'b1;
        a_len <= len;
      end else if (a_req && a_ack) begin
        a_req <= 1'b0;
        in_flight <= 1'b1;
        a_addr <= a_addr + a_len;
        words_left <= words_left - a_len;
      end else if (in_flight && a_done) begin
        in_flight <= 1'b0;
      end
    end
  end
endmodule
