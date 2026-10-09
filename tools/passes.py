"""Summarise a <prefix>_frame_*.txt dump: full-resolution passes in order.

usage: python passes.py <frame.txt> [WxH] [--all]
Prints every draw/dispatch/copy that writes a WxH resource (default 1920x1080),
collapsing runs of the same shader pair into one line with a draw count.
"""
import re
import sys

path = sys.argv[1]
res = next((a for a in sys.argv[2:] if re.fullmatch(r"\d+x\d+", a)), "1920x1080")
lines = open(path, encoding="utf-8", errors="replace").read().splitlines()

fmt = {}
for l in lines:
    m = re.match(r"(T\d+)\s+tex2d (\d+x\d+) (\S+)", l)
    if m:
        fmt[m.group(1)] = (m.group(2), m.group(3))

def ids(s):
    return re.findall(r"T\d+", s or "")

def short(t):
    r, f = fmt.get(t, ("", ""))
    return f"{t}({f})" if r == res else t

def field(l, name):
    m = re.search(name + r"=\[([^\]]*)\]", l)
    return m.group(1) if m else ""

prev = None
count = 0
for l in lines:
    m = re.match(r"#(\d+) x(\d+)\s+n=\d+\s+\|?\s*(\w+)", l)
    if not m:
        continue
    idx, op = m.group(1), m.group(3)
    if op.startswith("Map") or op.startswith("Execute") or op.startswith("Update"):
        continue
    outs = ids(field(l, "RT")) + ids(field(l, "UAV"))
    ds = re.search(r"DS=(T\d+)", l)
    if ds:
        outs.append(ds.group(1))
    dst = re.search(r"(?:dst|view)=(T\d+)", l)
    if dst:
        outs.append(dst.group(1))
    if not any(fmt.get(t, ("",))[0] == res for t in outs):
        continue
    sh = " ".join(re.findall(r"(?:VS|PS|CS)=[0-9A-F]+", l))
    srv = field(l, "PS.SRV") or field(l, "CS.SRV")
    src = re.search(r"src=(T\d+)", l)
    key = (op, sh, tuple(outs))
    if key == prev:
        count += int(m.group(2))
        continue
    if prev is not None:
        print(f"   (x{count})" if count > 1 else "")
    prev, count = key, int(m.group(2))
    extra = f" src={short(src.group(1))}" if src else ""
    print(f"#{idx} {op:<20} {sh} -> {','.join(short(t) for t in outs)}"
          f" SRV=[{','.join(short(t) for t in ids(srv))}]{extra}", end="")
print(f"   (x{count})" if count > 1 else "")
