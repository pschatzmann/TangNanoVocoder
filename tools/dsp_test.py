#!/usr/bin/env python3
"""Checks the DSP multipliers on a Tang Nano 20K running gateware/test/dsp_test_top.v:
random signed operands, products compared with Python's.   dsp_test.py PORT [N]"""
import random, struct, sys
import serial

def to_bytes(v, bits, n):
    return (v & ((1 << bits) - 1)).to_bytes(n, "little")

def signed(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v

s = serial.Serial(sys.argv[1], 921600, timeout=1)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 200
bad18 = bad36 = 0
cases = [(1, 1, 1, 1), (-1, 1, -1, 1), (32767, 127, -(1 << 34), 32768), (-32767, -2047, (1 << 34) - 1, 12345)]
cases += [(random.randint(-(1 << 17), (1 << 17) - 1), random.randint(-(1 << 17), (1 << 17) - 1),
           random.randint(-(1 << 35), (1 << 35) - 1), random.randint(-(1 << 35), (1 << 35) - 1)) for _ in range(n)]
for i, (a, b, A, B) in enumerate(cases):
    s.write(b"M" + to_bytes(a, 18, 3) + to_bytes(b, 18, 3) + to_bytes(A, 36, 5) + to_bytes(B, 36, 5))
    r = s.read(14)
    if len(r) != 14:
        sys.exit(f"no reply (got {len(r)} bytes)")
    p18 = signed(int.from_bytes(r[:5], "little"), 36)
    p36 = signed(int.from_bytes(r[5:], "little"), 72)
    if p18 != a * b:
        bad18 += 1
        if bad18 <= 4: print(f"MULT18X18 {a} * {b} = {a*b}, got {p18}")
    if p36 != A * B:
        bad36 += 1
        if bad36 <= 4: print(f"MULT36X36 {A} * {B} = {A*B}, got {p36}")
print(f"{len(cases)} cases: MULT18X18 {bad18} wrong, MULT36X36 {bad36} wrong")
