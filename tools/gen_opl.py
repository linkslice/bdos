#!/usr/bin/env python3
"""The OPL2 tables: the log-sine and exponent ROMs the real chip uses, and
envelope rates for 48 kHz output. Writes kernel/dos/opl_tables.h."""
import math, sys

out = sys.argv[1] if len(sys.argv) > 1 else "kernel/dos/opl_tables.h"
logsin = [round(-math.log2(math.sin((i + 0.5) * math.pi / 512)) * 256) for i in range(256)]
exp = [round((2 ** (i / 256) - 1) * 1024) for i in range(256)]
RATE = 48000
dec, atk = [], []
for eff in range(64):
    if eff < 4:
        dec.append(0)
        atk.append(0)
        continue
    t_dec = 39.28064 * 2 ** (-(eff - 4) / 4)          # seconds for 96 dB
    dec.append(min(0x7FFFFFFF, round(512 * 65536 / (t_dec * RATE))))
    if eff >= 60:
        atk.append(65536)                                # instant
    else:
        t_atk = 2.82624 * 2 ** (-(eff - 4) / 4)
        atk.append(max(1, round(65536 * (1 - math.exp(-6.2 / (t_atk * RATE))))))


def table(name, ctype, vals):
    s = f"static const {ctype} {name}[{len(vals)}] = {{\n"
    for i in range(0, len(vals), 12):
        s += "\t" + ", ".join(str(v) for v in vals[i:i + 12]) + ",\n"
    return s + "};\n"


with open(out, "w") as f:
    f.write("/* opl_tables.h - made by tools/gen_opl.py: don't edit */\n")
    f.write(table("opl_logsin", "uint16_t", logsin))
    f.write(table("opl_exp", "uint16_t", exp))
    f.write(table("opl_dec", "uint32_t", dec))
    f.write(table("opl_atk", "uint32_t", atk))
