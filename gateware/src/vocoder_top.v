// Tang Nano 20K top level of the vocoder: PLL, reset, pins
// (gateware/constraints/tangnano20k.cst), vocoder_system.
//
// The SDRAM ports are named so nextpnr binds them to the package's internal
// SDRAM (see arduino-tangnano20k's docs/ARCHITECTURE.md).
//
// LEDs (active low): 0 program loaded, 1 receiving, 2 computing, 3 playing,
// 4 error, 5 ready for a sentence.
`timescale 1ns / 1ps

`ifndef VOCODER_CLK_HZ
`define VOCODER_CLK_HZ 54000000
`define VOCODER_PLL_IDIV 0
`define VOCODER_PLL_FBDIV 1
`define VOCODER_PLL_ODIV 16
`endif

module vocoder_top (
    input  wire        clk_27m,
    input  wire        reset_button,   // S1, active high

    output wire [5:0]  leds,

    output wire        i2s_pa_en,
    output wire        i2s_bclk,
    output wire        i2s_ws,
    output wire        i2s_din,
    output wire        pwm_out,

    input  wire        spi_sclk,
    input  wire        spi_mosi,
    input  wire        spi_cs_n,
    output wire        spi_miso,

    input  wire        uart_rx,
    output wire        uart_tx,
    input  wire        usb_uart_rx,
    output wire        usb_uart_tx,

    inout  wire [31:0] IO_sdram_dq,
    output wire [10:0] O_sdram_addr,
    output wire [1:0]  O_sdram_ba,
    output wire        O_sdram_cs_n,
    output wire        O_sdram_wen_n,
    output wire        O_sdram_ras_n,
    output wire        O_sdram_cas_n,
    output wire        O_sdram_clk,
    output wire        O_sdram_cke,
    output wire [3:0]  O_sdram_dqm
);
  wire clk, clk_sdram, locked;
  vocoder_pll u_pll (.clock_in(clk_27m), .clock_out(clk), .clock_p180(clk_sdram), .locked(locked));

  reg [2:0] rst_sync = 3'b000;
  always @(posedge clk) rst_sync <= {rst_sync[1:0], locked && !reset_button};
  wire rst_n = rst_sync[2];

  wire miso;
  wire [5:0] state;
  // RD_LAT 5: reads sampled half a clock earlier than the simulation model
  // does (sdram_ctrl.v) - between the two whole-clock points, which both
  // mixed neighbouring words on a Tang Nano 20K at 54 MHz. 'L' changes it.
  vocoder_system #(.CLK_HZ(`VOCODER_CLK_HZ), .RD_LAT(5)) u_sys (
      .clk(clk), .clk_sdram(clk_sdram), .rst_n(rst_n),
      .spi_sclk(spi_sclk), .spi_mosi(spi_mosi), .spi_cs_n(spi_cs_n), .spi_miso(miso),
      .uart_rx(uart_rx), .uart_tx(uart_tx), .usb_uart_rx(usb_uart_rx), .usb_uart_tx(usb_uart_tx),
      .i2s_bclk(i2s_bclk), .i2s_ws(i2s_ws), .i2s_din(i2s_din), .pwm_out(pwm_out), .state_leds(state),
      .SDRAM_DQ(IO_sdram_dq), .SDRAM_A(O_sdram_addr), .SDRAM_BA(O_sdram_ba), .SDRAM_nCS(O_sdram_cs_n),
      .SDRAM_nWE(O_sdram_wen_n), .SDRAM_nRAS(O_sdram_ras_n), .SDRAM_nCAS(O_sdram_cas_n),
      .SDRAM_CLK(O_sdram_clk), .SDRAM_CKE(O_sdram_cke), .SDRAM_DQM(O_sdram_dqm));

  assign spi_miso = spi_cs_n ? 1'bz : miso;
  assign i2s_pa_en = 1'b1;
  assign leds = ~state;
endmodule

// 27 MHz -> system clock, 27 * (FBDIV + 1) / (IDIV + 1), VCO = output * ODIV,
// plus the same clock shifted by 180 degrees for the SDRAM. Settings as in
// TangNanoFaust's pll_sys.v (54 MHz: 0 1 16; 64.8 MHz: 4 11 8, TangNanoGPU).
module vocoder_pll (
    input  wire clock_in,
    output wire clock_out,
    output wire clock_p180,
    output wire locked
);
`ifdef SYNTHESIS
  rPLL #(
      .FCLKIN("27"),
      .IDIV_SEL(`VOCODER_PLL_IDIV),
      .FBDIV_SEL(`VOCODER_PLL_FBDIV),
      .ODIV_SEL(`VOCODER_PLL_ODIV),
      .PSDA_SEL("1000"),
      .DUTYDA_SEL("1000"),
      .CLKOUT_FT_DIR(1'b1),
      .CLKOUTP_FT_DIR(1'b1),
      .CLKFB_SEL("internal"),
      .CLKOUT_BYPASS("false"),
      .CLKOUTP_BYPASS("false"),
      .DYN_DA_EN("false"),
      .DEVICE("GW2AR-18C")
  ) pll (
      .CLKOUTD(), .CLKOUTD3(), .RESET(1'b0), .RESET_P(1'b0), .CLKFB(1'b0),
      .FBDSEL(6'b0), .IDSEL(6'b0), .ODSEL(6'b0), .PSDA(4'b0), .DUTYDA(4'b0), .FDLY(4'b0),
      .CLKIN(clock_in), .CLKOUT(clock_out), .CLKOUTP(clock_p180), .LOCK(locked)
  );
`else
  assign clock_out  = clock_in;
  assign clock_p180 = ~clock_in;
  assign locked     = 1'b1;
`endif
endmodule
