#!/usr/bin/env python3
"""Builds a gateware layer test from the phase 1 model's output.

Reads the quantized program (vocoder_model --export) and the golden vectors
(vocoder_model --dump), picks one op and a range of its output frames, and
writes what tb_conv_layer.v needs into OUT_DIR:

  cfg.hex      op configuration, one value per line (see tb_conv_layer.v)
  act.hex      the input tile, leaky ReLU applied, in load order: one line
               per value, "bank addr data"
  weights.hex  [cout][k][cin] (16 bit)
  params.hex   per output channel: bias (32 bit), mult (16 bit), shift
  res.hex      residual for every expected output, [t][co] (0 if none)
  expect.hex   expected outputs [t - t0][co]

  prep_layer_test.py PROGRAM GOLDEN_DIR OP OUT_DIR [--t0 N] [--count N]

--t0 -1 picks the last `count` outputs, so both sequence edges get tested.
"""
import argparse
import os
import struct
import sys

LANES = 16
BANK_DEPTH = 1024


class Reader:
    def __init__(self, data):
        self.b, self.p = data, 0

    def get(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.p)
        self.p += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]

    def string(self):
        n = self.b[self.p]
        s = self.b[self.p + 1:self.p + 1 + n].decode()
        self.p += 1 + n
        return s


def load_program(path):
    r = Reader(open(path, "rb").read())
    if r.b[:4] != b"TNVQ":
        sys.exit(f"{path}: not a TNVQ program")
    r.p = 4
    if r.get("I") != 1:
        sys.exit(f"{path}: unsupported version")
    groups = []
    for _ in range(r.get("I")):
        bits, fixed, scale = r.get("BBf")
        groups.append(dict(bits=bits, fixed=fixed, scale=scale, name=r.string()))
    buffers = []
    for _ in range(r.get("I")):
        ch, rate, group = r.get("HHH")
        buffers.append(dict(channels=ch, rate=rate, group=group, name=r.string()))
    inp, out = r.get("II")
    ops = []
    for _ in range(r.get("I")):
        kind, wbits, lr_mul = r.get("BBH")
        io = r.get("5h")
        cin, cout, k, dil, stride, pad = r.get("6H")
        op = dict(kind=kind, wbits=wbits, lr_mul=lr_mul, inp=io[0], out=io[1], residual=io[2],
                  in2=io[3], in3=io[4], cin=cin, cout=cout, k=k, dilation=dil, stride=stride,
                  padding=pad, name=r.string())
        if kind != 2:
            n = cout * k * cin
            if wbits <= 8:
                op["w"] = list(r.get(f"{n}b")) if n > 1 else [r.get("b")]
            else:
                op["w"] = list(r.get(f"{n}h")) if n > 1 else [r.get("h")]
            op["bias"] = list(r.get(f"{cout}i")) if cout > 1 else [r.get("i")]
        op["mult"] = list(r.get(f"{cout}H")) if cout > 1 else [r.get("H")]
        op["shift"] = list(r.get(f"{cout}B")) if cout > 1 else [r.get("B")]
        ops.append(op)
    n = r.get("I")
    lut = list(r.get(f"{n}h"))
    return dict(groups=groups, buffers=buffers, input=inp, output=out, ops=ops, tanh=lut)


def load_golden(golden_dir, prog):
    """buffer index -> (frames, channels, flat int list)"""
    by_name = {b["name"]: i for i, b in enumerate(prog["buffers"])}
    out = {}
    for line in open(os.path.join(golden_dir, "manifest.txt")):
        f, _op, name, frames, ch, _bits = line.split()
        frames, ch = int(frames), int(ch)
        data = open(os.path.join(golden_dir, f), "rb").read()
        out[by_name[name]] = (frames, ch, list(struct.unpack(f"<{frames * ch}h", data)))
    return out


def round_shift(v, s):
    return (v + (1 << (s - 1))) >> s


def leaky(q, mul):
    return q if q >= 0 or mul == 32768 else round_shift(q * mul, 15)


