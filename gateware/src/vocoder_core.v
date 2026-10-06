// The vocoder: runs the hardware image (model/src/HwImage.h) on one
// sentence's latent in SDRAM and writes its PCM to SDRAM. Executable spec:
// model/src/HwSim.h, which this follows step by step:
//
//   for every op (16-word descriptor):
//     load the per-channel parameters
//     for every group of output channels whose weights fit the weight RAM:
//       load the group's weights
//       for every tile of output frames:
//         load the input tile with its halo into the banks (leaky ReLU on
//         the way), channel plane by channel plane
//         run the engine over the tile; the post unit takes the residual
//         from a stream read from SDRAM meanwhile, and writes the results
//         into the output buffer
//         write the output buffer to SDRAM, channel plane by channel plane
//
// SDRAM traffic goes through one sdram_ctrl B-style master port (vocoder_dma).
// Header: read when `hdr_load` pulses (after a program upload).
`timescale 1ns / 1ps

module vocoder_core #(
    parameter TANH_FILE = "vocoder_tanh.hex"
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        hdr_load,
    output reg         hdr_valid,
    output reg  [12:0] max_frames,
    output reg  [20:0] lat_base0, lat_base1, lat_plane, pcm_base0, pcm_base1,

    input  wire        go,           // run a sentence (hdr_valid)
    input  wire [12:0] frames,
    input  wire [20:0] lat_off,      // added to the latent buffer's address (slot select)
    input  wire [20:0] pcm_off,      // added to the PCM buffer's address
    output reg         busy,
    output reg         done,         // pulse
    output reg  [31:0] run_cycles,   // of the last sentence
    output reg  [31:0] mac_cycles,   // of those, engine busy
    output wire [39:0] dbg,          // state, op, op count, op table, frames (for the link's 'D')

    output wire        m_req,
    output wire        m_we,
    output wire [20:0] m_addr,
    output wire [8:0]  m_len,
    input  wire        m_ack,
    input  wire        m_wpull,
    output wire [31:0] m_wdata,
    output wire [3:0]  m_wbe,
    input  wire        m_rvalid,
    input  wire [31:0] m_rdata,
    input  wire        m_done
);
  localparam TBITS = 20, ACCW = 36, ABITS = 10, WABITS = 13;
  localparam SW = TBITS + 2;

  // ---------------------------------------------------------------- descriptor
  reg [31:0] d [0:12];
  wire        dsc_transposed = d[0][0];
  wire        dsc_residual   = d[0][2];
  wire        dsc_output     = d[0][3];
  wire        dsc_in_latent  = d[0][4];
  wire        dsc_out_pcm    = d[0][5];
  wire [2:0]  dsc_cin_log2   = d[0][10:8];
  wire [2:0]  dsc_s          = d[0][14:12];
  wire [4:0]  dsc_k          = d[0][20:16];
  wire [2:0]  dsc_dil        = d[0][26:24];
  wire [7:0]  dsc_cout       = d[1][7:0];
  wire [5:0]  dsc_pad        = d[1][13:8];
  wire [15:0] dsc_lr         = d[1][31:16];
  wire [20:0] dsc_in_plane   = d[3][20:0];
  wire [20:0] dsc_out_plane  = d[5][20:0];
  wire [20:0] dsc_res_plane  = d[7][20:0];
  wire [7:0]  dsc_group      = d[11][7:0];
  wire [15:0] dsc_tile       = d[11][31:16];
  wire signed [15:0] dsc_a   = d[12][15:0];
  wire signed [15:0] dsc_b   = d[12][31:16];

  function [3:0] log2_rate(input [15:0] r);
    integer i;
    begin
      log2_rate = 0;
      for (i = 0; i < 16; i = i + 1) if (r[i]) log2_rate = i;
    end
  endfunction

  reg [12:0]      T;
  reg [TBITS-1:0] tin, tout;
  reg [20:0]      in_base, out_base;   // slot offsets applied
  reg [7:0]       op_i, n_ops;
  reg [20:0]      op_table;

  // loop state
  reg [7:0]        co0, cnt;
  reg [TBITS-1:0]  t0, n;
  reg [20:0]       w_next, grp_res, grp_out;
  reg signed [SW-1:0] lo, hi;

  // ---------------------------------------------------------------- DMA
  localparam M_HDR = 3'd0, M_DESC = 3'd1, M_PARAM = 3'd2, M_WGT = 3'd3, M_TILE = 3'd4, M_RES = 3'd5,
             M_WRITE = 3'd6;
  reg [2:0]  mode;
  reg        dma_start;
  reg        dma_we;
  reg [20:0] dma_first, dma_stride;
  reg [7:0]  dma_nseg;
  reg [15:0] dma_words;
  reg [8:0]  dma_max;
  wire       dma_busy;
  wire [20:0] dma_seg_base;
  wire [15:0] res_room;

  vocoder_dma u_dma (
      .clk(clk), .rst_n(rst_n), .start(dma_start), .we(dma_we), .first(dma_first), .stride(dma_stride),
      .nseg(dma_nseg), .words(dma_words), .max_burst(dma_max), .throttle(mode == M_RES), .room(res_room),
      .busy(dma_busy), .seg_base(dma_seg_base),
      .m_req(m_req), .m_we(m_we), .m_addr(m_addr), .m_len(m_len), .m_ack(m_ack), .m_done(m_done));

  // a transfer is finished once the DMA went busy and back
  reg dma_kicked;
  wire dma_finished = dma_kicked && !dma_busy && !dma_start;

  // words received in the current segment / overall
  reg [15:0] rx_word;   // within the segment
  reg [15:0] rx_total;  // within the transfer
  reg [7:0]  rx_seg;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rx_word <= 16'd0;
      rx_total <= 16'd0;
      rx_seg <= 8'd0;
    end else if (dma_start) begin
      rx_word <= 16'd0;
      rx_total <= 16'd0;
      rx_seg <= 8'd0;
    end else if (m_rvalid) begin
      rx_total <= rx_total + 16'd1;
      if (rx_word == dma_words - 16'd1) begin
        rx_word <= 16'd0;
        rx_seg <= rx_seg + 8'd1;
      end else begin
        rx_word <= rx_word + 16'd1;
      end
    end
  end

  // ---------------------------------------------------------------- header / descriptor words
  integer i;
  always @(posedge clk) begin
    if (m_rvalid && mode == M_HDR) begin
      case (rx_total[3:0])
        4'd2: n_ops <= m_rdata[7:0];
        4'd3: op_table <= m_rdata[20:0];
        4'd4: max_frames <= m_rdata[12:0];
        4'd5: lat_base0 <= m_rdata[20:0];
        4'd6: lat_base1 <= m_rdata[20:0];
        4'd7: lat_plane <= m_rdata[20:0];
        4'd8: pcm_base0 <= m_rdata[20:0];
        4'd9: pcm_base1 <= m_rdata[20:0];
        default: ;
      endcase
    end
    if (m_rvalid && mode == M_DESC && rx_total < 13) d[rx_total[3:0]] <= m_rdata;
  end

  // ---------------------------------------------------------------- parameter RAMs
  reg [31:0] bias_ram [0:127];
  reg [21:0] ms_ram [0:127];
  always @(posedge clk)
    if (m_rvalid && mode == M_PARAM) begin
      if (!rx_total[0]) bias_ram[rx_total[7:1]] <= m_rdata;
      else ms_ram[rx_total[7:1]] <= {m_rdata[21:16], m_rdata[15:0]};
    end

  // ---------------------------------------------------------------- weight RAM (even / odd values)
  (* ram_style = "block" *) reg [15:0] w_even [0:(1 << (WABITS - 1)) - 1];
  (* ram_style = "block" *) reg [15:0] w_odd  [0:(1 << (WABITS - 1)) - 1];
  wire              e_w_en;
  wire [WABITS-1:0] e_w_addr;
  reg  [15:0]       w_even_q, w_odd_q;
  reg               w_sel;
  always @(posedge clk) begin
    if (m_rvalid && mode == M_WGT) begin
      w_even[rx_total[WABITS-2:0]] <= m_rdata[15:0];
      w_odd[rx_total[WABITS-2:0]] <= m_rdata[31:16];
    end
    if (e_w_en) begin
      w_even_q <= w_even[e_w_addr[WABITS-1:1]];
      w_odd_q <= w_odd[e_w_addr[WABITS-1:1]];
      w_sel <= e_w_addr[0];
    end
  end
  wire [15:0] e_w_data = w_sel ? w_odd_q : w_even_q;

  // ---------------------------------------------------------------- tile loader (+ leaky ReLU)
  reg [TBITS:0] tl_frame;      // frame of the low half of the current word
  reg           l1_v0, l1_v1;  // stage 1: values and their bank addresses
  reg signed [15:0] l1_x0, l1_x1;
  reg [3:0]     l1_b0, l1_b1;
  reg [ABITS-1:0] l1_a0, l1_a1;
  wire signed [SW-1:0] tl_l0 = $signed({1'b0, tl_frame}) - lo;  // local index of the low half
  wire signed [SW-1:0] tl_l1 = tl_l0 + 1;
  wire tl_in0 = $signed({1'b0, tl_frame}) >= lo && $signed({1'b0, tl_frame}) <= hi;
  wire tl_in1 = $signed({1'b0, tl_frame}) + 1 >= lo && $signed({1'b0, tl_frame}) + 1 <= hi;
  wire [ABITS-1:0] tl_row0 = (tl_l0 >>> 4) << dsc_cin_log2;
  wire [ABITS-1:0] tl_row1 = (tl_l1 >>> 4) << dsc_cin_log2;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      l1_v0 <= 1'b0;
      l1_v1 <= 1'b0;
    end else begin
      l1_v0 <= m_rvalid && mode == M_TILE && tl_in0;
      l1_v1 <= m_rvalid && mode == M_TILE && tl_in1;
      l1_x0 <= m_rdata[15:0];
      l1_x1 <= m_rdata[31:16];
      l1_b0 <= tl_l0[3:0];
      l1_b1 <= tl_l1[3:0];
      l1_a0 <= tl_row0 | {{(ABITS-8){1'b0}}, rx_seg};
      l1_a1 <= tl_row1 | {{(ABITS-8){1'b0}}, rx_seg};
      if (dma_start) tl_frame <= {1'b0, lo[TBITS-1:0]} & ~{{TBITS{1'b0}}, 1'b1};
      else if (m_rvalid && mode == M_TILE) begin
        if (rx_word == dma_words - 16'd1) tl_frame <= {1'b0, lo[TBITS-1:0]} & ~{{TBITS{1'b0}}, 1'b1};
        else tl_frame <= tl_frame + 2;
      end
    end
  end
  wire signed [35:0] lk_p0, lk_p1;
  vocoder_mul18 u_lk0 (.clk(clk), .a({{2{l1_x0[15]}}, l1_x0}), .b({2'b00, dsc_lr}), .p(lk_p0));
  vocoder_mul18 u_lk1 (.clk(clk), .a({{2{l1_x1[15]}}, l1_x1}), .b({2'b00, dsc_lr}), .p(lk_p1));
  wire signed [35:0] lk_r0 = (lk_p0 + 36'sd16384) >>> 15;
  wire signed [35:0] lk_r1 = (lk_p1 + 36'sd16384) >>> 15;
  wire [15:0] lk_y0 = (l1_x0[15] && dsc_lr != 16'd32768) ? lk_r0[15:0] : l1_x0;
  wire [15:0] lk_y1 = (l1_x1[15] && dsc_lr != 16'd32768) ? lk_r1[15:0] : l1_x1;

  // ---------------------------------------------------------------- banks, engine, post
  wire              act_en;
  wire [16*ABITS-1:0] act_addr;
  wire [16*16-1:0]  act_data;
  vocoder_act_banks #(.LANES(16), .ABITS(ABITS)) u_banks (
      .clk(clk),
      .wr_en(l1_v0), .wr_bank(l1_b0), .wr_addr(l1_a0), .wr_data(lk_y0),
      .wr2_en(l1_v1), .wr2_bank(l1_b1), .wr2_addr(l1_a1), .wr2_data(lk_y1),
      .rd_en(act_en), .rd_addr(act_addr), .rd_data(act_data));

  reg               eng_start;
  wire              eng_busy;
  wire              e_valid, e_ready;
  wire [6:0]        e_co;
  wire [TBITS-1:0]  e_t;
  wire signed [ACCW-1:0] e_acc;
  vocoder_conv_engine #(.ABITS(ABITS), .WABITS(WABITS), .TBITS(TBITS), .ACCW(ACCW)) u_engine (
      .clk(clk), .rst_n(rst_n), .start(eng_start), .busy(eng_busy),
      .cfg_transposed(dsc_transposed), .cfg_cin_log2(dsc_cin_log2), .cfg_co0(co0[6:0]), .cfg_cocount(cnt[6:0]),
      .cfg_k(dsc_k), .cfg_dil(dsc_dil), .cfg_stride_log2(dsc_s), .cfg_pad(dsc_pad),
      .cfg_tin(tin), .cfg_tile_base(lo[TBITS-1:0]), .cfg_t0(t0), .cfg_tcount(n),
      .act_en(act_en), .act_addr(act_addr), .act_data(act_data),
      .w_en(e_w_en), .w_addr(e_w_addr), .w_data(e_w_data),
      .res_valid(e_valid), .res_ready(e_ready), .res_co(e_co), .res_t(e_t), .res_acc(e_acc));

  // residual stream: words from SDRAM, values in the engine's output order
  // (per output channel, frames t0 .. t0+n-1)
  reg [31:0] rf_mem [0:63];
  reg [6:0]  rf_wp, rf_rp;
  reg        rf_half;
  reg [TBITS-1:0] rf_count;
  wire [6:0] rf_level = rf_wp - rf_rp;
  assign res_room = 16'd64 - {9'd0, rf_level};
  wire [31:0] rf_head = rf_mem[rf_rp[5:0]];
  wire signed [15:0] rf_value = rf_half ? rf_head[31:16] : rf_head[15:0];
  assign e_ready = !dsc_residual || rf_level != 0;
  wire pop = e_valid && e_ready && dsc_residual;
  always @(posedge clk) if (m_rvalid && mode == M_RES) rf_mem[rf_wp[5:0]] <= m_rdata;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rf_wp <= 7'd0;
      rf_rp <= 7'd0;
      rf_half <= 1'b0;
      rf_count <= {TBITS{1'b0}};
    end else begin
      if (m_rvalid && mode == M_RES) rf_wp <= rf_wp + 7'd1;
      if (pop) begin
        if (rf_count == n - 1) begin  // last value of this channel: next channel starts on a new word
          rf_count <= {TBITS{1'b0}};
          rf_half <= 1'b0;
          rf_rp <= rf_rp + 7'd1;
        end else begin
          rf_count <= rf_count + 1'b1;
          rf_half <= !rf_half;
          if (rf_half) rf_rp <= rf_rp + 7'd1;
        end
      end
    end
  end

  wire [6:0] prm_co;
  reg signed [31:0] prm_bias;
  reg [15:0] prm_mult;
  reg [5:0]  prm_shift;
  always @(posedge clk) begin
    prm_bias <= bias_ram[prm_co];
    prm_mult <= ms_ram[prm_co][15:0];
    prm_shift <= ms_ram[prm_co][21:16];
  end

  wire              p_valid;
  wire [6:0]        p_co;
  wire [TBITS-1:0]  p_t;
  wire signed [15:0] p_y;
  vocoder_post #(.TBITS(TBITS), .ACCW(ACCW), .TANH_FILE(TANH_FILE)) u_post (
      .clk(clk), .rst_n(rst_n), .cfg_residual(dsc_residual), .cfg_output(dsc_output),
      .in_valid(e_valid && e_ready), .in_co(e_co), .in_t(e_t), .in_acc(e_acc), .in_res(rf_value),
      .prm_co(prm_co), .prm_bias(prm_bias), .prm_mult(prm_mult), .prm_shift(prm_shift),
      .out_valid(p_valid), .out_co(p_co), .out_t(p_t), .out_y(p_y));

  // ---------------------------------------------------------------- output buffer (even / odd frames)
  (* ram_style = "block" *) reg [15:0] o_even [0:4095];
  (* ram_style = "block" *) reg [15:0] o_odd  [0:4095];
  reg [12:0] ob_base;   // (co - co0) * tile
  reg [6:0]  ob_co;
  wire [12:0] ob_idx_new = (p_co != ob_co) ? ob_base + dsc_tile[12:0] : ob_base;
  wire [12:0] ob_idx = ob_idx_new + (p_t - t0);
  always @(posedge clk) begin
    if (p_valid) begin
      if (ob_idx[0]) o_odd[ob_idx[12:1]] <= p_y;
      else o_even[ob_idx[12:1]] <= p_y;
    end
  end

  // writer: word index into the output buffer, read one clock ahead
  reg  [11:0] wr_idx;      // word presented now
  reg  [11:0] wr_seg_idx;  // first word of the current segment
  reg  [15:0] wr_left;     // words left in the segment
  reg  [15:0] o_even_q, o_odd_q;
  wire [11:0] wr_rd = wr_idx + (m_wpull ? 12'd1 : 12'd0);
  always @(posedge clk) begin
    o_even_q <= o_even[wr_rd];
    o_odd_q <= o_odd[wr_rd];
  end
  assign m_wdata = {o_odd_q, o_even_q};
  assign m_wbe = (wr_left == 16'd1 && n[0]) ? 4'b0011 : 4'b1111;
  wire [15:0] seg_words_out = n[16:1] + {15'd0, n[0]};

  // ---------------------------------------------------------------- scheduler
  localparam S_IDLE = 5'd0, S_HDR = 5'd1, S_DESC = 5'd2, S_DECODE = 5'd3, S_PARAM = 5'd4,
             S_WGT = 5'd5, S_TPREP = 5'd6, S_TILE = 5'd7, S_COMP = 5'd8, S_DRAIN = 5'd9, S_WRITE = 5'd10,
             S_NEXT = 5'd11, S_WAIT = 5'd12, S_NPREP = 5'd13;
  reg [4:0] state, after;
  assign dbg = {3'd0, state, op_i, n_ops, op_table[15:0]};
  reg [3:0] drain;
  reg       eng_started;

  wire signed [SW-1:0] lo_raw = ($signed({2'b00, t0}) + dsc_a) >>> dsc_s;
  wire signed [SW-1:0] hi_raw = ($signed({2'b00, t0}) + $signed({2'b00, n}) - 1 + dsc_b) >>> dsc_s;
  wire signed [SW-1:0] s_tin = $signed({2'b00, tin});

  task kick(input [2:0] m, input w, input [20:0] f, input [20:0] st, input [7:0] ns, input [15:0] wd,
            input [8:0] mb, input [4:0] next_state);
    begin
      mode <= m;
      dma_start <= 1'b1;
      dma_we <= w;
      dma_first <= f;
      dma_stride <= st;
      dma_nseg <= ns;
      dma_words <= wd;
      dma_max <= mb;
      dma_kicked <= 1'b1;
      after <= next_state;
      state <= S_WAIT;
    end
  endtask

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state <= S_IDLE;
      hdr_valid <= 1'b0;
      busy <= 1'b0;
      done <= 1'b0;
      dma_start <= 1'b0;
      dma_kicked <= 1'b0;
      eng_start <= 1'b0;
      eng_started <= 1'b0;
      mode <= M_HDR;
      run_cycles <= 32'd0;
      mac_cycles <= 32'd0;
    end else begin
      dma_start <= 1'b0;
      eng_start <= 1'b0;
      done <= 1'b0;
      if (busy) begin
        run_cycles <= run_cycles + 32'd1;
        if (eng_busy) mac_cycles <= mac_cycles + 32'd1;
      end
      if (p_valid && p_co != ob_co) begin
        ob_base <= ob_base + dsc_tile[12:0];
        ob_co <= p_co;
      end
      if (m_wpull) begin
        wr_idx <= wr_idx + 12'd1;
        if (wr_left == 16'd1) begin  // next segment: next output channel's row of the buffer
          wr_seg_idx <= wr_seg_idx + dsc_tile[12:1];
          wr_idx <= wr_seg_idx + dsc_tile[12:1];
          wr_left <= seg_words_out;
        end else begin
          wr_left <= wr_left - 16'd1;
        end
      end

      case (state)
        S_IDLE: begin
          if (hdr_load) begin
            hdr_valid <= 1'b0;
            kick(M_HDR, 1'b0, 21'd0, 21'd0, 8'd1, 16'd16, 9'd16, S_IDLE);
          end else if (go && hdr_valid) begin
            busy <= 1'b1;
            run_cycles <= 32'd0;
            mac_cycles <= 32'd0;
            T <= frames;
            op_i <= 8'd0;
            state <= S_NEXT;
          end
        end
        S_WAIT: if (dma_finished) begin
          dma_kicked <= 1'b0;
          state <= after;
          if (mode == M_HDR) hdr_valid <= 1'b1;
        end
        S_NEXT: begin  // next op
          if (op_i == n_ops) begin
            busy <= 1'b0;
            done <= 1'b1;
            state <= S_IDLE;
          end else begin
            kick(M_DESC, 1'b0, op_table + {9'd0, op_i, 4'd0}, 21'd0, 8'd1, 16'd16, 9'd16, S_DECODE);
          end
        end
        S_DECODE: begin
          tin <= {7'd0, T} << log2_rate(d[10][15:0]);
          tout <= {7'd0, T} << log2_rate(d[10][31:16]);
          in_base <= d[2][20:0] + (dsc_in_latent ? lat_off : 21'd0);
          out_base <= d[4][20:0] + (dsc_out_pcm ? pcm_off : 21'd0);
          kick(M_PARAM, 1'b0, d[9][20:0], 21'd0, 8'd1, {7'd0, dsc_cout, 1'b0}, 9'd256, S_PARAM);
        end
        S_PARAM: begin  // parameters loaded: first group
          co0 <= 8'd0;
          cnt <= dsc_group < dsc_cout ? dsc_group : dsc_cout;
          w_next <= d[8][20:0];
          grp_res <= d[6][20:0];
          grp_out <= out_base;
          state <= S_WGT;
        end
        S_WGT: begin
          // cnt * k * cin / 2 words
          kick(M_WGT, 1'b0, w_next, 21'd0, 8'd1, (({8'd0, cnt} * {11'd0, dsc_k}) << dsc_cin_log2) >> 1, 9'd256,
               S_TPREP);
          w_next <= w_next + ((({13'd0, cnt} * {16'd0, dsc_k}) << dsc_cin_log2) >> 1);
          t0 <= {TBITS{1'b0}};
          n <= (tout < {4'd0, dsc_tile}) ? tout : {4'd0, dsc_tile};
        end
        S_TPREP: begin
          lo <= lo_raw < 0 ? {SW{1'b0}} : lo_raw;
          hi <= hi_raw >= s_tin ? s_tin - 1 : hi_raw;
          state <= S_TILE;
        end
        S_TILE: begin
          // one segment per input channel: words (lo >> 1) .. (hi >> 1)
          kick(M_TILE, 1'b0, in_base + {1'b0, lo[TBITS:1]}, dsc_in_plane, 8'd1 << dsc_cin_log2,
               hi[TBITS:1] - lo[TBITS:1] + 16'd1, 9'd256, S_COMP);
        end
        S_COMP: begin
          if (!eng_started) begin
            eng_start <= 1'b1;
            eng_started <= 1'b1;
            ob_base <= 13'd0;
            ob_co <= co0[6:0];
            if (dsc_residual) begin
              mode <= M_RES;
              dma_start <= 1'b1;
              dma_we <= 1'b0;
              dma_first <= grp_res + {1'b0, t0[TBITS-1:1]};
              dma_stride <= dsc_res_plane;
              dma_nseg <= cnt;
              dma_words <= seg_words_out;
              dma_max <= 9'd32;
            end
          end else if (!eng_start && !eng_busy && !dma_busy && !dma_start) begin
            eng_started <= 1'b0;
            drain <= 4'd10;
            state <= S_DRAIN;
          end
        end
        S_DRAIN: begin  // the post unit's pipeline
          if (drain == 0) begin
            wr_idx <= 12'd0;
            wr_seg_idx <= 12'd0;
            wr_left <= seg_words_out;
            kick(M_WRITE, 1'b1, grp_out + {1'b0, t0[TBITS-1:1]}, dsc_out_plane, cnt, seg_words_out, 9'd256, S_NPREP);
          end else begin
            drain <= drain - 4'd1;
          end
        end
        S_NPREP: begin  // next tile, group or op
          if (t0 + n < tout) begin
            t0 <= t0 + n;
            n <= (tout - (t0 + n) < {4'd0, dsc_tile}) ? tout - (t0 + n) : {4'd0, dsc_tile};
            state <= S_TPREP;
          end else if ({1'b0, co0} + cnt < {1'b0, dsc_cout}) begin
            co0 <= co0 + cnt;
            cnt <= (dsc_cout - (co0 + cnt) < dsc_group) ? dsc_cout - (co0 + cnt) : dsc_group;
            grp_res <= grp_res + mul_plane(cnt, dsc_res_plane);
            grp_out <= grp_out + mul_plane(cnt, dsc_out_plane);
            state <= S_WGT;
          end else begin
            op_i <= op_i + 8'd1;
            state <= S_NEXT;
          end
        end
        default: state <= S_IDLE;
      endcase
    end
  end


  // group size x plane (at most 64 x 2^21): shift-add over the 7 bits of cnt
  function [20:0] mul_plane(input [7:0] c, input [20:0] p);
    integer j;
    begin
      mul_plane = 21'd0;
      for (j = 0; j < 8; j = j + 1) if (c[j]) mul_plane = mul_plane + (p << j);
    end
  endfunction

endmodule
