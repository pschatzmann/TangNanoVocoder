#!/usr/bin/env bash
# All gateware simulations: I/O (link over UART/SPI, I2S, sigma-delta) and
# the layer tests (engine + post against the phase 1 golden vectors).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../src"
mkdir -p "$here/build"
echo "== I/O"
iverilog -g2012 -o "$here/build/tb_io.vvp" "$here/tb_io.v" "$src/vocoder_link.v" "$src/vocoder_uart.v" \
  "$src/vocoder_spi_slave.v" "$src/vocoder_audio_out.v"
vvp -n "$here/build/tb_io.vvp" | grep -v "finish called" | tee "$here/build/tb_io.log"
grep -q "^PASS" "$here/build/tb_io.log"
echo "== layers"
"$here/run_layer_tests.sh"
