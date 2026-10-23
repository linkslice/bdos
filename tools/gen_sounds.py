#!/usr/bin/env python3
"""Make the sounds bdos ships with: a start-up chime and a test tone.

Usage: gen_sounds.py <outdir>
"""
import math, os, struct, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def wav(name, rate, samples):
    data = b"".join(struct.pack("<h", max(-32767, min(32767, int(s)))) for s in samples)
    with open(os.path.join(out, name), "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


rate = 22050
# CHIMES: a bright major arpeggio with a soft bell-ish decay
notes = [523.25, 659.25, 783.99, 1046.5]
n = int(rate * 1.6)
s = [0.0] * n
for i, f in enumerate(notes):
    start = int(i * 0.12 * rate)
    for t in range(n - start):
        x = t / rate
        env = math.exp(-x * 3.0) * min(1, t / 60)
        v = math.sin(2 * math.pi * f * x) + 0.3 * math.sin(2 * math.pi * f * 2.01 * x) * math.exp(-x * 6)
        s[start + t] += 6000 * env * v
wav("CHIMES.WAV", rate, s)

# TONE: one second of 440 Hz, for checking a sound card works
wav("TONE.WAV", rate, [12000 * math.sin(2 * math.pi * 440 * t / rate) for t in range(rate)])
