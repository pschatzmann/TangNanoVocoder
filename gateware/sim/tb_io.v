// I/O test: the link protocol over UART and SPI (vocoder_link + receivers)
// and the audio output (I2S decoded back from the pins, sigma-delta duty).
//
//   iverilog -g2012 -o tb_io.vvp tb_io.v ../src/vocoder_{link,uart,spi_slave,audio_out}.v && vvp tb_io.vvp
`timescale 1ns / 1ps

module tb_io;
  localparam CLK_HZ = 54_000_000, BAUD = 921_600;
  localparam integer BIT_NS = 1_000_000_000 / BAUD;
  localparam FRAMES = 3;

  reg clk = 1'b0;
  always #9.259 clk = ~clk;  // 54MHz
  reg rst_n = 1'b0;
  integer errors = 0;

  task check(input cond, input [8*64-1:0] what);
    if (!cond) begin
      $display("  FAIL: %0s", what);
      errors = errors + 1;
    end
  endtask

  // ---------------------------------------------------------------- link
  reg  uart_rx = 1'b1;
  reg  sclk = 1'b0, mosi = 1'b0, cs_n = 1'b1;
  wire miso;
  reg  use_spi = 1'b0;

  wire u_valid, s_valid;
  wire [7:0] u_data, s_data;
  wire u_ferr;
  vocoder_uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_urx (
      .clk(clk), .rst_n(rst_n), .rx(uart_rx), .valid(u_valid), .data(u_data), .frame_error(u_ferr));

  wire [7:0] status;
  vocoder_spi_slave u_spi (
      .clk(clk), .rst_n(rst_n), .sclk(sclk), .mosi(mosi), .cs_n(cs_n), .miso(miso),
      .valid(s_valid), .data(s_data), .tx_data(status));

  // layout the link writes latents to (normally from the image header)
  localparam LAT0 = 1000, LAT1 = 2000, PLANE = 4;
  reg        hdr_valid = 1'b0;
  reg [1:0]  lat_full = 2'b00;
  wire       wslot, sent, sent_slot, prog_done, prog_valid, query, query_stats, readback;
  wire [12:0] sent_frames;
  wire [20:0] rb_addr;
  wire [15:0] rb_count;
  wire       m_req, m_we;
  wire [20:0] m_addr;
  wire [8:0] m_len;
  wire [31:0] m_wdata;
  wire [3:0] m_wbe;
  reg        m_ack = 1'b0, m_done = 1'b0;
  vocoder_link u_link (
      .clk(clk), .rst_n(rst_n),
      .rx_valid(use_spi ? s_valid : u_valid), .rx_data(use_spi ? s_data : u_data),
      .hdr_valid(hdr_valid), .max_frames(13'd8), .lat_base0(21'd1000), .lat_base1(21'd2000), .lat_plane(21'd4),
      .slot_free(!lat_full[wslot]), .busy(1'b0), .wslot(wslot),
      .sent(sent), .sent_slot(sent_slot), .sent_frames(sent_frames), .prog_done(prog_done),
      .prog_valid(prog_valid), .status(status), .query(query), .query_stats(query_stats),
      .readback(readback), .rb_addr(rb_addr), .rb_count(rb_count),
      .m_req(m_req), .m_we(m_we), .m_addr(m_addr), .m_len(m_len), .m_ack(m_ack), .m_wpull(m_ack),
      .m_wdata(m_wdata), .m_wbe(m_wbe), .m_done(m_done));

  // SDRAM port: ack a request, take its one word, done three clocks later
  reg [31:0] sdmem [0:4095];
  reg [1:0]  sd_wait = 2'd0;
  reg        sd_busy = 1'b0;
  integer    sd_writes = 0;
  always @(posedge clk) begin
    m_ack <= 1'b0;
    m_done <= 1'b0;
    if (!sd_busy && m_req && !m_ack) begin
      m_ack <= 1'b1;
      sd_busy <= 1'b1;
      sd_wait <= 2'd3;
      if (m_we) begin
        if (m_wbe[0]) sdmem[m_addr][7:0] <= m_wdata[7:0];
        if (m_wbe[1]) sdmem[m_addr][15:8] <= m_wdata[15:8];
        if (m_wbe[2]) sdmem[m_addr][23:16] <= m_wdata[23:16];
        if (m_wbe[3]) sdmem[m_addr][31:24] <= m_wdata[31:24];
        sd_writes = sd_writes + 1;
      end
    end else if (sd_busy) begin
      if (sd_wait == 0) begin
        m_done <= 1'b1;
        sd_busy <= 1'b0;
      end else begin
        sd_wait <= sd_wait - 2'd1;
      end
    end
  end

  // UART transmitter (status replies), looped back to a checker
  wire utx, utx_busy;
  vocoder_uart_tx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_utx (
      .clk(clk), .rst_n(rst_n), .start(query), .data(status), .busy(utx_busy), .tx(utx));
  wire r_valid;
  wire [7:0] r_data;
  vocoder_uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_loop (
      .clk(clk), .rst_n(rst_n), .rx(utx), .valid(r_valid), .data(r_data), .frame_error());
  reg [7:0] last_reply = 8'h00;
  always @(posedge clk) if (r_valid) last_reply <= r_data;

  integer sent_count = 0, stats_count = 0, rb_count_seen = 0, prog_count = 0;
  always @(posedge clk) begin
    if (sent) begin
      sent_count = sent_count + 1;
      lat_full[sent_slot] <= 1'b1;
    end
    if (query_stats) stats_count = stats_count + 1;
    if (readback) rb_count_seen = rb_count_seen + 1;
    if (prog_done) prog_count = prog_count + 1;
  end

  reg [7:0] spi_in;
  task send_byte(input [7:0] b);
    integer i;
    if (!use_spi) begin
      uart_rx = 1'b0;
      #(BIT_NS);
      for (i = 0; i < 8; i = i + 1) begin
        uart_rx = b[i];
        #(BIT_NS);
      end
      uart_rx = 1'b1;
      #(BIT_NS);
    end else begin
      for (i = 7; i >= 0; i = i - 1) begin
        mosi = b[i];
        #(150);
        sclk = 1'b1;
        spi_in = {spi_in[6:0], miso};
        #(150);
        sclk = 1'b0;
      end
      #(150);
    end
  endtask

  function signed [15:0] value(input integer n, input integer seed);
    value = (n * 977 + seed * 131) ^ (n << 7);
  endfunction

  task send_sentence(input integer seed, input bad_sum);
    integer n;
    reg [7:0] sum;
    reg signed [15:0] v;
    begin
      sum = 8'd0;
      if (use_spi) begin
        cs_n = 1'b0;
        #(200);
      end
      send_byte(8'h53);
      send_byte(FRAMES);
      send_byte(8'h00);
      for (n = 0; n < FRAMES * 32; n = n + 1) begin
        v = value(n, seed);
        send_byte(v[7:0]);
        send_byte(v[15:8]);
        sum = sum + v[7:0] + v[15:8];
      end
      send_byte(bad_sum ? sum + 8'd1 : sum);
      if (use_spi) begin
        #(200);
        cs_n = 1'b1;
      end
      #(2000);
    end
  endtask

  // latent (frame f, channel c) of a slot: half f & 1 of word base + c * PLANE + f / 2
  task check_latent(input integer seed, input integer base);
    integer n, bad;
    reg [31:0] w;
    reg [15:0] got;
    begin
      bad = 0;
      for (n = 0; n < FRAMES * 32; n = n + 1) begin
        w = sdmem[base + (n % 32) * PLANE + (n / 32) / 2];
        got = ((n / 32) % 2) ? w[31:16] : w[15:0];
        if (got !== value(n, seed)) bad = bad + 1;
      end
      check(bad == 0, "latent values in SDRAM");
    end
  endtask

  // ---------------------------------------------------------------- audio
  reg               a_valid = 1'b0;
  reg signed [15:0] a_sample;
  wire              a_ready, bclk, ws, din, pwm, underrun;
  wire [10:0]       level;
  vocoder_audio_out #(.CLK_HZ(CLK_HZ), .SAMPLE_RATE(44_100), .FIFO_ABITS(10)) u_audio (
      .clk(clk), .rst_n(rst_n), .in_valid(a_valid), .in_ready(a_ready), .in_sample(a_sample), .level(level),
      .i2s_bclk(bclk), .i2s_ws(ws), .i2s_din(din), .pwm_out(pwm), .underrun(underrun));

  // I2S receiver: sample DIN on BCLK rising; a word ends with the bit
  // during which WS changes (Philips: WS leads the MSB by one bit)
  reg [31:0] i2s_shift;
  reg        ws_d = 1'b0;
  reg signed [15:0] word, left_rx;
  integer    frames_rx = 0;  // frames captured since `capture` was set
  reg        capture = 1'b0;
  reg signed [15:0] heard [0:63];
  always @(posedge bclk) begin
    i2s_shift <= {i2s_shift[30:0], din};
    ws_d <= ws;
    if (ws != ws_d) begin
      word = {i2s_shift[14:0], din};
      if (ws) begin
        left_rx = word;
      end else begin
        if (left_rx !== word) begin
          $display("  FAIL: I2S left %0d != right %0d", left_rx, word);
          errors = errors + 1;
        end
        if (capture) begin
          if (frames_rx < 64) heard[frames_rx] = word;
          frames_rx = frames_rx + 1;
        end
      end
    end
  end

  integer pwm_high = 0, pwm_total = 0;
  reg count_pwm = 1'b0;
  always @(posedge clk) if (count_pwm) begin
    pwm_total = pwm_total + 1;
    if (pwm) pwm_high = pwm_high + 1;
  end

  // ---------------------------------------------------------------- sequence
  integer i, k, first;
  real duty, expected;
  initial begin
    repeat (5) @(posedge clk);
    rst_n = 1'b1;
    repeat (5) @(posedge clk);

    $display("UART: program upload");
    check(status == 8'h80, "status before a program");
    send_byte(8'h50);
    send_byte(8'd8);
    send_byte(8'd0);
    send_byte(8'd0);
    send_byte(8'd0);
    send_byte(8'h11); send_byte(8'h22); send_byte(8'h33); send_byte(8'h44);
    send_byte(8'h55); send_byte(8'h66); send_byte(8'h77); send_byte(8'h88);
    send_byte(8'h64);  // 0x11 + ... + 0x88 = 0x264
    #(2000);
    check(prog_count == 1 && prog_valid, "program accepted");
    check(sdmem[0] == 32'h44332211 && sdmem[1] == 32'h88776655, "program words in SDRAM");
    hdr_valid = 1'b1;
    #(100);
    check(status == 8'h91, "status ready with a program");

    $display("UART: sentence");
    send_sentence(1, 1'b0);
    check(sent_count == 1 && sent_frames == FRAMES, "sentence complete");
    check(!lat_full[1] && lat_full[0], "into latent slot 0");
    check_latent(1, LAT0);

    $display("UART: bad checksum");
    send_sentence(2, 1'b1);
    check(sent_count == 1, "no sentence after a bad checksum");
    check(status[3], "error flag");

    $display("UART: status query, statistics, readback");
    send_byte(8'h3F);
    #(12 * BIT_NS);
    check(last_reply == status, "status reply over UART");
    send_byte(8'h54);
    check(stats_count == 1, "'T' recognized");
    send_byte(8'h52);
    send_byte(8'h34); send_byte(8'h12); send_byte(8'h01); send_byte(8'h00);
    send_byte(8'h05); send_byte(8'h00);
    #(100);
    check(rb_count_seen == 1 && rb_addr == 21'h11234 && rb_count == 16'd5, "'R' arguments");

    $display("SPI: sentence");
    use_spi = 1'b1;
    send_sentence(3, 1'b0);
    check(sent_count == 2, "sentence over SPI");
    check(lat_full == 2'b11, "into latent slot 1");
    check_latent(3, LAT1);
    check(status == 8'h90, "status: program, not ready (both slots full)");
    cs_n = 1'b0;
    #(200);
    send_byte(8'h00);
    cs_n = 1'b1;
    check(spi_in == status, "status on MISO");

    $display("SPI: not ready");
    send_sentence(4, 1'b0);
    check(sent_count == 2, "sentence dropped while not ready");
    check(status[3], "error flag after a dropped sentence");
    check_latent(3, LAT1);
    lat_full = 2'b00;

    $display("audio: I2S and sigma-delta");
    capture = 1'b1;
    for (i = 0; i < 40; i = i + 1) begin
      @(negedge clk);
      a_valid = 1'b1;
      a_sample = (i == 0) ? 16'sd0 : (i * 1500 - 30000);
    end
    @(negedge clk);
    a_valid = 1'b0;
    // wait for the 40 frames plus a few of silence
    wait (frames_rx >= 44);
    first = -1;
    for (k = 0; k < 44 && first < 0; k = k + 1) if (heard[k] == -16'sd28500) first = k - 1;
    check(first >= 0, "found the sample sequence on I2S");
    for (i = 1; i < 40; i = i + 1) check(heard[first + i] == i * 1500 - 30000, "I2S sample order and value");
    check(level == 0, "FIFO drained");

    // sigma-delta duty for a constant sample
    a_sample = 16'sd16384;  // duty 0.75, queued long enough for the measurement
    for (i = 0; i < 20; i = i + 1) begin
      @(negedge clk);
      a_valid = 1'b1;
    end
    @(negedge clk);
    a_valid = 1'b0;
    wait (u_audio.sample == 16'sd16384);
    count_pwm = 1'b1;
    repeat (4000) @(posedge clk);  // a multiple of the pattern length (4 for 0.75)
    count_pwm = 1'b0;
    duty = pwm_high * 1.0 / pwm_total;
    check(duty > 0.7495 && duty < 0.7505, "sigma-delta duty");
    $display("  sigma-delta duty %f for 0.75", duty);

    $display("%s: %0d errors", errors == 0 ? "PASS" : "FAIL", errors);
    $finish;
  end

  initial begin
    #(200_000_000);
    $display("FAIL: timeout");
    $finish;
  end
endmodule
