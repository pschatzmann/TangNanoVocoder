// Shares sdram_ctrl's port B between two masters with the same interface.
// A master owns the port from its request until the port's `done`; master
// 0 (the link, single-word writes) wins when both wait.
`timescale 1ns / 1ps

module vocoder_sdram_arb (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        m0_req, m0_we,
    input  wire [20:0] m0_addr,
    input  wire [8:0]  m0_len,
    input  wire [31:0] m0_wdata,
    input  wire [3:0]  m0_wbe,
    output wire        m0_ack, m0_wpull, m0_rvalid, m0_done,

    input  wire        m1_req, m1_we,
    input  wire [20:0] m1_addr,
    input  wire [8:0]  m1_len,
    input  wire [31:0] m1_wdata,
    input  wire [3:0]  m1_wbe,
    output wire        m1_ack, m1_wpull, m1_rvalid, m1_done,

    output wire        b_req, b_we,
    output wire [20:0] b_addr,
    output wire [8:0]  b_len,
    output wire [31:0] b_wdata,
    output wire [3:0]  b_wbe,
    input  wire        b_ack, b_wpull, b_rvalid, b_done
);
  reg owned, owner;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      owned <= 1'b0;
      owner <= 1'b0;
    end else if (!owned) begin
      if (m0_req) begin
        owned <= 1'b1;
        owner <= 1'b0;
      end else if (m1_req) begin
        owned <= 1'b1;
        owner <= 1'b1;
      end
    end else if (b_done) begin
      owned <= 1'b0;
    end
  end

  assign b_req   = owned && (owner ? m1_req : m0_req);
  assign b_we    = owner ? m1_we : m0_we;
  assign b_addr  = owner ? m1_addr : m0_addr;
  assign b_len   = owner ? m1_len : m0_len;
  assign b_wdata = owner ? m1_wdata : m0_wdata;
  assign b_wbe   = owner ? m1_wbe : m0_wbe;

  assign m0_ack    = owned && !owner && b_ack;
  assign m0_wpull  = owned && !owner && b_wpull;
  assign m0_rvalid = owned && !owner && b_rvalid;
  assign m0_done   = owned && !owner && b_done;
  assign m1_ack    = owned && owner && b_ack;
  assign m1_wpull  = owned && owner && b_wpull;
  assign m1_rvalid = owned && owner && b_rvalid;
  assign m1_done   = owned && owner && b_done;
endmodule
