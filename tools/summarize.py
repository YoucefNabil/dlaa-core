"""Summarize a <prefix>_frame_*.txt dump into passes (runs of draws with the same RT/DS/VP)."""
import re, sys
path = sys.argv[1]
lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
res = {}
for l in lines:
    m = re.match(r'^(T\d+)  (.*)$', l)
    if m: res[m.group(1)] = m.group(2)
def short(t):
    d = res.get(t.rstrip('m0123456789') if 'm' in t[1:] else t, '')
    d = d.replace('tex2d ', '').replace(' array=1', '').replace(' msaa=1', '')
    return f"{t}({d})" if d else t
cur = None; cur_n = 0; shaders = {}; start = 0; cbups = 0
def flush():
    global cur, shaders, cur_n
    if cur is None: return
    rt, ds, vp = cur
    rts = ','.join(short(x) for x in rt.strip('[]').split(',') if x and x != '-')
    print(f"#{start:04d} draws={cur_n:<5} RT=[{rts}] DS={short(ds) if ds!='-' else '-'} VP={vp}")
    for (vs, ps, srv, extra), n in list(shaders.items())[:12]:
        print(f"        x{n:<4} VS={vs} PS={ps} SRV={srv}{extra}")
    if len(shaders) > 12: print(f"        ... {len(shaders)-12} more shader combos")
    cur = None; shaders = {}; cur_n = 0
for l in lines:
    # Entries recorded inside a command list are indented with '| '.
    m = re.match(r'^#(\d+) x(\d+)\s+n=\d+\s+(?:\|\s+)*(\S+)\s+(.*)$', l)
    if not m: continue
    idx, rep, op, rest = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
    if op.startswith('Draw'):
        g = dict(re.findall(r'(\w+(?:\.\w+)?)=(\S+)', rest))
        key = (g.get('RT'), g.get('DS'), g.get('VP'))
        if key != cur:
            flush(); cur = key; start = idx
        extra = ''
        if 'blend' in g: extra += ' blend=' + g['blend']
        if 'inst' in g: extra += ' inst=' + g['inst']
        k = (g.get('VS'), g.get('PS'), g.get('PS.SRV'), extra)
        shaders[k] = shaders.get(k, 0) + rep; cur_n += rep
    elif op in ('Map/Unmap', 'UpdateSubresource', 'UpdateSubresource1'):
        cbups += 1
    else:
        flush()
        if op == 'Dispatch':
            print(f"#{idx:04d} Dispatch {rest[:200]}")
        elif op.startswith('Clear'):
            v = re.search(r'view=(\S+)', rest).group(1)
            print(f"#{idx:04d} {op} {short(v)}")
        else:
            g = dict(re.findall(r'(\w+)=(\S+)', rest))
            print(f"#{idx:04d} {op} dst={short(g.get('dst','-'))} src={short(g.get('src','-'))}")
flush()
print(f"\n# CB uploads: {cbups}")
