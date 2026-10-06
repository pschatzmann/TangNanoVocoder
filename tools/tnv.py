#!/usr/bin/env python3
"""Talks to the vocoder on a Tang Nano 20K over a serial port (the board's
USB serial bridge, or the header UART): uploads the program image, sends
sentences, and checks speed and audio against the fixed-point model.

  tnv.py PORT status
  tnv.py PORT upload [IMAGE]          # default: build one with vocoder_model (flow + vocoder;
                                      # --vocoder-only before the command: vocoder alone)
  tnv.py PORT flash-image [IMAGE]     # store the image in the board's flash: loaded at every
                                      # power-up (with a bitstream from gateware/build.py --flash)
  tnv.py PORT calibrate               # find the SDRAM read timing (after upload)
  tnv.py PORT debug                   # scheduler state and the header as the core read it
  tnv.py PORT say "Some text." [--verify] [--wav out.wav]

`say` makes the sentence's latent with vocoder_model (TinyTTS on the PC, the
part the ESP32 will do), sends it, waits until it has been computed and
played, and reports the board's compute time against the audio's length.
--verify reads the PCM back from the board's SDRAM and compares it with the
model's, sample by sample (bit-exact means the board's audio quality is the
model's: docs/studies.md); --wav saves what the board computed.

Needs pyserial. Protocol: docs/gateware.md, "Link".
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL = os.path.join(ROOT, "model", "build", "vocoder_model")
CLK_HZ = 54_000_000
# the program image's place in the board's SPI flash (vocoder_flash_boot.v
# OFFSET; the bitstream is below it, under 1MB)
FLASH_IMAGE_OFFSET = 0x100000
FLAGS = ["READY", "RECEIVING", "BUSY", "ERROR", "PROGRAM"]


def flags(status):
    return " ".join(f for i, f in enumerate(FLAGS) if status >> i & 1) or "-"


class Vocoder:
    def __init__(self, port, baud):
        self.s = serial.Serial(port, baud, timeout=2)
        time.sleep(0.1)
        self.s.reset_input_buffer()

    def status(self):
        self.s.write(b"?")
        r = self.s.read(1)
        if len(r) != 1 or r[0] & 0xE0 != 0x80:
            sys.exit(f"no answer from the vocoder (got {r!r}) - bitstream loaded? right port and baud?")
        return r[0]

    def stats(self):
        self.s.write(b"T")
        r = self.s.read(10)
        if len(r) != 10:
            sys.exit("no statistics reply")
        _, run, mac, slot = struct.unpack("<BIIB", r)
        return run, mac, slot

    def debug(self):
        self.s.write(b"D")
        r = self.s.read(16)
        if len(r) != 16:
            sys.exit("no debug reply")
        return {
            "state": r[0], "op": r[1], "ops": r[2], "op_table": r[3] | r[4] << 8, "rd_lat": r[5],
            "max_frames": r[6] | r[7] << 8, "lat_base0": r[8] | r[9] << 8 | r[10] << 16,
            "lat_plane": r[11] | r[12] << 8, "slots": f"{r[13]:08b}", "flags": f"{r[14]:08b}",
            "frames0_lo": r[15],
        }

    def set_latency(self, lat):
        self.s.write(b"L" + bytes([lat]))
        self.s.flush()
        time.sleep(0.01)

    def read_words(self, addr, count):
        out = bytearray()
        while count:
            n = min(count, 4096)
            self.s.write(b"R" + struct.pack("<IH", addr, n))
            data = self.s.read(4 * n)
            if len(data) != 4 * n:
                sys.exit(f"readback: got {len(data)} of {4 * n} bytes")
            out += data
            addr += n
            count -= n
        return bytes(out)

    def wait(self, cond, what, timeout=120):
        t0 = time.time()
        while True:
            st = self.status()
            if cond(st):
                return st
            if time.time() - t0 > timeout:
                sys.exit(f"timeout waiting for {what} (status {flags(st)})")
            time.sleep(0.02)


# the flow's fixed-point settings, as in tools/make_image_header.py; empty
# with --vocoder-only (the image without the flow: sentences are z, not z_p)
FLOW_ARGS = ["--flow", "--flow-wbits", "12", "--flow-headroom", "1.25"]


def build_image(tmp):
    path = os.path.join(tmp, "image.bin")
    subprocess.run([MODEL, "--text", "Hello world!", "--export-hw", path] + FLOW_ARGS, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return path


def cmd_upload(v, image_path):
    data = open(image_path, "rb").read()
    st = v.status()
    if st & 4:
        sys.exit("the vocoder is busy; wait until it has finished playing")
    t0 = time.time()
    v.s.write(b"P" + struct.pack("<I", len(data)))
    for i in range(0, len(data), 4096):
        v.s.write(data[i:i + 4096])
        print(f"\rupload {100 * (i + 4096) // len(data):3d}%", end="", flush=True)
    v.s.write(bytes([sum(data) & 0xFF]))
    v.s.flush()
    st = v.wait(lambda s: not s & 2, "the upload to finish")
    print(f"\ruploaded {len(data)} bytes in {time.time() - t0:.1f}s, status {flags(st)}")
    if not st & 16:
        sys.exit("program not accepted")


def open_fpga_loader():
    """openFPGALoader: the one arduino-tangnano20k installs (as gateware/build.py
    uses), else PATH. Distribution packages can be too old: v0.12 can't write
    raw data at a flash offset."""
    cands = sorted(glob.glob(os.path.expanduser(
        "~/.arduino15/packages/nanotang/tools/oss-cad-suite-gowin/*/bin/openFPGALoader")))
    found = cands[-1] if cands else shutil.which("openFPGALoader")
    if not found:
        sys.exit("openFPGALoader not found (install the arduino-tangnano20k core, docs/installation.md)")
    return found


def flash_record(image):
    """What vocoder_flash_boot.v reads: "TNVF", u32 byte count, image, checksum."""
    h = struct.unpack("<16I", image[:64])
    if h[0] != 0x48564E54 or h[10] * 4 != len(image):
        sys.exit("not a program image (TNVH)")
    return b"TNVF" + struct.pack("<I", len(image)) + image + bytes([sum(image) & 0xFF])


def cmd_flash_image(v, image_path):
    """Writes the image into the board's flash. Writing makes the FPGA
    reconfigure from flash; the bitstream there then loads the image."""
    image = open(image_path, "rb").read()
    with tempfile.TemporaryDirectory() as tmp:
        rec = os.path.join(tmp, "tnv_image.bin")
        open(rec, "wb").write(flash_record(image))
        cmd = [open_fpga_loader(), "-b", "tangnano20k", "-f", "-o", str(FLASH_IMAGE_OFFSET), rec]
        print("+ " + " ".join(cmd), flush=True)
        t0 = time.time()
        subprocess.run(cmd, check=True)
    print(f"wrote {len(image)} bytes at flash offset 0x{FLASH_IMAGE_OFFSET:x} in {time.time() - t0:.0f}s")
    # the board reconfigured and loads the image from flash (about 1s)
    time.sleep(0.5)
    v.s.reset_input_buffer()
    t0 = time.time()
    st = -1
    v.s.timeout = 0.2
    while time.time() - t0 < 10:  # no answer while it loads: the host's bytes are ignored then
        v.s.reset_input_buffer()
        v.s.write(b"?")
        r = v.s.read(1)
        st = r[0] if len(r) == 1 and r[0] & 0xE0 == 0x80 else -1
        if st >= 0 and st & 16 and not st & 2:
            break
        time.sleep(0.1)
    v.s.timeout = 2
    print(f"loaded after {time.time() - t0:.1f}s")
    header = struct.unpack("<16I", image[:64])
    ok, d = header_ok(v, header)
    if st >= 0 and st & 16 and ok:
        print(f"the board loaded it from flash: status {flags(st)}")
    else:
        sys.exit(f"the board didn't load the image from flash (status {flags(st) if st >= 0 else 'no answer'}) - "
                 "is the bitstream in flash one with the flash loader (gateware/build.py --gowin --flash)?")


def header_ok(v, image_header):
    """The header as the core read it (a 16-word burst) against the image."""
    d = v.debug()
    want = {"ops": image_header[2], "op_table": image_header[3], "max_frames": image_header[4],
            "lat_base0": image_header[5], "lat_plane": image_header[7]}
    return all(d[k] == w for k, w in want.items()), d


def cmd_calibrate(v):
    """Finds the SDRAM read sample point: for each, the core re-reads the
    header (a zero-length 'P' upload keeps the image) and 'D' shows it."""
    with tempfile.TemporaryDirectory() as tmp:
        header = struct.unpack("<16I", open(build_image(tmp), "rb").read(64))
    # sample points in time order: half clocks from E+CAS-0.5 to E+CAS+2
    good = []
    for lat in (4, 0, 5, 1, 6, 2):
        v.set_latency(lat)
        v.s.write(b"P" + struct.pack("<I", 0) + b"\x00")
        v.s.flush()
        time.sleep(0.05)
        ok, d = header_ok(v, header)
        print(f"read sample point {lat} ({(lat & 3) - (0.5 if lat & 4 else 0):+.1f} clocks): header {'ok' if ok else 'WRONG'}  "
              f"(ops {d['ops']}, op table {d['op_table']}, max frames {d['max_frames']}, "
              f"latent base {d['lat_base0']}, plane {d['lat_plane']})")
        if ok:
            good.append(lat)
    if not good:
        sys.exit("no read sample point reads the header correctly")
    lat = good[len(good) // 2]
    v.set_latency(lat)
    v.s.write(b"P" + struct.pack("<I", 0) + b"\x00")
    v.s.flush()
    time.sleep(0.05)
    print(f"using read sample point {lat}")


def cmd_say(v, text, verify, wav_path):
    with tempfile.TemporaryDirectory() as tmp:
        sentence, pcm_path = os.path.join(tmp, "s.bin"), os.path.join(tmp, "pcm.bin")
        image = os.path.join(tmp, "image.bin")
        t0 = time.time()
        subprocess.run([MODEL, "--text", text, "--sentence-out", sentence, "--pcm-out", pcm_path,
                        "--export-hw", image] + FLOW_ARGS, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t_model = time.time() - t0
        packet = open(sentence, "rb").read()
        expected = open(pcm_path, "rb").read()
        header = struct.unpack("<16I", open(image, "rb").read(64))
    frames = struct.unpack("<H", packet[1:3])[0]
    seconds = frames * 512 / 44100
    print(f"\"{text}\": {frames} latent frames, {seconds:.2f}s of audio (latent made on the PC in {t_model:.1f}s)")

    st = v.wait(lambda s: s & 1, "READY")
    t_send = time.time()
    v.s.write(packet)
    v.s.flush()
    v.wait(lambda s: s & 4, "BUSY", timeout=10)
    t_busy = time.time()
    st = v.wait(lambda s: not s & 4, "computing and playing", timeout=60 + 3 * seconds)
    t_end = time.time()
    if st & 8:
        sys.exit("the vocoder reported an error")
    run, mac, slot = v.stats()
    compute = run / CLK_HZ
    print(f"board: computed in {compute:.3f}s = {compute / seconds:.2f} x real time "
          f"(engine busy {100 * mac / max(run, 1):.0f}% of it)")
    print(f"host: sent in {t_busy - t_send:.2f}s, computing + playing took {t_end - t_busy:.2f}s")

    if verify or wav_path:
        pcm_base = header[8 + slot]
        data = v.read_words(pcm_base, frames * 256)
        if verify:
            got = struct.unpack(f"<{frames * 512}h", data)
            want = struct.unpack(f"<{frames * 512}h", expected)
            bad = sum(g != w for g, w in zip(got, want))
            print(f"verify: {len(got)} samples read back, {bad} differ from the fixed-point model"
                  + (" - bit-exact" if bad == 0 else ""))
        if wav_path:
            with wave.open(wav_path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(44100)
                w.writeframes(data)
            print(f"wrote {wav_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--vocoder-only", action="store_true",
                    help="the image without the flow (the PC runs the flow and sends z)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    sub.add_parser("debug")
    sub.add_parser("calibrate")
    up = sub.add_parser("upload")
    up.add_argument("image", nargs="?")
    fl = sub.add_parser("flash-image")
    fl.add_argument("image", nargs="?")
    say = sub.add_parser("say")
    say.add_argument("text")
    say.add_argument("--verify", action="store_true")
    say.add_argument("--wav")
    a = ap.parse_args()
    if a.vocoder_only:
        FLOW_ARGS.clear()

    v = Vocoder(a.port, a.baud)
    if a.cmd == "status":
        print(flags(v.status()))
    elif a.cmd == "debug":
        for k, val in v.debug().items():
            print(f"{k:12s} {val}")
    elif a.cmd == "calibrate":
        cmd_calibrate(v)
    elif a.cmd == "upload":
        if a.image:
            cmd_upload(v, a.image)
        else:
            with tempfile.TemporaryDirectory() as tmp:
                cmd_upload(v, build_image(tmp))
    elif a.cmd == "flash-image":
        if a.image:
            cmd_flash_image(v, a.image)
        else:
            with tempfile.TemporaryDirectory() as tmp:
                cmd_flash_image(v, build_image(tmp))
    elif a.cmd == "say":
        cmd_say(v, a.text, a.verify, a.wav)


if __name__ == "__main__":
    main()
