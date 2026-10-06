// The flow's LayerNorm and attention (docs/studies.md, "The flow in fixed point"), bit-exact
// with model/src/FlowOps.h. Inputs come from the activation banks (and,
// for attention, q from the weight RAM); results go out one value at a time
// as (channel, frame, value) into vocoder_core's output buffer.
//
// LayerNorm: frames 0..n-1 of a 32-channel tile in the banks, loaded as
// usual (frame l in bank l % 16, channel c at (l / 16) * 32 + c). Three
// passes over the 32 channels per frame: sum; u = 32 x - sum and sum(u^2);
// y = ((u R >> (2 + E/2)) g >> (14 + gs)) + b. R = 1/sqrt(mantissa of W)
// from the table: W is normalized by shifting (8 or 1 bits per clock), which
// gives the table index and the 16-bit interpolation fraction directly.
//
// Attention, one head of one layer per run: k and v loaded transposed into
// the banks (frame j, channel c in bank (c + 8 (j & 1)) % 16, at address j
// for k and 512 + j for v), so a frame's 16 channels are one read; q in
// the weight RAM (q_i[c] in bank (i + c) & 1 at word i * 8 + c / 2); the
// relative tables in the banks too, row d (0..8), channel c in bank c, at
// REL_K + d and REL_V + d (so T <= 496). Four lane multipliers take four
// channels (a group g: channels 4g..4g+3) per clock. Per query row i:
//   q pass:      read q_i
//   rel pass:    rs_d = q_i . rel_k[d], 4 clocks each, into the row buffer
//                at RS + d
//   score pass:  s_j = q_i . k_j + rs_(j-i+4) (within +-4), 4 clocks per
//                frame, keeping the max
//   exp pass:    e_j = exp2Q((s_j - max) score_mult >> score_shift)
//   division:    r = 2^47 / sum(e), one bit per clock
//   per group g: PV pass acc_c += p_j v_jc, p_j = e_j r >> 32 (c in g),
//                then 9 more steps acc_c += p_(i+d-4) rel_v[d][c];
//                requantize acc_c x merge_mult >> merge_shift
// (q . (k + rel_k) and p (v + rel_v) split into two sums: same integers.)
// About 9 T + 170 clocks per row.
//
// All reads are addressed combinationally from the issue counters, so data
// arrives one clock after issue; each pass is a short pipeline with valid
// bits v1.. and the issue index carried along.
`timescale 1ns / 1ps

module vocoder_flow_unit #(
    parameter EXP_FILE = "vocoder_exp2.hex",    // 256 x {lut[i + 1], lut[i]}, 17 bits each
    parameter RSQRT_FILE = "vocoder_rsqrt.hex"
) (
    input  wire               clk,
    input  wire               rst_n,

    input  wire               ln_start,
    input  wire               att_start,
    output reg                busy,

    // LayerNorm
    input  wire [8:0]         ln_frames,     // frames in the tile
    input  wire [31:0]        ln_eps,
    input  wire [5:0]         ln_gs,         // <= 30
    output wire [6:0]         prm_c,         // gamma / beta of this channel, one clock later:
    input  wire signed [31:0] prm_g,
    input  wire signed [31:0] prm_b,

    // attention
    input  wire [8:0]         att_frames,    // T
    input  wire [15:0]        score_mult,
    input  wire [7:0]         score_shift,   // 1..47
    input  wire [15:0]        merge_mult,
    input  wire [7:0]         merge_shift,   // 1..47

    // activation banks (all banks get the same address), 1-clock latency
    output wire               bank_en,
    output wire [9:0]         bank_addr,
    input  wire [16*16-1:0]   bank_data,

    // weight RAM (q): both banks at the same word address, 1-clock latency
    output wire               q_en,
    output wire [11:0]        q_addr,
    input  wire [15:0]        q_even,
    input  wire [15:0]        q_odd,

    // results
    output reg                out_valid,
    output reg  [5:0]         out_c,
    output reg  [8:0]         out_t,
    output reg  signed [15:0] out_y
);
  localparam QMAX = 32767;
  localparam SCORE_FRAC = 11;
  localparam [9:0] REL_K = 10'd496, REL_V = 10'd1008;
  localparam [8:0] RS = 9'd503;

  // ---------------------------------------------------------------- tables (block RAM ROMs)
  reg [33:0] exp_rom [0:255];
  reg [33:0] rsqrt_rom [0:255];
  initial begin
    $readmemh(EXP_FILE, exp_rom);
    $readmemh(RSQRT_FILE, rsqrt_rom);
  end
  wire [7:0] exp_addr, rsqrt_addr;
  reg [33:0] exp_q, rsqrt_q;
  always @(posedge clk) begin
    exp_q <= exp_rom[exp_addr];
    rsqrt_q <= rsqrt_rom[rsqrt_addr];
  end

  // ---------------------------------------------------------------- multipliers
  reg  signed [35:0] m36_a, m36_b;
  wire signed [71:0] m36_p;
  vocoder_mul36 u_m36 (.clk(clk), .a(m36_a), .b(m36_b), .p(m36_p));
  localparam L = 4;  // lanes
  reg  signed [17:0] lane_a [0:L-1];
  reg  signed [17:0] lane_b [0:L-1];
  wire signed [35:0] lane_p [0:L-1];
  genvar g;
  generate
    for (g = 0; g < L; g = g + 1) begin : lanes
      vocoder_mul18 u_mul (.clk(clk), .a(lane_a[g]), .b(lane_b[g]), .p(lane_p[g]));
    end
  endgenerate

  // +-32767 by bit tests (no wide compare): above when non-negative with a
  // bit set above bit 14; below when negative and not in [-32768, -1], or -32768
  function signed [15:0] clamp16(input signed [57:0] v);
    clamp16 = (!v[57] && |v[56:15]) ? 16'sd32767 :
              (v[57] && (!(&v[56:15]) || v[14:0] == 15'd0)) ? -16'sd32767 : v[15:0];
  endfunction

  // ---------------------------------------------------------------- row buffer (scores, then e; rs at RS..)
  (* ram_style = "block" *) reg [35:0] rowbuf [0:511];
  reg        rb_we;
  reg [8:0]  rb_waddr;
  reg [35:0] rb_wdata;
  wire [8:0] rb_raddr;
  reg [35:0] rb_q;
  always @(posedge clk) begin
    if (rb_we) rowbuf[rb_waddr] <= rb_wdata;
    rb_q <= rowbuf[rb_raddr];
  end

  // ---------------------------------------------------------------- state
  localparam S_IDLE = 4'd0, S_L_SUM = 4'd1, S_L_VAR = 4'd2, S_L_W = 4'd3, S_L_NORM = 4'd4, S_L_R1 = 4'd5,
             S_L_R2 = 4'd6, S_L_R3 = 4'd7, S_L_OUT = 4'd8, S_A_Q = 4'd9, S_A_REL = 4'd10, S_A_SCORE = 4'd11,
             S_A_EXP = 4'd12, S_A_DIV = 4'd13, S_A_PV = 4'd14, S_A_REQ = 4'd15;
  reg [3:0]  state;
  reg [10:0] iss;                  // issue counter of the current pass
  reg [10:0] nIss;                 // how many to issue
  wire       issuing = iss < nIss;
  reg        v1, v2, v3, v4, v5, v6;  // pipeline valid
  reg [10:0] i1, i2, i3, i4, i5, i6;  // index through the pipeline
  reg [1:0]  pv_g;                 // channel group of the PV pass
  // rel and score passes: frame (or relative row) iss / 4, group iss % 4
  wire [8:0] iss_j = iss[10:2];
  wire       pipe_empty = !v1 && !v2 && !v3 && !v4 && !v5 && !v6;
  wire       pass_done = !issuing && pipe_empty;

  reg [8:0]  frame;                // LayerNorm: frame in the tile; attention: query row
  wire [3:0] fbank = frame[3:0];
  wire signed [15:0] ln_x = bank_data[fbank*16 +: 16];

  // attention issue: relative row of frame iss (score pass), frame of the
  // relative step (PV pass)
  wire signed [11:0] sc_d = $signed({3'b0, iss_j}) - $signed({3'b0, frame}) + 12'sd4;
  wire               sc_in = sc_d >= 0 && sc_d <= 8;
  wire               pv_rel = iss >= {2'b0, att_frames};
  wire [9:0]         pv_d = iss[9:0] - {1'b0, att_frames};
  wire signed [11:0] pv_j = $signed({3'b0, frame}) + $signed({2'b0, pv_d}) - 12'sd4;
  wire               pv_in = pv_j >= 0 && pv_j < $signed({3'b0, att_frames});
  // issues that produce nothing: relative steps outside the sentence
  wire       iss_ok = !(state == S_A_PV && pv_rel && !pv_in);
  // channel c of the read in bank c + 8 (j & 1) (k, v) or bank c (relative rows)
  wire       iss_rot = state == S_A_SCORE ? iss_j[0] : (state == S_A_PV && !pv_rel ? iss[0] : 1'b0);

  // read addressing (combinational from the issue counter)
  assign bank_en = issuing && (state == S_L_SUM || state == S_L_VAR || state == S_L_OUT || state == S_A_REL ||
                               state == S_A_SCORE || state == S_A_PV);
  assign bank_addr = state == S_A_REL ? REL_K + {1'b0, iss_j} :
                     state == S_A_SCORE ? {1'b0, iss_j} :
                     state == S_A_PV ? (pv_rel ? REL_V + pv_d : 10'd512 | iss[9:0]) :
                     {frame[8:4], iss[4:0]};
  assign q_en = issuing && state == S_A_Q;
  assign q_addr = {frame, iss[2:0]};
  assign rb_raddr = state == S_A_SCORE ? RS + sc_d[8:0] : (state == S_A_PV && pv_rel ? pv_j[8:0] : iss[8:0]);
  assign prm_c = iss[6:0];

  // LayerNorm registers
  reg signed [22:0] sum;
  reg [49:0] sq, W;
  reg [5:0]  nsh;                  // left shifts that normalized W: E = 49 - nsh
  reg [5:0]  E;
  reg [16:0] R, ra;
  reg signed [31:0] g1, g2, b1, b2, b3, b4;
  reg signed [47:0] shy_r;
  reg        hold;                 // S_L_R2, S_L_R3: one clock for m36q
  assign rsqrt_addr = W[48:41];

  // attention registers
  reg signed [15:0] q [0:15];
  reg signed [15:0] vreg [0:L-1];
  reg signed [15:0] vreg2 [0:L-1];
  reg [16:0] e_r;                  // exp pass: e_j, stage 5 -> 6
  reg signed [35:0] prod [0:L-1];
  reg signed [35:0] acc [0:L-1];
  reg signed [39:0] mx, score;     // score: accumulated over the 4 groups of a frame
  reg signed [39:0] xs;
  reg [2:0]  lut_rem;
  reg [4:0]  lut_ip;
  reg [24:0] esum;
  reg [47:0] div_rem, div_quo;
  reg [5:0]  div_step;
  reg [31:0] r_recip;
  reg        rot1;                 // bank rotation of the read in stage 1
  reg        rin1;                 // score pass: frame has a relative term
  reg signed [35:0] rs1, rs2;      // its rs, stages 1, 2
  wire [39:0] nxs = -xs;
  assign exp_addr = nxs[10:3];

  // the 4 channels of the stage-1 item's group in the bank read
  wire [1:0] grp1 = state == S_A_PV ? pv_g : i1[1:0];
  wire signed [15:0] tbx [0:L-1];
  wire signed [15:0] qg [0:L-1];
  generate
    for (g = 0; g < L; g = g + 1) begin : rot
      wire [3:0] c = {grp1, 2'b00} + g;
      wire [3:0] b = c + (rot1 ? 4'd8 : 4'd0);
      assign tbx[g] = bank_data[b*16 +: 16];
      assign qg[g] = q[c];
    end
  endgenerate

  // sum of the lane products, each sign-extended explicitly
  reg signed [39:0] prod_sum;
  integer k;
  always @* begin
    prod_sum = 0;
    for (k = 0; k < L; k = k + 1) prod_sum = prod_sum + {{4{prod[k][35]}}, prod[k]};
  end

  // shared rounding shifter on the 36x36 product: (p + 2^(s-1)) >> s
  // amount and rounding constant registered: fixed during a pass, and their
  // first use is two clocks after it starts
  wire [7:0] shx_s = state == S_L_OUT ? 8'd2 + {3'd0, E[5:1]} : (state == S_A_EXP ? score_shift : merge_shift);
  reg  [7:0] shx_sr;
  reg  signed [57:0] shx_rnd;
  always @(posedge clk) begin
    shx_sr <= shx_s;
    shx_rnd <= 58'sd1 <<< (shx_s - 8'd1);
  end
  // its input is the product registered (m36q), for timing: DSP, shift and
  // clamp don't fit one clock at 54 MHz
  reg  signed [55:0] m36q;
  wire signed [57:0] shx_in = {{2{m36q[55]}}, m36q};
  wire signed [57:0] shx = (shx_in + shx_rnd) >>> shx_sr;
  // LayerNorm output shift on lane 0: >> 14 + gs
  wire [6:0] shy_s = 7'd14 + {1'b0, ln_gs};
  wire signed [47:0] shy_in = {{12{lane_p[0][35]}}, lane_p[0]};
  wire signed [47:0] shy = (shy_in + (48'sd1 <<< (shy_s - 1))) >>> shy_s;

  // exp2 of the clamped log2-domain score (stage 4): table pair and fraction
  wire [16:0] lut_a = exp_q[16:0], lut_b = exp_q[33:17];
  wire signed [20:0] lut_d = $signed({1'b0, lut_b}) - $signed({1'b0, lut_a});
  wire signed [24:0] lut_step = lut_d * $signed({1'b0, lut_rem});
  wire signed [24:0] lut_val = $signed({8'd0, lut_a}) + ((lut_step + 25'sd4) >>> 3);
  wire [16:0] e_now = lut_val[16:0] >> lut_ip;

  // LayerNorm: interpolated 1/sqrt (S_L_R2)
  wire signed [57:0] r_step = (shx_in + 58'sd32768) >>> 16;
  wire [16:0] r_interp = ra + r_step[16:0];

  task start_pass(input [3:0] s, input [10:0] n);
    begin
      state <= s;
      iss <= 11'd0;
      nIss <= n;
    end
  endtask

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state <= S_IDLE;
      busy <= 1'b0;
      out_valid <= 1'b0;
      rb_we <= 1'b0;
      v1 <= 0; v2 <= 0; v3 <= 0; v4 <= 0; v5 <= 0; v6 <= 0;
      iss <= 0;
      nIss <= 0;
    end else begin
      out_valid <= 1'b0;
      rb_we <= 1'b0;
      // default pipeline advance (passes with an issue counter)
      v1 <= issuing && iss_ok && (state == S_L_SUM || state == S_L_VAR || state == S_L_OUT || state == S_A_Q ||
                                  state == S_A_REL || state == S_A_SCORE || state == S_A_EXP ||
                                  state == S_A_PV || state == S_A_REQ);
      i1 <= iss;
      rot1 <= iss_rot;
      rin1 <= sc_in;
      v2 <= v1; i2 <= i1;
      v3 <= v2; i3 <= i2;
      v4 <= v3; i4 <= i3;
      v5 <= v4; i5 <= i4;
      v6 <= v5; i6 <= i5;
      m36q <= m36_p[55:0];
      if (issuing) iss <= iss + 11'd1;

      case (state)
        S_IDLE: begin
          if (ln_start) begin
            busy <= 1'b1;
            frame <= 9'd0;
            sum <= 0;
            start_pass(S_L_SUM, 11'd32);
          end else if (att_start) begin
            busy <= 1'b1;
            frame <= 9'd0;
            start_pass(S_A_Q, 11'd8);
          end
        end

        // ------------------------------------------------ LayerNorm
        S_L_SUM: begin
          if (v1) sum <= sum + ln_x;
          if (pass_done) begin
            sq <= 0;
            start_pass(S_L_VAR, 11'd32);
          end
        end
        S_L_VAR: begin
          if (v1) begin  // u = 32 x - sum, squared
            m36_a <= {{15{ln_x[15]}}, ln_x, 5'd0} - {{13{sum[22]}}, sum};
            m36_b <= {{15{ln_x[15]}}, ln_x, 5'd0} - {{13{sum[22]}}, sum};
          end
          if (v2) sq <= sq + m36_p[49:0];
          if (pass_done) state <= S_L_W;
        end
        S_L_W: begin
          W <= (sq >> 5) + ln_eps == 0 ? 50'd1 : (sq >> 5) + ln_eps;
          nsh <= 6'd0;
          state <= S_L_NORM;
        end
        S_L_NORM: begin  // shift the leading one to bit 49; the table read follows W
          if (W[49:42] == 8'd0) begin
            W <= W << 8;
            nsh <= nsh + 6'd8;
          end else if (!W[49]) begin
            W <= W << 1;
            nsh <= nsh + 6'd1;
          end else begin
            E <= 6'd49 - nsh;
            state <= S_L_R1;
          end
        end
        S_L_R1: begin  // table pair for W[48:41]; (b - a) x the 16-bit fraction W[40:25]
          ra <= rsqrt_q[16:0];
          m36_a <= $signed({19'd0, rsqrt_q[33:17]}) - $signed({19'd0, rsqrt_q[16:0]});
          m36_b <= {20'd0, W[40:25]};
          hold <= 1'b1;
          state <= S_L_R2;
        end
        S_L_R2: begin  // R = a + round(product / 2^16); times 1/sqrt(2) for an odd exponent
          hold <= 1'b0;
          if (!hold) begin
            R <= r_interp;
            m36_a <= {19'd0, r_interp};
            m36_b <= 36'sd46341;
            hold <= 1'b1;
            if (E[0]) state <= S_L_R3;
            else start_pass(S_L_OUT, 11'd32);
          end
        end
        S_L_R3: begin
          hold <= 1'b0;
          if (!hold) begin
            R <= r_step[16:0];
            start_pass(S_L_OUT, 11'd32);
          end
        end
        S_L_OUT: begin
          if (v1) begin  // u x R; gamma and beta arrive with the data
            m36_a <= {{15{ln_x[15]}}, ln_x, 5'd0} - {{13{sum[22]}}, sum};
            m36_b <= {19'd0, R};
            g1 <= prm_g;
            b1 <= prm_b;
          end
          if (v2) begin  // the product goes to m36q
            g2 <= g1;
            b2 <= b1;
          end
          if (v3) begin  // nq (Q14) x gamma on lane 0
            lane_a[0] <= shx[17:0];
            lane_b[0] <= g2[17:0];
            b3 <= b2;
          end
          if (v4) begin  // >> 14 + gs
            shy_r <= shy;
            b4 <= b3;
          end
          if (v5) begin
            out_valid <= 1'b1;
            out_c <= i5[5:0];
            out_t <= frame;
            out_y <= clamp16({{10{shy_r[47]}}, shy_r} + {{26{b4[31]}}, b4});
          end
          if (pass_done) begin
            if (frame + 9'd1 == ln_frames) begin
              busy <= 1'b0;
              state <= S_IDLE;
            end else begin
              frame <= frame + 9'd1;
              sum <= 0;
              start_pass(S_L_SUM, 11'd32);
            end
          end
        end

        // ------------------------------------------------ attention
        S_A_Q: begin  // q_i[2m] in bank (i & 1), q_i[2m + 1] in the other
          if (v1) begin
            q[{i1[2:0], 1'b0}] <= frame[0] ? q_odd : q_even;
            q[{i1[2:0], 1'b1}] <= frame[0] ? q_even : q_odd;
          end
          if (pass_done) start_pass(S_A_REL, 11'd36);
        end
        S_A_REL, S_A_SCORE: begin  // item iss: frame (row) iss / 4, group iss % 4
          if (v1) begin  // lanes: q_c x k_jc (or rel_k[d][c])
            for (k = 0; k < L; k = k + 1) begin
              lane_a[k] <= {{2{qg[k][15]}}, qg[k]};
              lane_b[k] <= {{2{tbx[k][15]}}, tbx[k]};
            end
            rs1 <= rin1 ? $signed(rb_q) : 36'sd0;
          end
          if (v2) begin
            for (k = 0; k < L; k = k + 1) prod[k] <= lane_p[k];
            rs2 <= rs1;
          end
          if (v3) begin  // group 0 starts the sum (with rs), the others add
            if (i3[1:0] == 2'd0) score <= prod_sum + (state == S_A_SCORE ? {{4{rs2[35]}}, rs2} : 40'sd0);
            else score <= score + prod_sum;
          end
          if (v4 && i4[1:0] == 2'd3) begin  // complete
            rb_we <= 1'b1;
            rb_waddr <= state == S_A_SCORE ? i4[10:2] : RS + i4[10:2];
            rb_wdata <= score[35:0];
            if (state == S_A_SCORE && score > mx) mx <= score;
          end
          if (pass_done) begin
            if (state == S_A_REL) begin
              mx <= -(40'sd1 <<< 38);
              start_pass(S_A_SCORE, {att_frames, 2'b00});
            end else begin
              esum <= 0;
              start_pass(S_A_EXP, {2'b0, att_frames});
            end
          end
        end
        S_A_EXP: begin
          if (v1) begin  // (s_j - max) x score_mult
            m36_a <= $signed(rb_q) - mx[35:0];
            m36_b <= {20'd0, score_mult};
          end
          if (v3) begin  // (m36q) >> score_shift, at least -16 in log2 units
            xs <= (shx[57] && !(&shx[56:15])) ? -(40'sd16 <<< SCORE_FRAC) : shx[39:0];
          end
          if (v4) begin  // table pair for the fraction (read now), shift for the integer part
            lut_rem <= nxs[2:0];
            lut_ip <= nxs[15:11];
          end
          if (v5) e_r <= e_now;  // (timing: table, interpolation and shift take this clock)
          if (v6) begin
            rb_we <= 1'b1;
            rb_waddr <= i6[8:0];
            rb_wdata <= {19'd0, e_r};
            esum <= esum + e_r;
          end
          if (pass_done) begin
            div_rem <= 48'd0;
            div_quo <= 48'd0;
            div_step <= 6'd47;
            state <= S_A_DIV;
          end
        end
        S_A_DIV: begin : div  // r = 2^47 / esum, one quotient bit per clock
          reg [48:0] rem2;
          rem2 = {div_rem, div_step == 6'd47};
          if (rem2 >= {24'd0, esum}) begin
            div_rem <= rem2 - {24'd0, esum};
            div_quo <= {div_quo[46:0], 1'b1};
          end else begin
            div_rem <= rem2[47:0];
            div_quo <= {div_quo[46:0], 1'b0};
          end
          if (div_step == 0) begin
            for (k = 0; k < L; k = k + 1) acc[k] <= 0;
            pv_g <= 2'd0;
            start_pass(S_A_PV, {2'b0, att_frames} + 11'd9);
          end else begin
            div_step <= div_step - 6'd1;
          end
        end
        S_A_PV: begin  // group pv_g: T frames, then the 9 relative steps
          r_recip <= div_quo[31:0];
          if (v1) begin  // e_j x r; v_j (or rel_v[d])
            m36_a <= {19'd0, rb_q[16:0]};
            m36_b <= {4'd0, r_recip};  // r_recip took the quotient in the pass's first clock
            for (k = 0; k < L; k = k + 1) vreg[k] <= tbx[k];
          end
          if (v2) for (k = 0; k < L; k = k + 1) vreg2[k] <= vreg[k];  // the product goes to m36q
          if (v3) begin  // p_j (Q15, from m36q) x v
            for (k = 0; k < L; k = k + 1) begin
              lane_a[k] <= (m36q[47:0] + 48'd2147483648) >> 32;
              lane_b[k] <= {{2{vreg2[k][15]}}, vreg2[k]};
            end
          end
          if (v4) for (k = 0; k < L; k = k + 1) prod[k] <= lane_p[k];
          if (v5) for (k = 0; k < L; k = k + 1) acc[k] <= acc[k] + prod[k];
          if (pass_done) start_pass(S_A_REQ, L);
        end
        S_A_REQ: begin  // acc_c x merge_mult >> merge_shift
          if (issuing) begin
            m36_a <= acc[iss[1:0]];
            m36_b <= {20'd0, merge_mult};
          end
          if (v2) begin  // product of the operands issued two clocks earlier (m36q)
            out_valid <= 1'b1;
            out_c <= {2'd0, pv_g, i2[1:0]};
            out_t <= frame;
            out_y <= clamp16(shx);
          end
          if (pass_done) begin
            if (pv_g != 2'd3) begin  // next group
              for (k = 0; k < L; k = k + 1) acc[k] <= 0;
              pv_g <= pv_g + 2'd1;
              start_pass(S_A_PV, {2'b0, att_frames} + 11'd9);
            end else if (frame + 9'd1 == att_frames) begin
              busy <= 1'b0;
              state <= S_IDLE;
            end else begin
              frame <= frame + 9'd1;
              start_pass(S_A_Q, 11'd8);
            end
          end
        end
        default: state <= S_IDLE;
      endcase
    end
  end
endmodule
