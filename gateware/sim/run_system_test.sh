#!/usr/bin/env bash
# Whole-system simulation: vocoder_system + SDRAM model, one short sentence
# (FRAMES latent frames of "Hello world!", default 12) against the model's
# PCM, bit for bit. Takes a few minutes.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
src="$here/../src"
out="$here/build/system"
frames="${FRAMES:-12}"
mkdir -p "$out/golden"

cmake -S "$root/model" -B "$root/model/build" > /dev/null
cmake --build "$root/model/build" -j > /dev/null
"$root/model/build/vocoder_model" --text "Hello world!" --crop "$frames" --max-tile "${MAX_TILE:-0}" --export-hw "$out/image.bin" \
  --sentence-out "$out/sentence.bin" --dump "$out/golden" 2> /dev/null | grep -E "wrote gateware"

python3 - "$out" <<'EOF'
import os, struct, sys
out = sys.argv[1]
img = open(os.path.join(out, "image.bin"), "rb").read()
with open(os.path.join(out, "image.hex"), "w") as f:
    f.writelines("%08x\n" % w for w in struct.unpack("<%dI" % (len(img) // 4), img))
with open(os.path.join(out, "sentence.hex"), "w") as f:
    f.writelines("%02x\n" % b for b in open(os.path.join(out, "sentence.bin"), "rb").read())
last = [l.split() for l in open(os.path.join(out, "golden", "manifest.txt"))][-1]
data = open(os.path.join(out, "golden", last[0]), "rb").read()
with open(os.path.join(out, "pcm.hex"), "w") as f:
    f.writelines("%04x\n" % (v & 0xFFFF) for v in struct.unpack("<%dh" % (len(data) // 2), data))
EOF

cd "$here"
iverilog -g2012 -DTANH_FILE="\"$src/vocoder_tanh.hex\"" -o "$out/tb_system.vvp" tb_system.v sdram_model.v \
  "$src"/vocoder_system.v "$src"/vocoder_core.v "$src"/vocoder_dma.v "$src"/vocoder_conv_engine.v \
  "$src"/vocoder_post.v "$src"/vocoder_act_banks.v "$src"/vocoder_mul.v "$src"/vocoder_link.v \
  "$src"/vocoder_uart.v "$src"/vocoder_spi_slave.v "$src"/vocoder_audio_out.v "$src"/vocoder_playback.v \
  "$src"/vocoder_sdram_arb.v "$src"/sdram_ctrl.v
vvp -n "$out/tb_system.vvp" | grep -v -E "^VCD|finish called|readmemh"
