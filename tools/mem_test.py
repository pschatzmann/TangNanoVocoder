#!/usr/bin/env python3
"""Checks vocoder_act_banks' block RAMs on a Tang Nano 20K running
gateware/test/mem_test_top.v: writes through either port, reads all 16 banks
(the outputs must hold while the read enable is low).   mem_test.py PORT"""
import random, struct, sys
import serial

s = serial.Serial(sys.argv[1], 921600, timeout=1)
N = 64
ref = {}
for a in range(N):
    for b in range(16):
        v = random.randint(0, 0xFFFF)
        ref[(b, a)] = v
        if (a + b) % 3 == 0 and b < 15:  # some through both ports at once
            continue
        s.write(b"W" + bytes([b]) + struct.pack("<HH", a, v))
for a in range(N):
    for b in range(0, 15, 1):
        if (a + b) % 3 == 0:
            b2 = b + 1
            s.write(b"V" + bytes([b]) + struct.pack("<HH", a, ref[(b, a)]) + bytes([b2]) +
                    struct.pack("<HH", a, ref[(b2, a)]))
s.flush()
bad = 0
for a in range(N):
    s.write(b"R" + struct.pack("<H", a))
    r = s.read(32)
    if len(r) != 32:
        sys.exit(f"no reply for address {a}")
    got = struct.unpack("<16H", r)
    for b in range(16):
        if got[b] != ref[(b, a)]:
            bad += 1
            if bad <= 8: print(f"bank {b} addr {a}: wrote {ref[(b, a)]:04x}, read {got[b]:04x}")
print(f"{N * 16} words: {bad} wrong")
