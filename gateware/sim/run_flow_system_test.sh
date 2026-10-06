#!/usr/bin/env bash
# The flow on the whole chip: an image with the flow, a z_p sentence of
# FRAMES latent frames, only the flow's ops run (header op count), and the
# latent slot - z after the flow - compared with the model's integers.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
src="$here/../src"
out="${OUT:-$here/build/flowsys}"
frames="${FRAMES:-8}"
mkdir -p "$out"
"$root/model/build/vocoder_model" --flow --flow-wbits 12 --flow-headroom 1.25 --text "Hello world!" --crop "$frames" \
  --export-hw "$out/image.bin" --sentence-out "$out/sentence.bin" --z-out "$out/z.bin" \
  $( [ "${OPS:-flow}" != flow ] && echo --trace-op "$OPS" "$out/trace.hex" ) 2> /dev/null |
  grep -E "hardware image|first sentence|output buffer"
python3 - "$out" "${OPS:-flow}" <<'PY'
import os, struct, sys
out, ops = sys.argv[1], sys.argv[2]
img = bytearray(open(os.path.join(out, "image.bin"), "rb").read())
W = struct.unpack("<%dI" % (len(img) // 4), img)
n_ops = W[14] if ops == "flow" else int(ops)
struct.pack_into("<I", img, 8, n_ops)
open(os.path.join(out, "image.hex"), "w").writelines("%08x\n" % w for w in struct.unpack("<%dI" % (len(img) // 4), img))
open(os.path.join(out, "sentence.hex"), "w").writelines("%02x\n" % b for b in open(os.path.join(out, "sentence.bin"), "rb").read())
z = open(os.path.join(out, "z.bin"), "rb").read()
T = len(z) // 64
zv = struct.unpack("<%dh" % (T * 32), z)
if ops != "flow":  # the traced op's output buffer (vocoder_model --trace-op)
    open(os.path.join(out, "expect.hex"), "w").write(open(os.path.join(out, "trace.hex")).read())
else:
    with open(os.path.join(out, "expect.hex"), "w") as e:
        e.write(f"{W[5]} {W[7]} 32 {T}\n")
        e.writelines("%04x\n" % (zv[t * 32 + c] & 0xFFFF) for c in range(32) for t in range(T))
print(f"running {n_ops} ops on {T} frames; checking the latent slot (z after the flow)")
PY
cd "$here"
srcs="vocoder_system.v vocoder_flash_boot.v vocoder_core.v vocoder_flow_unit.v vocoder_dma.v vocoder_conv_engine.v vocoder_post.v
  vocoder_act_banks.v vocoder_mul.v vocoder_link.v vocoder_uart.v vocoder_spi_slave.v vocoder_audio_out.v
  vocoder_playback.v vocoder_sdram_arb.v sdram_ctrl.v"
iverilog -g2012 -DTANH_FILE="\"$src/vocoder_tanh.hex\"" -DEXP_FILE="\"$src/vocoder_exp2.hex\"" \
  -DRSQRT_FILE="\"$src/vocoder_rsqrt.hex\"" -DGATE_DIR="\"$out\"" -o "$out/sim.vvp" tb_gate.v sdram_model.v \
  $(for f in $srcs; do echo "$src/$f"; done)
vvp -n "$out/sim.vvp" | grep -v -E "finish called|readmemh|status 95"
