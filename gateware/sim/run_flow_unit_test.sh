#!/usr/bin/env bash
# vocoder_flow_unit (LayerNorm, attention) against the model's golden vectors.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
src="$here/../src"
mkdir -p "$here/build/flow"
"$root/model/build/vocoder_model" --flow --flow-wbits 12 --flow-headroom 1.25 --flow-dump "$here/build/flow" \
  --crop "${FRAMES:-40}" --text "${TEXT:-Hello world!}" 2> /dev/null | grep "flow unit"
cd "$here"
iverilog -g2012 -DEXP_FILE="\"$src/vocoder_exp2.hex\"" -DRSQRT_FILE="\"$src/vocoder_rsqrt.hex\"" \
  -o build/tb_flow_unit.vvp tb_flow_unit.v "$src/vocoder_flow_unit.v" "$src/vocoder_mul.v" "$src/vocoder_act_banks.v"
vvp -n build/tb_flow_unit.vvp | grep -v -E "finish called|readmemh"
