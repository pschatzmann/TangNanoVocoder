// The whole vocoder without clocks and pins (vocoder_top.v adds those):
//
//   SPI / header UART / USB UART -> vocoder_link -> SDRAM (program, latents)
//   vocoder_core: latent slot -> vocoder -> PCM slot
//   vocoder_playback: PCM slot -> vocoder_audio_out -> I2S and sigma-delta pin
//
// Two latent slots and two PCM slots, so the next sentence can arrive while
// one is computed, and one can be computed while the previous one plays.
//
// Replies: '?' -> status byte; 'T' -> status, then the last sentence's
// total cycles and engine cycles (u32 little endian each) and the PCM slot
// it was written to (0/1), all on the interface the request came in on. Over SPI the reply bytes come out on
// MISO with the following bytes the master clocks (status when none).
// 'R' (readback): the words one by one, 4 bytes each, through the same
// reply queue (an SPI master must clock slowly enough, about 1 MHz, for
// each word to be fetched in time).
`timescale 1ns / 1ps

module vocoder_system #(
    parameter CLK_HZ      = 54_000_000,
    parameter BAUD        = 921_600,
    parameter SAMPLE_RATE = 44_100,
    parameter INIT_US     = 200,
    parameter TANH_FILE   = "vocoder_tanh.hex",
    parameter RD_LAT      = 1     // SDRAM read sample point after reset (sdram_ctrl.v rd_lat); 'L' changes it
) (
    input  wire        clk,
    input  wire        clk_sdram,   // clk, 180 degrees
    input  wire        rst_n,

    input  wire        spi_sclk, spi_mosi, spi_cs_n,
    output wire        spi_miso,
    input  wire        uart_rx,
    output wire        uart_tx,
    input  wire        usb_uart_rx,
    output wire        usb_uart_tx,

    output wire        i2s_bclk, i2s_ws, i2s_din,
    output wire        pwm_out,

    output wire [5:0]  state_leds,  // active high: program, receiving, computing, playing, error, ready

    inout  wire [31:0] SDRAM_DQ,
    output wire [10:0] SDRAM_A,
    output wire [1:0]  SDRAM_BA,
    output wire        SDRAM_nCS, SDRAM_nWE, SDRAM_nRAS, SDRAM_nCAS, SDRAM_CLK, SDRAM_CKE,
    output wire [3:0]  SDRAM_DQM
);
  // ---------------------------------------------------------------- serial in
  wire       s_valid, u_valid, b_valid;
  wire [7:0] s_data, u_data, b_data;
  wire [7:0] spi_tx;
  vocoder_spi_slave u_spi (
      .clk(clk), .rst_n(rst_n), .sclk(spi_sclk), .mosi(spi_mosi), .cs_n(spi_cs_n), .miso(spi_miso),
      .valid(s_valid), .data(s_data), .tx_data(spi_tx));
  vocoder_uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_urx (
      .clk(clk), .rst_n(rst_n), .rx(uart_rx), .valid(u_valid), .data(u_data), .frame_error());
  vocoder_uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_brx (
      .clk(clk), .rst_n(rst_n), .rx(usb_uart_rx), .valid(b_valid), .data(b_data), .frame_error());

  // one byte stream; the interface of the last byte gets the replies
  wire       rx_valid = s_valid | u_valid | b_valid;
  wire [7:0] rx_data  = s_valid ? s_data : (u_valid ? u_data : b_data);
  reg  [1:0] last_if;  // 0 SPI, 1 header UART, 2 USB UART
  always @(posedge clk) if (rx_valid) last_if <= s_valid ? 2'd0 : (u_valid ? 2'd1 : 2'd2);

  // ---------------------------------------------------------------- slots
  reg [1:0]  lat_full, pcm_full;
  reg [12:0] lat_frames0, lat_frames1;
  reg [18:0] pcm_samples0, pcm_samples1;
  reg        rslot, cslot, pslot;  // core reads latent, core writes PCM, playback reads PCM
  reg        last_pcm_slot;        // where the last sentence's PCM went

  // ---------------------------------------------------------------- core
  wire        hdr_valid;
  wire [12:0] max_frames;
  wire [20:0] lat_base0, lat_base1, lat_plane, pcm_base0, pcm_base1;
  wire        core_busy, core_done;
  wire [31:0] run_cycles, mac_cycles;
  wire        prog_done, prog_valid;
  reg         go;

  wire        c_req, c_we, c_ack, c_wpull, c_rvalid, c_done;
  wire [20:0] c_addr;
  wire [8:0]  c_len;
  wire [31:0] c_wdata;
  wire [3:0]  c_wbe;
  wire [31:0] rdata;

  vocoder_core #(.TANH_FILE(TANH_FILE)) u_core (
      .clk(clk), .rst_n(rst_n),
      .hdr_load(prog_done), .hdr_valid(hdr_valid), .max_frames(max_frames),
      .lat_base0(lat_base0), .lat_base1(lat_base1), .lat_plane(lat_plane),
      .pcm_base0(pcm_base0), .pcm_base1(pcm_base1),
      .go(go), .frames(rslot ? lat_frames1 : lat_frames0),
      .lat_off(rslot ? lat_base1 - lat_base0 : 21'd0), .pcm_off(cslot ? pcm_base1 - pcm_base0 : 21'd0),
      .busy(core_busy), .done(core_done), .run_cycles(run_cycles), .mac_cycles(mac_cycles), .dbg(core_dbg),
      .m_req(c_req), .m_we(c_we), .m_addr(c_addr), .m_len(c_len), .m_ack(c_ack), .m_wpull(c_wpull),
      .m_wdata(c_wdata), .m_wbe(c_wbe), .m_rvalid(c_rvalid), .m_rdata(rdata), .m_done(c_done));

  // ---------------------------------------------------------------- link
  wire        wslot, sent, sent_slot, query, query_stats, readback, query_debug, lat_set;
  wire [2:0]  lat_value;
  reg  [2:0]  rd_lat;
  wire [39:0] core_dbg;
  always @(posedge clk or negedge rst_n)
    if (!rst_n) rd_lat <= RD_LAT;
    else if (lat_set) rd_lat <= lat_value;
  wire [20:0] rb_addr;
  wire [15:0] rb_count;
  wire [12:0] sent_frames;
  wire [7:0]  status;
  wire        pb_busy;
  wire        l_req, l_we, l_ack, l_wpull, l_done;
  wire [20:0] l_addr;
  wire [8:0]  l_len;
  wire [31:0] l_wdata;
  wire [3:0]  l_wbe;
  wire        any_busy = core_busy || go || pb_busy || pcm_full != 2'b00 || lat_full != 2'b00;

  vocoder_link u_link (
      .clk(clk), .rst_n(rst_n), .rx_valid(rx_valid), .rx_data(rx_data),
      .hdr_valid(hdr_valid), .max_frames(max_frames), .lat_base0(lat_base0), .lat_base1(lat_base1),
      .lat_plane(lat_plane), .slot_free(!lat_full[wslot]), .busy(any_busy), .wslot(wslot),
      .sent(sent), .sent_slot(sent_slot), .sent_frames(sent_frames), .prog_done(prog_done),
      .prog_valid(prog_valid), .status(status), .query(query), .query_stats(query_stats),
      .readback(readback), .rb_addr(rb_addr), .rb_count(rb_count),
      .query_debug(query_debug), .lat_set(lat_set), .lat_value(lat_value),
      .m_req(l_req), .m_we(l_we), .m_addr(l_addr), .m_len(l_len), .m_ack(l_ack), .m_wpull(l_wpull),
      .m_wdata(l_wdata), .m_wbe(l_wbe), .m_done(l_done));

  // ---------------------------------------------------------------- playback, audio out
  wire        a_req, a_ack, a_rvalid, a_done;
  wire [20:0] a_addr;
  wire [8:0]  a_len;
  wire        pb_valid, pb_ready;
  wire [15:0] pb_sample;
  reg         pb_start;
  vocoder_playback u_pb (
      .clk(clk), .rst_n(rst_n), .start(pb_start), .base(pslot ? pcm_base1 : pcm_base0),
      .samples(pslot ? pcm_samples1 : pcm_samples0), .busy(pb_busy),
      .a_req(a_req), .a_addr(a_addr), .a_len(a_len), .a_ack(a_ack), .a_rvalid(a_rvalid), .a_rdata(rdata),
      .a_done(a_done), .out_valid(pb_valid), .out_ready(pb_ready), .out_sample(pb_sample));

  wire underrun;
  vocoder_audio_out #(.CLK_HZ(CLK_HZ), .SAMPLE_RATE(SAMPLE_RATE), .FIFO_ABITS(10)) u_audio (
      .clk(clk), .rst_n(rst_n), .in_valid(pb_valid), .in_ready(pb_ready), .in_sample(pb_sample), .level(),
      .i2s_bclk(i2s_bclk), .i2s_ws(i2s_ws), .i2s_din(i2s_din), .pwm_out(pwm_out), .underrun(underrun));

  // ---------------------------------------------------------------- slot bookkeeping
  reg pb_started;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      lat_full <= 2'b00;
      pcm_full <= 2'b00;
      rslot <= 1'b0;
      cslot <= 1'b0;
      pslot <= 1'b0;
      go <= 1'b0;
      pb_start <= 1'b0;
      pb_started <= 1'b0;
    end else begin
      go <= 1'b0;
      pb_start <= 1'b0;
      if (sent) begin
        lat_full[sent_slot] <= 1'b1;
        if (sent_slot) lat_frames1 <= sent_frames;
        else lat_frames0 <= sent_frames;
      end
      // not in the clock `done` comes: the slots are only updated with it
      if (!core_busy && !go && !core_done && hdr_valid && lat_full[rslot] && !pcm_full[cslot]) go <= 1'b1;
      if (core_done) begin
        last_pcm_slot <= cslot;
        lat_full[rslot] <= 1'b0;
        rslot <= !rslot;
        pcm_full[cslot] <= 1'b1;
        // frames x 512 samples (fits 19 bits for up to 1023 frames)
        if (cslot) pcm_samples1 <= {(rslot ? lat_frames1[9:0] : lat_frames0[9:0]), 9'd0};
        else pcm_samples0 <= {(rslot ? lat_frames1[9:0] : lat_frames0[9:0]), 9'd0};
        cslot <= !cslot;
      end
      if (!pb_busy && !pb_start && !pb_started && pcm_full[pslot]) begin
        pb_start <= 1'b1;
        pb_started <= 1'b1;
      end
      if (pb_started && !pb_start && !pb_busy) begin  // finished
        pb_started <= 1'b0;
        pcm_full[pslot] <= 1'b0;
        pslot <= !pslot;
      end
    end
  end

  // ---------------------------------------------------------------- replies
  reg [7:0] rq [0:15];
  reg [4:0] rq_n, rq_i;
  reg [1:0] rq_if;
  wire      rq_any = rq_i != rq_n;
  wire [7:0] rq_head = rq[rq_i];
  // SPI: the next byte clocked out is the head of the reply, else status
  assign spi_tx = (rq_any && rq_if == 2'd0) ? rq_head : status;
  wire       ut_busy, bt_busy;
  wire       ut_start = rq_any && rq_if == 2'd1 && !ut_busy;
  wire       bt_start = rq_any && rq_if == 2'd2 && !bt_busy;
  reg        ut_start_d, bt_start_d;
  vocoder_uart_tx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_utx (
      .clk(clk), .rst_n(rst_n), .start(ut_start && !ut_start_d), .data(rq_head), .busy(ut_busy), .tx(uart_tx));
  vocoder_uart_tx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_btx (
      .clk(clk), .rst_n(rst_n), .start(bt_start && !bt_start_d), .data(rq_head), .busy(bt_busy), .tx(usb_uart_tx));

  // readback: one word at a time through the link's SDRAM port
  reg         rb_active, rb_req, rb_wait;
  reg  [20:0] rb_a;
  reg  [15:0] rb_left;
  reg  [1:0]  rb_if;
  wire        m0_ack, m0_wpull, m0_rvalid, m0_done;
  assign l_ack   = !rb_active && m0_ack;
  assign l_wpull = !rb_active && m0_wpull;
  assign l_done  = !rb_active && m0_done;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rq_n <= 5'd0;
      rq_i <= 5'd0;
      ut_start_d <= 1'b0;
      bt_start_d <= 1'b0;
      rb_active <= 1'b0;
      rb_req <= 1'b0;
      rb_wait <= 1'b0;
    end else begin
      if (readback && rb_count != 0) begin
        rb_active <= 1'b1;
        rb_a <= rb_addr;
        rb_left <= rb_count;
        rb_if <= last_if;
      end
      if (rb_active && !rb_req && !rb_wait && !rq_any && !(query || query_stats)) rb_req <= 1'b1;
      if (rb_active && rb_req && m0_ack) begin
        rb_req <= 1'b0;
        rb_wait <= 1'b1;
      end
      if (rb_active && m0_done) begin
        rb_wait <= 1'b0;
        rb_a <= rb_a + 21'd1;
        rb_left <= rb_left - 16'd1;
        if (rb_left == 16'd1) rb_active <= 1'b0;
      end
      ut_start_d <= ut_start;
      bt_start_d <= bt_start;
      // a byte leaves: UART when its transmitter takes it, SPI when the
      // master has clocked a byte (the value was loaded at its start)
      if ((ut_start && !ut_start_d) || (bt_start && !bt_start_d) || (rq_any && rq_if == 2'd0 && s_valid))
        rq_i <= rq_i + 5'd1;
      if (query || query_stats) begin
        rq[0] <= status;
        {rq[4], rq[3], rq[2], rq[1]} <= run_cycles;
        {rq[8], rq[7], rq[6], rq[5]} <= mac_cycles;
        rq[9] <= {7'd0, last_pcm_slot};
        rq_n <= query_stats ? 5'd10 : 5'd1;
        rq_i <= 5'd0;
        rq_if <= last_if;
      end
      if (query_debug) begin
        // 'D': state, op, op count, op table (2), rd_lat, max frames (2),
        // latent base 0 (3), latent plane (2), slots, flags, frames of slot 0
        {rq[0], rq[1], rq[2], rq[4], rq[3]} <= core_dbg;
        rq[5] <= {5'd0, rd_lat};
        {rq[7], rq[6]} <= {3'd0, max_frames};
        {rq[10], rq[9], rq[8]} <= {3'd0, lat_base0};
        {rq[12], rq[11]} <= lat_plane[15:0];
        rq[13] <= {lat_full, pcm_full, wslot, rslot, cslot, pslot};
        rq[14] <= {2'd0, pb_busy, core_busy, hdr_valid, prog_valid, go, last_pcm_slot};
        rq[15] <= lat_frames0[7:0];
        rq_n <= 5'd16;
        rq_i <= 5'd0;
        rq_if <= last_if;
      end
      if (rb_active && m0_rvalid) begin
        {rq[3], rq[2], rq[1], rq[0]} <= rdata;
        rq_n <= 5'd4;
        rq_i <= 5'd0;
        rq_if <= rb_if;
      end
    end
  end

  assign state_leds = {status[0], status[3], pb_busy, core_busy, status[1], prog_valid};

  // ---------------------------------------------------------------- SDRAM
  wire        b_req, b_we, b_ack, b_wpull, b_rvalid, b_done;
  wire [20:0] b_addr;
  wire [8:0]  b_len;
  wire [31:0] b_wdata;
  wire [3:0]  b_wbe;
  vocoder_sdram_arb u_arb (
      .clk(clk), .rst_n(rst_n),
      .m0_req(rb_active ? rb_req : l_req), .m0_we(rb_active ? 1'b0 : l_we), .m0_addr(rb_active ? rb_a : l_addr),
      .m0_len(rb_active ? 9'd1 : l_len), .m0_wdata(l_wdata), .m0_wbe(l_wbe),
      .m0_ack(m0_ack), .m0_wpull(m0_wpull), .m0_rvalid(m0_rvalid), .m0_done(m0_done),
      .m1_req(c_req), .m1_we(c_we), .m1_addr(c_addr), .m1_len(c_len), .m1_wdata(c_wdata), .m1_wbe(c_wbe),
      .m1_ack(c_ack), .m1_wpull(c_wpull), .m1_rvalid(c_rvalid), .m1_done(c_done),
      .b_req(b_req), .b_we(b_we), .b_addr(b_addr), .b_len(b_len), .b_wdata(b_wdata), .b_wbe(b_wbe),
      .b_ack(b_ack), .b_wpull(b_wpull), .b_rvalid(b_rvalid), .b_done(b_done));

  wire sdram_ready;
  sdram_ctrl #(.FREQ(CLK_HZ), .INIT_US(INIT_US)) u_sdram (
      .clk(clk), .clk_sdram(clk_sdram), .rst(!rst_n), .rd_lat(rd_lat), .ready(sdram_ready),
      .a_req(a_req && sdram_ready), .a_addr(a_addr), .a_len(a_len), .a_ack(a_ack), .a_rvalid(a_rvalid),
      .a_done(a_done),
      .b_req(b_req && sdram_ready), .b_we(b_we), .b_addr(b_addr), .b_len(b_len), .b_wdata(b_wdata),
      .b_wbe(b_wbe), .b_ack(b_ack), .b_wpull(b_wpull), .b_rvalid(b_rvalid), .b_done(b_done),
      .rdata(rdata),
      .SDRAM_DQ(SDRAM_DQ), .SDRAM_A(SDRAM_A), .SDRAM_BA(SDRAM_BA), .SDRAM_nCS(SDRAM_nCS),
      .SDRAM_nWE(SDRAM_nWE), .SDRAM_nRAS(SDRAM_nRAS), .SDRAM_nCAS(SDRAM_nCAS), .SDRAM_CLK(SDRAM_CLK),
      .SDRAM_CKE(SDRAM_CKE), .SDRAM_DQM(SDRAM_DQM));
endmodule
