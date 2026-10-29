#!/usr/bin/env python3
"""mktestiso.py <dir>: two small CD images for the tests, DISC1.ISO and DISC2.ISO

Each has a README.TXT saying which disc it is, and a folder with a long
(Joliet) file name in it, the way CD-era games ship.
"""
import io, os, sys
import pycdlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for n, word in ((1, "one"), (2, "two")):
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, joliet=3, vol_ident=f"BDOS_DISC{n}")
    readme = f"This is disc {word}.\r\n".encode()
    iso.add_fp(io.BytesIO(readme), len(readme), f"/README.TXT;1", joliet_path="/README.TXT")
    iso.add_directory("/GAME", joliet_path="/Game Data")
    body = (f"Disc {word}, a file with a long name.\r\n" * 2000).encode()      # several sectors
    iso.add_fp(io.BytesIO(body), len(body), "/GAME/LONGNAME.TXT;1",
               joliet_path="/Game Data/A Long File Name.txt")
    iso.write(os.path.join(out, f"DISC{n}.ISO"))
    iso.close()
