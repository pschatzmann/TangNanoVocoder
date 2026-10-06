#!/usr/bin/env bash
# vocoder_flash_boot (program image from the SPI flash at power-up) against a
# SPI NOR flash model.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../src"
mkdir -p "$here/build"
cd "$here"
iverilog -g2012 -o build/tb_flash_boot.vvp tb_flash_boot.v "$src/vocoder_flash_boot.v"
vvp -n build/tb_flash_boot.vvp | grep -v "finish called"
