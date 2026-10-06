// Audio output of the vocoder: a sample FIFO played at SAMPLE_RATE, both to
// I2S (the Tang Nano 20K's onboard MAX98357A) and as a 1-bit sigma-delta
// stream for a plain output pin with an RC low-pass filter (the "PWM pin"
// option: same filter as PWM audio, far more resolution - a first-order
// modulator at 54MHz has an oversampling ratio of 1224 at 44.1kHz).
// The top level decides which of the two goes to pins.
//
// One timebase: a phase accumulator toggles BCLK at 64 x SAMPLE_RATE (32
// bits per I2S frame), exact on average, edges jitter by one clock. Each
// frame takes one sample from the FIFO; if it is empty, the output is
// silence and `underrun` pulses.
//
// I2S: Philips format, 16 bits per channel, the mono sample on both
// channels; WS changes one BCLK before each MSB.
`timescale 1ns / 1ps

module vocoder_audio_out #(
    parameter CLK_HZ      = 54_000_000,
    parameter SAMPLE_RATE = 44_100,
    parameter FIFO_ABITS  = 10
) (
    input  wire               clk,
    input  wire               rst_n,

    input  wire               in_valid,
    output wire               in_ready,
    input  wire signed [15:0] in_sample,
    output wire [FIFO_ABITS:0] level,  // samples queued

    output reg                i2s_bclk,
    output reg                i2s_ws,
    output reg                i2s_din,
    output reg                pwm_out,
    output reg                underrun
);
  // ---- FIFO
  (* ram_style = "block" *) reg [15:0] fifo [0:(1 << FIFO_ABITS) - 1];
  reg [FIFO_ABITS:0] wp, rp;
  assign level = wp - rp;
  assign in_ready = level != (1 << FIFO_ABITS);
  always @(posedge clk) if (in_valid && in_ready) fifo[wp[FIFO_ABITS-1:0]] <= in_sample;

  // registered read (block RAM): `head` is fifo[rp] once `head_ok`, which
  // drops for a clock after every pop while the next entry is read
  reg [15:0] head;
  reg        head_ok;
  wire       pop;
  always @(posedge clk) head <= fifo[rp[FIFO_ABITS-1:0]];

  // ---- timebase: BCLK toggles on accumulator overflow
  localparam [63:0] INC64 = ((64'd1 << 32) * SAMPLE_RATE * 64 + CLK_HZ / 2) / CLK_HZ;
  localparam [31:0] INC = INC64[31:0];
  reg [31:0] phase;
  wire [32:0] phase_next = {1'b0, phase} + INC;
  wire toggle = phase_next[32];

  reg [4:0]  slot;      // I2S bit slot of the frame being sent
  assign pop = toggle && i2s_bclk && slot == 5'd31 && head_ok;
  reg [15:0] sample;    // sample of the current frame

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      wp <= 0;
      rp <= 0;
      phase <= 32'd0;
      i2s_bclk <= 1'b0;
      i2s_ws <= 1'b0;
      i2s_din <= 1'b0;
      slot <= 5'd31;
      sample <= 16'd0;
      underrun <= 1'b0;
      head_ok <= 1'b0;
    end else begin
      underrun <= 1'b0;
      head_ok <= level != 0 && !pop;
      if (in_valid && in_ready) wp <= wp + 1'b1;
      phase <= phase_next[31:0];
      if (toggle) begin
        i2s_bclk <= !i2s_bclk;
        if (i2s_bclk) begin  // falling edge: next slot
          slot <= slot + 5'd1;
          i2s_ws <= (slot + 5'd1) >= 5'd16;
          if (slot == 5'd31) begin  // new frame
            i2s_din <= sample[0];  // previous frame's right LSB
            if (head_ok) begin
              sample <= head;
              rp <= rp + 1'b1;
            end else begin
              sample <= 16'd0;
              underrun <= 1'b1;
            end
          end else begin
            // slots 1..16: left MSB..LSB, 17..31: right MSB..bit 1
            i2s_din <= sample[15 - ((slot) & 5'd15)];
          end
        end
      end
    end
  end

  // ---- sigma-delta for the PWM pin: duty = (sample + 32768) / 65536
  reg [15:0] sd_acc;
  wire [16:0] sd_next = {1'b0, sd_acc} + {1'b0, sample ^ 16'h8000};
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      sd_acc <= 16'd0;
      pwm_out <= 1'b0;
    end else begin
      sd_acc <= sd_next[15:0];
      pwm_out <= sd_next[16];
    end
  end
endmodule
