#!/usr/bin/env python3
"""Builds the vocoder bitstream with the open-source flow:
yosys -> nextpnr-himbaechel -> gowin_pack.

  gateware/build.py [--mhz 54|64.8] [--seed N] [--load] [--flash]

Output: gateware/build/vocoder.fs, plus nextpnr's log (utilization and
timing) in gateware/build/pnr.log. --load writes the bitstream into the
FPGA's SRAM with openFPGALoader (lost at power-off), --flash into its flash.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "src"
BUILD = HERE / "build"
CST = HERE / "constraints" / "tangnano20k.cst"
DEVICE = "GW2AR-LV18QN88C8/I7"
FAMILY = "GW2A-18C"

SOURCES = [
    "vocoder_top.v", "vocoder_system.v", "vocoder_core.v", "vocoder_dma.v", "vocoder_conv_engine.v",
    "vocoder_post.v", "vocoder_act_banks.v", "vocoder_mul.v", "vocoder_link.v", "vocoder_uart.v",
    "vocoder_spi_slave.v", "vocoder_audio_out.v", "vocoder_playback.v", "vocoder_sdram_arb.v", "sdram_ctrl.v",
]

# 27 MHz * (FBDIV + 1) / (IDIV + 1), VCO = output * ODIV
PLL = {"54": (54_000_000, 0, 1, 16), "64.8": (64_800_000, 4, 11, 8)}

# Block RAM ports whose output clock enable yosys 0.33 ties low; from
# arduino-tangnano20k's tools/build_bitstream.py (fix_bram_oce), where it is
# verified on hardware: a block with OCE low never updates its output.
BRAM_OCE_PORTS = {
    "SP": [("OCE", "CE")], "SPX9": [("OCE", "CE")],
    "SDPB": [("OCE", "CEB")], "SDPX9B": [("OCE", "CEB")],
    "DPB": [("OCEA", "CEA"), ("OCEB", "CEB")], "DPX9B": [("OCEA", "CEA"), ("OCEB", "CEB")],
    "pROM": [("OCE", "CE")], "pROMX9": [("OCE", "CE")],
}


def fix_bram_oce(json_path):
    netlist = json.loads(json_path.read_text())
    changed = 0
    for module in netlist["modules"].values():
        for cell in module.get("cells", {}).values():
            conns = cell["connections"]
            for oce, ce in BRAM_OCE_PORTS.get(cell["type"], []):
                if oce in conns and ce in conns and conns[oce] != conns[ce]:
                    conns[oce] = list(conns[ce])
                    changed += 1
    if changed:
        json_path.write_text(json.dumps(netlist))
    return changed


def toolchain():
    """The FPGA tools: $TNV_TOOLS/bin, else the toolchain arduino-tangnano20k
    installs (yosys 0.69 era - distribution yosys 0.33 emits block RAM cells
    nextpnr 0.11 can't place), else PATH."""
    dirs = []
    if os.environ.get("TNV_TOOLS"):
        dirs.append(Path(os.environ["TNV_TOOLS"]) / "bin")
    dirs += sorted(Path.home().glob(".arduino15/packages/nanotang/tools/oss-cad-suite-gowin/*/bin"), reverse=True)
    for d in dirs:
        if (d / "yosys").exists() and (d / "nextpnr-himbaechel").exists():
            return {t: str(d / t) for t in ("yosys", "nextpnr-himbaechel", "gowin_pack", "openFPGALoader")}
    return {t: shutil.which(t) or t for t in ("yosys", "nextpnr-himbaechel", "gowin_pack", "openFPGALoader")}


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mhz", default="54", choices=sorted(PLL))
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--load", action="store_true", help="load into the FPGA's SRAM")
    ap.add_argument("--flash", action="store_true", help="write to the FPGA's flash")
    a = ap.parse_args()

    tools = toolchain()
    print("tools: " + ", ".join(f"{k}={v}" for k, v in tools.items()))
    hz, idiv, fbdiv, odiv = PLL[a.mhz]
    BUILD.mkdir(exist_ok=True)
    defines = (f"-DSYNTHESIS -DVOCODER_CLK_HZ={hz} -DVOCODER_PLL_IDIV={idiv} -DVOCODER_PLL_FBDIV={fbdiv} "
               f"-DVOCODER_PLL_ODIV={odiv}")
    json_path = BUILD / "vocoder.json"
    # simplemap t:$buf: see arduino-tangnano20k's build (newer yosys leaves $buf cells nextpnr can't place)
    run([tools["yosys"], "-q", "-l", str(BUILD / "yosys.log"), "-p",
         f"read_verilog {defines} {' '.join(SOURCES)}; synth_gowin -top vocoder_top; "
         f"simplemap t:$buf; opt_clean; stat; write_json {json_path}"], cwd=SRC)
    print(f"fix_bram_oce: {fix_bram_oce(json_path)} block RAM ports")

    pnr_json = BUILD / "vocoder_pnr.json"
    log = BUILD / "pnr.log"
    seeds = [a.seed] if a.seed is not None else [1, 2, 3, 4]
    for seed in seeds:
        cmd = [tools["nextpnr-himbaechel"], "--json", str(json_path), "--write", str(pnr_json), "--device", DEVICE,
               "--vopt", f"family={FAMILY}", "--vopt", f"cst={CST}", "--freq", str(float(a.mhz)),
               "--seed", str(seed), "-l", str(log)]
        print("+ " + " ".join(cmd), flush=True)
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0:
            break
        print(f"place & route failed with seed {seed}, see {log}")
    else:
        sys.exit(1)

    text = log.read_text()
    for line in text.splitlines():
        if re.search(r"Max frequency for clock|LUT4|DFF|BSRAM|MULT|ALU|IOB", line) and "Info:" in line:
            print(line.strip())

    fs = BUILD / "vocoder.fs"
    run([tools["gowin_pack"], "-d", FAMILY, "-o", str(fs), str(pnr_json)])
    print(f"bitstream: {fs}")
    if a.load or a.flash:
        run([tools["openFPGALoader"], "-b", "tangnano20k"] + (["-f"] if a.flash else []) + [str(fs)])


if __name__ == "__main__":
    main()
