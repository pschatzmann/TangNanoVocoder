// Convolution engine of the vocoder: computes one Conv1d or ConvTranspose1d
// layer (or a range of its output frames) from an activation tile in
// vocoder_act_banks and a weight RAM, and streams out the raw accumulator
// of every output (t, co). Bias, requantize, residual and tanh are
// vocoder_post's job. See docs/gateware.md.
//
// LANES (16) lanes compute LANES output frames of one output channel at
// once. Each clock is one (tap, input channel) step: every lane reads its
// own input frame (consecutive frames -> one bank each) and all lanes share
// the step's weight, so the weight RAM needs one read per clock and every
// lane is busy whatever the channel count.
//
// Transposed convolution is computed as a gather: output t reads input
// ti = (t + pad - kk) / stride for the taps kk = (t + pad) % stride + m *
// stride. Outputs of one phase r = (t + pad) % stride with consecutive
// q = (t + pad) / stride read consecutive input frames q - m, so a lane
// block is LANES outputs of one phase, stride frames apart.
//
// The input tile must already be activated (leaky ReLU is applied when the
// tile is loaded) and must hold every in-sequence frame the requested
// outputs read; frames outside [0, cfg_tin) read as zero (padding).
//
// Requirements: cin (up to 128) and stride powers of two, k >= stride,
// k <= 16.
//
// Output channels cfg_co0 .. cfg_co0 + cfg_cocount - 1 are computed, with
// their weights at the start of the weight RAM, layout [co - co0][kk][ci]:
// address (co - co0) * k * cin + kk * cin + ci.
`timescale 1ns / 1ps

module vocoder_conv_engine #(
    parameter LANES  = 16,  // must be 16 (4-bit bank index)
    parameter ABITS  = 10,  // bank depth
    parameter WABITS = 15,  // weight RAM address width
    parameter TBITS  = 20,  // frame index width
    parameter ACCW   = 36   // accumulator width: exact for every layer of the model
) (
    input  wire                     clk,
    input  wire                     rst_n,

    input  wire                     start,  // pulse: configuration below is stable until !busy
    output reg                      busy,

    input  wire                     cfg_transposed,
    input  wire [2:0]               cfg_cin_log2,
    input  wire [6:0]               cfg_co0,          // first output channel
    input  wire [7:0]               cfg_cocount,      // number of output channels (up to 128)
    input  wire [4:0]               cfg_k,
    input  wire [2:0]               cfg_dil,
    input  wire [2:0]               cfg_stride_log2,  // transposed only
    input  wire [5:0]               cfg_pad,
    input  wire [TBITS-1:0]         cfg_tin,          // input frames in the sequence
    input  wire [TBITS-1:0]         cfg_tile_base,    // frame held at local index 0 of the banks
    input  wire [TBITS-1:0]         cfg_t0,           // first output frame to compute
    input  wire [TBITS-1:0]         cfg_tcount,       // number of output frames

    // activation banks (vocoder_act_banks), 1-clock read latency
    output wire                     act_en,
    output wire [LANES*ABITS-1:0]   act_addr,
    input  wire [LANES*16-1:0]      act_data,

    // weight RAM, 1-clock read latency
    output wire                     w_en,
    output wire [WABITS-1:0]        w_addr,
    input  wire [15:0]              w_data,

    // results, one (t, co) per clock while res_ready
    output wire                     res_valid,
    input  wire                     res_ready,
    output wire [6:0]               res_co,
    output wire [TBITS-1:0]         res_t,
    output wire signed [ACCW-1:0]   res_acc
);

  localparam SW = TBITS + 2;  // signed frame arithmetic

  // ---------------------------------------------------------------- sequencer
  localparam S_IDLE = 2'd0, S_SETUP = 2'd1, S_RUN = 2'd2, S_FLUSH = 2'd3;
  reg [1:0] state;

  reg [6:0]             co;
  reg [3:0]             r;          // transposed: phase
  reg [WABITS-1:0]      w_base;     // co * k * cin
  reg signed [SW-1:0]   blk;        // conv: first output frame of the block; transposed: first q
  reg signed [SW-1:0]   blk_end;    // exclusive
  reg signed [SW-1:0]   f_tap;      // input frame of lane 0 for the current tap
  reg [4:0]             tap, n_taps;
  reg [4:0]             kk_w;       // weight tap index
  reg [6:0]             ci;
  wire [7:0]            co_end = {1'b0, cfg_co0} + cfg_cocount;

  wire [7:0]  cin       = 8'd1 << cfg_cin_log2;
  wire [4:0]  stride    = 5'd1 << cfg_stride_log2;
  wire signed [SW-1:0] s_t0   = $signed({2'b00, cfg_t0});
  wire signed [SW-1:0] s_tend = $signed({2'b00, cfg_t0}) + $signed({2'b00, cfg_tcount});
  wire signed [SW-1:0] s_pad  = $signed({{(SW-6){1'b0}}, cfg_pad});
  wire signed [SW-1:0] s_r    = $signed({{(SW-4){1'b0}}, r});
  wire [WABITS-1:0] layer_row = {{(WABITS-5){1'b0}}, cfg_k} << cfg_cin_log2;  // k * cin

  // transposed: q range of phase r
  wire signed [SW-1:0] q_lo = (s_t0 + s_pad - s_r + $signed({{(SW-5){1'b0}}, stride}) - 1) >>> cfg_stride_log2;
  wire signed [SW-1:0] q_hi = (s_tend - 1 + s_pad - s_r) >>> cfg_stride_log2;
  wire [4:0] taps_t = (cfg_k - {1'b0, r} + stride - 5'd1) >> cfg_stride_log2;

  wire signed [SW-1:0] blk_next = blk + LANES;
  wire last_ci  = (ci == cin - 1);
  wire last_tap = (tap == n_taps - 1);
  wire issue;  // a step enters the pipeline this clock

  function signed [SW-1:0] tap0_frame(input signed [SW-1:0] b);
    tap0_frame = cfg_transposed ? b : b - s_pad;
  endfunction

  // advance to the next (co, r) after the last block of a phase
  task next_phase;
    begin
      if (cfg_transposed && ({1'b0, r} + 5'd1) < stride) begin
        r <= r + 4'd1;
        state <= S_SETUP;
      end else if ({1'b0, co} + 8'd1 < co_end) begin
        co <= co + 7'd1;
        r <= 4'd0;
        w_base <= w_base + layer_row;
        state <= S_SETUP;
      end else begin
        state <= S_FLUSH;
      end
    end
  endtask

  // ---------------------------------------------------------------- pipeline
  // A: step issued (addresses go to the RAMs)   B: RAM data   C: lane products
  // then accumulate. `ce` low freezes all of it when a finished block can't
  // be handed to the output buffer yet.
  wire ce;

  // step address and lane mask, from the sequencer registers
  wire signed [SW-1:0] l0 = f_tap - $signed({2'b00, cfg_tile_base});
  wire [3:0] rot = l0[3:0];
  wire [ABITS-1:0] row_lo = (l0 >>> 4) << cfg_cin_log2;
  wire [ABITS-1:0] row_hi = ((l0 >>> 4) + 1) << cfg_cin_log2;

  genvar b;
  generate
    for (b = 0; b < LANES; b = b + 1) begin : addr
      // bank b holds frame f_tap + ((b - rot) mod 16): one row further on for b < rot
      assign act_addr[b*ABITS +: ABITS] = ((b < rot) ? row_hi : row_lo) | {{(ABITS-7){1'b0}}, ci};
    end
  endgenerate
  assign act_en = ce;
  assign w_en   = ce;
  assign w_addr = w_base + ({{(WABITS-5){1'b0}}, kk_w} << cfg_cin_log2) + {{(WABITS-7){1'b0}}, ci};

  // lanes j in [lane_lo, lane_hi) read in-sequence frames
  wire signed [SW-1:0] neg_f = -f_tap;
  wire signed [SW-1:0] room  = $signed({2'b00, cfg_tin}) - f_tap;
  wire [4:0] lane_lo = neg_f <= 0 ? 5'd0 : (neg_f >= LANES ? LANES : neg_f[4:0]);
  wire [4:0] lane_hi = room  <= 0 ? 5'd0 : (room  >= LANES ? LANES : room[4:0]);

  // block metadata, handed along with the block's last step
  wire signed [SW-1:0] t_base = cfg_transposed ? (blk <<< cfg_stride_log2) + s_r - s_pad : blk;
  wire signed [SW-1:0] remain = blk_end - blk;
  wire [4:0] n_valid = remain >= LANES ? LANES : remain[4:0];

  reg              a_valid, a_first, a_last;
  reg [3:0]        a_rot;
  reg [4:0]        a_lo, a_hi, a_nvalid;
  reg [6:0]        a_co;
  reg [TBITS-1:0]  a_tbase;

  reg              b_valid, b_first, b_last;
  reg [4:0]        b_nvalid;
  reg [6:0]        b_co;
  reg [TBITS-1:0]  b_tbase;
  reg signed [15:0] b_x [0:LANES-1];
  reg signed [15:0] b_w;

  reg              c_valid, c_first, c_last;
  reg [4:0]        c_nvalid;
  reg [6:0]        c_co;
  reg [TBITS-1:0]  c_tbase;
  reg signed [31:0] c_prod [0:LANES-1];

  // Signed arrays: yosys drops the signedness of array elements (it turns
  // them into plain registers), so every width change below sign-extends
  // explicitly - iverilog keeps it, and the simulations passed with an
  // implicit extension that the chip then did as zero extension.
  reg signed [ACCW-1:0] acc [0:LANES-1];

  // output buffer: one finished block, drained one lane per clock
  reg                   o_busy;
  reg [4:0]             o_n, o_idx;
  reg [6:0]             o_co;
  reg [TBITS-1:0]       o_t;
  reg signed [ACCW-1:0] o_acc [0:LANES-1];
  wire o_free_next = !o_busy || (res_ready && o_idx == o_n - 1);

  assign ce    = !(c_valid && c_last && !o_free_next);
  assign issue = (state == S_RUN) && ce;

  // stage B inputs: rotate bank outputs into lane order, zero the padding
  wire signed [15:0] lane_x [0:LANES-1];
  generate
    for (b = 0; b < LANES; b = b + 1) begin : lane_in
      wire [3:0] src = a_rot + b;
      assign lane_x[b] = (b >= a_lo && b < a_hi) ? $signed(act_data[src*16 +: 16]) : 16'sd0;
    end
  endgenerate

  // stage C: lane multipliers (one MULT18X18 each)
  wire signed [31:0] lane_prod [0:LANES-1];
  generate
    for (b = 0; b < LANES; b = b + 1) begin : mult
      wire signed [35:0] p;
      vocoder_mul18 u_mul (.clk(clk), .a({{2{b_x[b][15]}}, b_x[b]}), .b({{2{b_w[15]}}, b_w}), .p(p));
      assign lane_prod[b] = p[31:0];
    end
  endgenerate

  // the phase's frame range, registered in S_SETUP's first clock (timing:
  // the arithmetic from the configuration doesn't fit one clock with the rest)
  reg                  su_wait;
  reg                  su_empty;
  reg signed [SW-1:0]  su_lo, su_hi1, su_tend, su_f0;
  reg [4:0]            su_taps;
  always @(posedge clk) begin
    su_empty <= q_lo > q_hi;
    su_lo <= q_lo;
    su_hi1 <= q_hi + 1;
    su_tend <= s_tend;
    su_f0 <= s_t0 - s_pad;
    su_taps <= taps_t;
  end

  integer j;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state <= S_IDLE;
      busy <= 1'b0;
      su_wait <= 1'b0;
      a_valid <= 1'b0;
      b_valid <= 1'b0;
      c_valid <= 1'b0;
      o_busy <= 1'b0;
    end else begin
      // ---- sequencer
      case (state)
        S_IDLE: if (start) begin
          busy <= 1'b1;
          co <= cfg_co0;
          r <= 4'd0;
          w_base <= {WABITS{1'b0}};
          state <= S_SETUP;
        end
        S_SETUP: if (!su_wait) su_wait <= 1'b1;  // first clock: su_* registered
        else begin
          su_wait <= 1'b0;
          tap <= 5'd0;
          ci <= 7'd0;
          if (cfg_transposed) begin
            if (su_empty) begin
              next_phase;  // no outputs in this phase
            end else begin
              blk <= su_lo;
              blk_end <= su_hi1;
              f_tap <= su_lo;
              n_taps <= su_taps;
              kk_w <= {1'b0, r};
              state <= S_RUN;
            end
          end else begin
            blk <= s_t0;
            blk_end <= su_tend;
            f_tap <= su_f0;
            n_taps <= cfg_k;
            kk_w <= 5'd0;
            state <= (cfg_tcount == 0 || cfg_cocount == 0) ? S_FLUSH : S_RUN;
          end
        end
        S_RUN: if (issue) begin
          if (!last_ci) begin
            ci <= ci + 7'd1;
          end else begin
            ci <= 7'd0;
            if (!last_tap) begin
              tap <= tap + 5'd1;
              f_tap <= cfg_transposed ? f_tap - 1 : f_tap + $signed({{(SW-3){1'b0}}, cfg_dil});
              kk_w <= kk_w + (cfg_transposed ? stride : 5'd1);
            end else begin
              tap <= 5'd0;
              kk_w <= cfg_transposed ? {1'b0, r} : 5'd0;
              if (blk_next < blk_end) begin
                blk <= blk_next;
                f_tap <= tap0_frame(blk_next);
              end else begin
                next_phase;
              end
            end
          end
        end
        S_FLUSH: if (!a_valid && !b_valid && !c_valid && !o_busy) begin
          busy <= 1'b0;
          state <= S_IDLE;
        end
      endcase

      // ---- pipeline
      if (ce) begin
        a_valid <= issue;
        a_first <= tap == 0 && ci == 0;
        a_last <= last_tap && last_ci;
        a_rot <= rot;
        a_lo <= lane_lo;
        a_hi <= lane_hi;
        a_nvalid <= n_valid;
        a_co <= co;
        a_tbase <= t_base[TBITS-1:0];

        b_valid <= a_valid;
        b_first <= a_first;
        b_last <= a_last;
        b_nvalid <= a_nvalid;
        b_co <= a_co;
        b_tbase <= a_tbase;
        for (j = 0; j < LANES; j = j + 1) b_x[j] <= lane_x[j];
        b_w <= $signed(w_data);

        c_valid <= b_valid;
        c_first <= b_first;
        c_last <= b_last;
        c_nvalid <= b_nvalid;
        c_co <= b_co;
        c_tbase <= b_tbase;
        for (j = 0; j < LANES; j = j + 1) c_prod[j] <= lane_prod[j];

        if (c_valid)
          for (j = 0; j < LANES; j = j + 1)
            acc[j] <= (c_first ? {ACCW{1'b0}} : acc[j]) + {{(ACCW-32){c_prod[j][31]}}, c_prod[j]};
      end

      // ---- output buffer
      if (o_busy && res_ready) begin
        o_idx <= o_idx + 5'd1;
        o_t <= o_t + (cfg_transposed ? {{(TBITS-5){1'b0}}, stride} : {{(TBITS-1){1'b0}}, 1'b1});
        if (o_idx == o_n - 1) o_busy <= 1'b0;
      end
      if (ce && c_valid && c_last) begin
        o_busy <= 1'b1;
        o_idx <= 5'd0;
        o_n <= c_nvalid;
        o_co <= c_co;
        o_t <= c_tbase;
        for (j = 0; j < LANES; j = j + 1)
          o_acc[j] <= (c_first ? {ACCW{1'b0}} : acc[j]) + {{(ACCW-32){c_prod[j][31]}}, c_prod[j]};
      end
    end
  end

  assign res_valid = o_busy;
  assign res_co    = o_co;
  assign res_t     = o_t;
  assign res_acc   = o_acc[o_idx[3:0]];

endmodule
