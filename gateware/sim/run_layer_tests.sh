#!/usr/bin/env bash
# Runs the gateware layer tests: vocoder_conv_engine + vocoder_post against
# the phase 1 model's golden vectors, bit for bit.
#
#   gateware/sim/run_layer_tests.sh
#
# Builds model/ if needed and exports the program and golden vectors (for a
# short sentence) into gateware/sim/build/ on the first run.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
build="$here/build"
mkdir -p "$build"

if [ ! -f "$build/vocoder_q.bin" ] || [ ! -f "$build/golden/manifest.txt" ]; then
  cmake -S "$root/model" -B "$root/model/build" > /dev/null
  cmake --build "$root/model/build" -j > /dev/null
  mkdir -p "$build/golden"
  "$root/model/build/vocoder_model" --text "Hello world!" --export "$build/vocoder_q.bin" \
    --dump "$build/golden" 2> /dev/null | grep -E "fixed-point model|wrote"
fi

src="$here/../src"
iverilog -g2012 -DTANH_FILE="\"$src/vocoder_tanh.hex\"" -o "$build/tb_conv_layer.vvp" \
  "$here/tb_conv_layer.v" "$src/vocoder_conv_engine.v" "$src/vocoder_post.v" "$src/vocoder_act_banks.v" "$src/vocoder_mul.v"

# op:first output (-1 = the last outputs):count
tests="${TESTS:-0:0:160 0:-1:160 1:0:160 1:-1:160 2:0:160 3:-1:160 18:0:160 19:-1:160
  21:0:160 41:-1:160 61:0:160 81:-1:160 98:0:160 98:-1:160 99:0:160 101:0:160 101:-1:160}"
pass=0
fail=0
for t in $tests; do
  IFS=: read -r op t0 count <<< "$t"
  dir="$build/op${op}_${t0}"
  info=$(python3 "$here/prep_layer_test.py" "$build/vocoder_q.bin" "$build/golden" "$op" "$dir" --t0 "$t0" --count "$count")
  result=$(vvp -n "$build/tb_conv_layer.vvp" +dir="$dir" 2>&1 | grep -v -E "^WARNING|finish called")
  macs=$(echo "$info" | sed -E 's/.* ([0-9]+) MACs/\1/')
  cycles=$(echo "$result" | sed -n -E 's/^(PASS|FAIL).* ([0-9]+) cycles.*/\2/p')
  printf "op %-3s %s\n       %s, %d%% of the 16 lanes busy\n" "$op" "$info" "$(echo "$result" | grep -E "^(PASS|FAIL)")" \
    $(( cycles > 0 ? macs * 100 / 16 / cycles : 0 ))
  if echo "$result" | grep -q "^PASS"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "$result" | head -12; fi
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
