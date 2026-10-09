"""Print constant-buffer uploads from a <prefix>_frame_*.cb blob.

usage: python cbdump.py <frame.cb> <resid> [which=last|all|N]
Records: uint32 rec, uint32 resid, uint32 offset, uint32 bytes, then bytes of data.
Prints the data as float4 rows (index: x y z w).
"""
import struct
import sys

path, resid = sys.argv[1], int(sys.argv[2].lstrip("T"))
which = sys.argv[3] if len(sys.argv) > 3 else "last"
d = open(path, "rb").read()
recs = []
p = 0
while p + 16 <= len(d):
    rec, rid, off, n = struct.unpack_from("<4I", d, p)
    p += 16
    if rid == resid:
        recs.append((rec, off, d[p:p + n]))
    p += n

print(f"T{resid}: {len(recs)} uploads")
sel = recs if which == "all" else [recs[-1]] if which == "last" else [recs[int(which)]]
for rec, off, data in sel:
    print(f"-- cb#{rec} offset={off} bytes={len(data)}")
    for i in range(len(data) // 16):
        v = struct.unpack_from("<4f", data, i * 16)
        print(f"  [{i:2}] " + " ".join(f"{x:12.6g}" for x in v))
