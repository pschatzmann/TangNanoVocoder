#!/usr/bin/env python3
"""Builds the vocoder bitstream, with the open-source flow (yosys ->
nextpnr-himbaechel -> gowin_pack) or, with --gowin, Gowin's own tools.

  gateware/build.py [--gowin] [--mhz 54|64.8] [--seed N] [--load] [--flash]

Output: gateware/build/vocoder.fs, plus the log with utilization and timing
(build/pnr.log, or build/gowin/impl/pnr/ for --gowin). --load writes the
bitstream into the FPGA's SRAM with openFPGALoader (lost at power-off),
--flash into its flash.

The design with the flow needs --gowin: Gowin's synthesis maps it to about
80% of the GW2AR-18's logic, yosys + nextpnr to more than 100% (yosys's
mapping for Gowin is much less compact). The vocoder alone fits either way.
Gowin's IDE is looked for in $GOWIN_HOME, else ~/gowin (gw_sh in IDE/bin).
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
    "vocoder_top.v", "vocoder_system.v", "vocoder_core.v", "vocoder_flow_unit.v", "vocoder_dma.v",
    "vocoder_flash_boot.v",
    "vocoder_conv_engine.v",
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


def gowin_build(a):
    """Synthesis, place and route with Gowin's gw_sh (batch mode)."""
    home = Path(os.environ.get("GOWIN_HOME", Path.home() / "gowin"))
    ide = home / "IDE"
    gw_sh = ide / "bin" / "gw_sh"
    if not gw_sh.exists():
        sys.exit(f"gw_sh not found in {ide}/bin (set GOWIN_HOME)")
    hz, idiv, fbdiv, odiv = PLL[a.mhz]
    out = BUILD / "gowin"
    out.mkdir(parents=True, exist_ok=True)
    (out / "defines.v").write_text(
        "`define SYNTHESIS\n"
        f"`define VOCODER_CLK_HZ {hz}\n`define VOCODER_PLL_IDIV {idiv}\n"
        f"`define VOCODER_PLL_FBDIV {fbdiv}\n`define VOCODER_PLL_ODIV {odiv}\n")
    for hexfile in SRC.glob("*.hex"):  # $readmemh paths are relative to the run directory
        shutil.copy(hexfile, out)
    tcl = [f"set_device {DEVICE} -name GW2AR-18C", f"add_file {out / 'defines.v'}"]
    tcl += [f"add_file {SRC / f}" for f in SOURCES]
    tcl += [f"add_file {CST}", "set_option -top_module vocoder_top", "set_option -verilog_std sysv2017",
            "set_option -output_base_name vocoder", "set_option -use_mspi_as_gpio 1",
            "set_option -use_sspi_as_gpio 1", "set_option -use_ready_as_gpio 1", "set_option -use_done_as_gpio 1",
            "set_option -use_cpu_as_gpio 1", "run all"]
    (out / "run.tcl").write_text("\n".join(tcl) + "\n")
    # the IDE's own Qt and libraries; the system's freetype (the bundled one
    # clashes with the system's fontconfig); no GL
    env = dict(os.environ, QT_XCB_GL_INTEGRATION="none", LIBGL_ALWAYS_SOFTWARE="1",
               QT_PLUGIN_PATH=str(ide / "plugins"), LD_LIBRARY_PATH=str(ide / "lib"))
    ft = sorted(Path("/lib/x86_64-linux-gnu").glob("libfreetype.so.6*"))
    if ft:
        env["LD_PRELOAD"] = str(ft[0])
    log = out / "gw.log"
    print(f"+ {gw_sh} run.tcl (log: {log})", flush=True)
    with open(log, "w") as f:
        r = subprocess.run([str(gw_sh), "run.tcl"], cwd=out, env=env, stdout=f, stderr=subprocess.STDOUT)
    text = log.read_text(errors="ignore")
    fs = out / "impl" / "pnr" / "vocoder.fs"
    if r.returncode != 0 or "ERROR" in text or not fs.exists():
        print("\n".join(l for l in text.splitlines() if "ERROR" in l)[:4000])
        sys.exit(f"Gowin build failed, see {log}")
    rpt = (out / "impl" / "pnr" / "vocoder.rpt.txt").read_text(errors="ignore")
    for line in rpt.splitlines():
        if re.match(r"\s+(Logic|Register|CLS|BSRAM|DSP)\s+\|", line):
            print(line.strip())
    tr = (out / "impl" / "pnr" / "vocoder_tr_content.html").read_text(errors="ignore")
    tr = re.sub(r"\|[\s|]*", "|", re.sub(r"<[^>]+>", "|", tr))
    m = re.search(r"CLKOUT\.default_gen_clk\|([\d.]+)\(MHz\)\|([\d.]+)\(MHz\)", tr)
    if m:
        ok = float(m.group(2)) >= float(m.group(1))
        print(f"Fmax {m.group(2)} MHz for {m.group(1)} MHz: {'PASS' if ok else 'FAIL'}")
    dst = BUILD / "vocoder.fs"
    dst.unlink(missing_ok=True)
    shutil.copyfile(fs, dst)  # (Gowin writes it read-only)
    print(f"bitstream: {dst}")


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mhz", default="54", choices=sorted(PLL))
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--load", action="store_true", help="load into the FPGA's SRAM")
    ap.add_argument("--flash", action="store_true", help="write to the FPGA's flash")
    ap.add_argument("--gowin", action="store_true", help="Gowin's synthesis, place and route (gw_sh)")
    a = ap.parse_args()

    tools = toolchain()
    if a.gowin:
        BUILD.mkdir(exist_ok=True)
        gowin_build(a)
        if a.load or a.flash:
            run([tools["openFPGALoader"], "-b", "tangnano20k"] + (["-f"] if a.flash else []) + [str(BUILD / "vocoder.fs")])
        return
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