def input_frames_needed(op, t0, count, tin):
    """Range of input frames the outputs [t0, t0+count) read, clipped to the sequence."""
    if op["kind"] == 1:  # transposed: t = ti*stride - pad + kk
        lo = (t0 + op["padding"] - (op["k"] - 1)) // op["stride"]
        hi = (t0 + count - 1 + op["padding"]) // op["stride"]
    else:
        lo = t0 - op["padding"]
        hi = t0 + count - 1 - op["padding"] + (op["k"] - 1) * op["dilation"]
    return max(lo, 0), min(hi, tin - 1)


def h(v, bits):
    return format(v & ((1 << bits) - 1), "0%dx" % ((bits + 3) // 4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("program")
    ap.add_argument("golden")
    ap.add_argument("op", type=int)
    ap.add_argument("out")
    ap.add_argument("--t0", type=int, default=0)
    ap.add_argument("--count", type=int, default=160)
    a = ap.parse_args()

    prog = load_program(a.program)
    op = prog["ops"][a.op]
    if op["kind"] not in (0, 1, 3):
        sys.exit(f"op {a.op} ({op['name']}) is not a convolution")
    gold = load_golden(a.golden, prog)
    tin, cin, xin = gold[op["inp"]]
    tout, cout, yout = gold[op["out"]]
    assert cin == op["cin"] and cout == op["cout"]

    count = min(a.count, tout)
    t0 = tout - count if a.t0 < 0 else a.t0
    lo, hi = input_frames_needed(op, t0, count, tin)
    if (hi - lo + 1) * cin > LANES * BANK_DEPTH:
        sys.exit(f"tile of {hi - lo + 1} frames x {cin} channels doesn't fit the banks; lower --count")

    os.makedirs(a.out, exist_ok=True)
    stride_log2 = op["stride"].bit_length() - 1
    cin_log2 = cin.bit_length() - 1
    assert 1 << stride_log2 == op["stride"] and 1 << cin_log2 == cin, "stride and cin must be powers of 2"
    cfg = [op["kind"], cin_log2, cout, op["k"], op["dilation"], stride_log2, op["padding"], tin, lo, t0, count,
           1 if op["residual"] >= 0 else 0, len(range(lo, hi + 1)) * cin]
    with open(os.path.join(a.out, "cfg.hex"), "w") as f:
        f.writelines(h(v, 32) + "\n" for v in cfg)

    with open(os.path.join(a.out, "act.hex"), "w") as f:
        for fr in range(lo, hi + 1):
            l = fr - lo
            for ci in range(cin):
                v = leaky(xin[fr * cin + ci], op["lr_mul"])
                f.write(f"{h(l % LANES, 8)} {h((l // LANES) * cin + ci, 16)} {h(v, 16)}\n")

    with open(os.path.join(a.out, "weights.hex"), "w") as f:
        f.writelines(h(v, 16) + "\n" for v in op["w"])
    with open(os.path.join(a.out, "params.hex"), "w") as f:
        for co in range(cout):
            f.write(f"{h(op['bias'][co], 32)} {h(op['mult'][co], 16)} {h(op['shift'][co], 8)}\n")
    res = gold[op["residual"]][2] if op["residual"] >= 0 else None
    with open(os.path.join(a.out, "res.hex"), "w") as rf, open(os.path.join(a.out, "expect.hex"), "w") as ef:
        for t in range(t0, t0 + count):
            for co in range(cout):
                rf.write(h(res[t * cout + co] if res else 0, 16) + "\n")
                ef.write(h(yout[t * cout + co], 16) + "\n")
    with open(os.path.join(a.out, "tanh.hex"), "w") as f:
        f.writelines(h(v, 16) + "\n" for v in prog["tanh"])

    macs = count * cout * cin * (op["k"] // op["stride"] if op["kind"] == 1 else op["k"])
    with open(os.path.join(a.out, "info.txt"), "w") as f:
        f.write(f"{op['name']} outputs {t0}..{t0 + count - 1} of {tout}, input tile {lo}..{hi} of {tin}, "
                f"{macs} MACs\n")
    print(open(os.path.join(a.out, "info.txt")).read().strip())


if __name__ == "__main__":
    main()
