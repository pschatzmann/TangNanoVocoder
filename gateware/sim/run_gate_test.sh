#!/usr/bin/env bash
# Gate-level check: the system test (tb_gate.v) on yosys's synthesized
# netlist of vocoder_system, against the same test on the RTL. OPS limits
# how many ops run (default 1); FRAMES is the sentence length (default 4).
#   gateware/sim/run_gate_test.sh [rtl|gate|both]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
src="$here/../src"
out="$here/build/gate"
mode="${1:-both}"
ops="${OPS:-1}"
frames="${FRAMES:-4}"
tools=$(ls -d ~/.arduino15/packages/nanotang/tools/oss-cad-suite-gowin/*/ | tail -1)
mkdir -p "$out/golden"

"$root/model/build/vocoder_model" --text "Hello world!" --crop "$frames" --export-hw "$out/image.bin" \
  --sentence-out "$out/sentence.bin" --dump "$out/golden" > /dev/null 2>&1
python3 - "$out" "$ops" <<'PY'
import os, struct, sys
out, ops = sys.argv[1], int(sys.argv[2])
img = bytearray(open(os.path.join(out, "image.bin"), "rb").read())
struct.pack_into("<I", img, 8, ops)
W = struct.unpack("<%dI" % (len(img) // 4), img)
open(os.path.join(out, "image.hex"), "w").writelines("%08x\n" % w for w in W)
open(os.path.join(out, "sentence.hex"), "w").writelines(
    "%02x\n" % b for b in open(os.path.join(out, "sentence.bin"), "rb").read())
d = W[16 + 16 * (ops - 1): 32 + 16 * (ops - 1)]
man = [l.split() for l in open(os.path.join(out, "golden", "manifest.txt"))]
f, _, name, fr, ch, _ = man[ops]
fr, ch = int(fr), int(ch)
data = struct.unpack("<%dh" % (fr * ch), open(os.path.join(out, "golden", f), "rb").read())
with open(os.path.join(out, "expect.hex"), "w") as e:
    e.write(f"{d[4]} {d[5]} {ch} {fr}\n")
    e.writelines("%04x\n" % (data[t * ch + c] & 0xFFFF) for c in range(ch) for t in range(fr))
print(f"checking op {ops - 1} ({name}): {ch} channels x {fr} frames")
PY

srcs="vocoder_system.v vocoder_core.v vocoder_dma.v vocoder_conv_engine.v vocoder_post.v vocoder_act_banks.v
  vocoder_mul.v vocoder_link.v vocoder_uart.v vocoder_spi_slave.v vocoder_audio_out.v vocoder_playback.v
  vocoder_sdram_arb.v sdram_ctrl.v"
srcs=$(echo $srcs)  # one line: yosys takes a newline as the end of a command
cd "$here"
if [ "$mode" != gate ]; then
  iverilog -g2012 -DTANH_FILE="\"$src/vocoder_tanh.hex\"" -o "$out/rtl.vvp" tb_gate.v sdram_model.v \
    $(for f in $srcs; do echo "$src/$f"; done)
  echo "== RTL"; vvp -n "$out/rtl.vvp" | grep -v -E "finish called|readmemh"
fi
if [ "$mode" != rtl ]; then
  (cd "$src" && "$tools/bin/yosys" -q -p "read_verilog -DSYNTHESIS $srcs;
     chparam -set CLK_HZ 54000000 -set BAUD 3375000 -set INIT_US 2 -set TANH_FILE \"$src/vocoder_tanh.hex\" vocoder_system;
     synth_gowin -top vocoder_system; simplemap t:\$buf; opt_clean; write_json $out/netlist.json")
  python3 -c "import sys, pathlib; sys.path.insert(0, '$here/..'); import build; print('fix_bram_oce:', build.fix_bram_oce(pathlib.Path('$out/netlist.json')))"
  "$tools/bin/yosys" -q -p "read_json $out/netlist.json; write_verilog -noattr $out/netlist.v"
  iverilog -g2012 -DGATE -o "$out/gate.vvp" tb_gate.v sdram_model.v gowin_dsp_models.v "$src/vocoder_uart.v" \
    "$out/netlist.v" "$tools/share/yosys/gowin/cells_sim.v"
  echo "== gate level"; vvp -n "$out/gate.vvp" | grep -v -E "finish called|readmemh"
fi
