"""Analyses a <prefix>_capture_<time> folder (capture::Request, F11 by default).

  python tools/analyze_capture.py <capture folder> [--png]

Prints, per frame and over the capture:
  * how far DLSS's output is from its input, and from the average of the inputs
    (a DLSS that keeps its history lands near the average on a static view);
  * temporal flicker: mean |frame - previous frame| of the input, output and final image;
  * edge energy (mean gradient), which antialiasing lowers on static edges;
  * motion-vector and depth statistics.
With --png it writes previews next to the capture: tone-mapped in/out, per-pixel
temporal deviation (where it flickers) of in and out, and the motion vectors.
"""
import argparse
import glob
import os
import re
import struct
import sys

import numpy as np

FMT_RGBA16F, FMT_RG16F, FMT_R32F = 10, 34, 41
FMT_RGB10A2, FMT_RGBA8, FMT_RGBA8_SRGB, FMT_R11G11B10F, FMT_BGRA8, FMT_R8 = 24, 28, 29, 26, 87, 61


def small_float(bits, mant_bits):
    e = (bits >> mant_bits) & 31
    m = (bits & ((1 << mant_bits) - 1)).astype(np.float32)
    v = np.where(e == 0, m / (1 << mant_bits) * 2.0 ** -14,
                 (1 + m / (1 << mant_bits)) * np.exp2(e.astype(np.float32) - 15))
    return np.where(e == 31, np.nan, v).astype(np.float32)


def load_dds(path):
    with open(path, 'rb') as f:
        data = f.read()
    h, w = struct.unpack_from('<II', data, 12)
    fmt = struct.unpack_from('<I', data, 128)[0]
    raw = data[148:]
    if fmt == FMT_R11G11B10F:
        b = np.frombuffer(raw, np.uint32).reshape(h, w)
        return np.stack([small_float(b & 0x7FF, 6), small_float((b >> 11) & 0x7FF, 6),
                         small_float(b >> 22, 5)], -1)
    if fmt == FMT_RGBA16F:
        return np.frombuffer(raw, np.float16).reshape(h, w, 4).astype(np.float32)
    if fmt == FMT_RG16F:
        return np.frombuffer(raw, np.float16).reshape(h, w, 2).astype(np.float32)
    if fmt == FMT_R32F:
        return np.frombuffer(raw, np.float32).reshape(h, w)
    if fmt in (FMT_RGBA8, FMT_RGBA8_SRGB):
        return np.frombuffer(raw, np.uint8).reshape(h, w, 4)[..., :3].astype(np.float32) / 255
    if fmt == FMT_BGRA8:
        return np.frombuffer(raw, np.uint8).reshape(h, w, 4)[..., 2::-1].astype(np.float32) / 255
    if fmt == FMT_RGB10A2:
        b = np.frombuffer(raw, np.uint32).reshape(h, w)
        return np.stack([(b >> s) & 1023 for s in (0, 10, 20)], -1).astype(np.float32) / 1023
    raise ValueError(f'{path}: format {fmt} not handled')


def luma(img):
    img = np.nan_to_num(img[..., :3])
    return img @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def perceptual(img):
    """Reinhard on luma, so bright highlights don't dominate the averages."""
    y = luma(img)
    return y / (1 + y)


def edge_energy(y):
    return float(np.mean(np.abs(np.diff(y, axis=0))[:, :-1] + np.abs(np.diff(y, axis=1))[:-1]))


def write_png(path, img):
    from PIL import Image
    Image.fromarray(np.clip(img * 255 + 0.5, 0, 255).astype(np.uint8)).save(path)


def tonemap(img):
    img = np.nan_to_num(img[..., :3])
    return (img / (1 + img)) ** (1 / 2.2)


def bilinear(img, mv):
    """img sampled at (x + mv.x, y + mv.y), clamped at the borders."""
    h, w = img.shape
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    sx = np.clip(x + 0.5 + mv[..., 0] - 0.5, 0, w - 1.001)
    sy = np.clip(y + 0.5 + mv[..., 1] - 0.5, 0, h - 1.001)
    x0, y0 = sx.astype(int), sy.astype(int)
    fx, fy = sx - x0, sy - y0
    top = img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx
    bottom = img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx
    return top * (1 - fy) + bottom * fy


def heat(v, scale):
    t = np.clip(v / scale, 0, 1)
    return np.stack([t, t ** 2, np.zeros_like(t)], -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--png', action='store_true')
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.folder, 'f*_*.dds')))
    frames = {}
    for p in files:
        m = re.match(r'f(\d+)_(\w+)\.dds', os.path.basename(p))
        frames.setdefault(int(m.group(1)), {})[m.group(2)] = p
    if not frames:
        sys.exit('no frames in ' + args.folder)
    info = os.path.join(args.folder, 'info.txt')
    if os.path.exists(info):
        print('--- info.txt (dlaa / jitter / camera lines) ---')
        for line in open(info):
            if re.match(r'f\d+ (dlaa|jitter|camera)', line):
                print(line.rstrip())
    idx = sorted(frames)
    kinds = sorted({k for f in frames.values() for k in f})
    print(f'\n{len(idx)} frames, images: {", ".join(kinds)}')

    seq = {k: [perceptual(load_dds(frames[i][k])) for i in idx if k in frames[i]]
           for k in ('in', 'out', 'final') if k in kinds}

    print('\nper frame (perceptual luma, 0..1):')
    print('  f   edge_in edge_out |out-in| |out-mean_in|  mv_mean   mv_p99   mv_max  depth_min..max')
    mean_in = np.mean(seq['in'], 0) if 'in' in seq else None
    for n, i in enumerate(idx):
        fr = frames[i]
        cols = [f'{i:3d}']
        yi = perceptual(load_dds(fr['in'])) if 'in' in fr else None
        yo = perceptual(load_dds(fr['out'])) if 'out' in fr else None
        cols.append(f'{edge_energy(yi):9.5f}' if yi is not None else ' ' * 9)
        cols.append(f'{edge_energy(yo):8.5f}' if yo is not None else ' ' * 8)
        cols.append(f'{np.mean(np.abs(yo - yi)):8.5f}' if yo is not None else ' ' * 8)
        cols.append(f'{np.mean(np.abs(yo - mean_in)):12.5f}' if yo is not None else ' ' * 12)
        if 'mv' in fr:
            mv = load_dds(fr['mv'])
            mag = np.hypot(mv[..., 0], mv[..., 1])
            cols.append(f'{mag.mean():8.4f} {np.percentile(mag, 99):8.4f} {mag.max():8.3f}')
        if 'depth' in fr:
            d = load_dds(fr['depth'])
            cols.append(f'  {d.min():.5f}..{d.max():.5f}')
        print(' '.join(cols))

    print('\ntemporal flicker, mean |frame - previous| (perceptual luma):')
    for k, ys in seq.items():
        if len(ys) < 2:
            continue
        d = [float(np.mean(np.abs(ys[n] - ys[n - 1]))) for n in range(1, len(ys))]
        std = np.std(ys, 0)
        print(f'  {k:5s} mean {np.mean(d):.5f}  per frame ' + ' '.join(f'{x:.5f}' for x in d))
        print(f'        per-pixel temporal std: mean {std.mean():.5f}, p99 {np.percentile(std, 99):.4f}, '
              f'pixels > 0.02: {np.mean(std > 0.02) * 100:.2f}%')

    # Motion-vector check: the previous frame's input moved by this frame's
    # vectors (prev = cur + mv, in pixels) should match this frame's input much
    # better than the previous frame left in place. Pixels where it still
    # doesn't match are where DLSS's history is wrong (ghosting / smearing).
    print('\nmotion vectors vs the image (|in - reprojected previous in|, perceptual luma):')
    print('  f   no_motion  with_mv   moving_px  bad_px(>0.05)  |out-in| on moving px')
    worst = None
    for a, b in zip(idx, idx[1:]):
        fa, fb = frames[a], frames[b]
        if b != a + 1 or not all(k in fa for k in ('in',)) or not all(k in fb for k in ('in', 'mv')):
            continue
        prev, cur = perceptual(load_dds(fa['in'])), perceptual(load_dds(fb['in']))
        mv = load_dds(fb['mv'])
        warped = bilinear(prev, mv)
        err0, err1 = np.abs(cur - prev), np.abs(cur - warped)
        moving = np.hypot(mv[..., 0], mv[..., 1]) > 0.5
        bad = err1 > 0.05
        lag = ''
        if 'out' in fb and moving.any():
            lag = f'{np.mean(np.abs(perceptual(load_dds(fb["out"])) - cur)[moving]):.5f}'
        print(f'{b:3d}   {err0.mean():9.5f} {err1.mean():9.5f}  {moving.mean() * 100:8.2f}%  '
              f'{bad.mean() * 100:11.2f}%   {lag}')
        worst = (b, err1, err0)
    if worst and args.png:
        b, err1, err0 = worst
        write_png(os.path.join(args.folder, f'p_reproj_err_f{b:02d}.png'), heat(err1, 0.1))
        write_png(os.path.join(args.folder, f'p_noreproj_err_f{b:02d}.png'), heat(err0, 0.1))

    if args.png:
        out_dir = args.folder
        first = idx[0]
        for k in ('in', 'out', 'final'):
            if k in frames[first]:
                img = load_dds(frames[first][k])
                write_png(os.path.join(out_dir, f'p_{k}.png'),
                          img if k == 'final' else tonemap(img))
        for k, ys in seq.items():
            if len(ys) >= 2:
                write_png(os.path.join(out_dir, f'p_flicker_{k}.png'), heat(np.std(ys, 0), 0.05))
        if 'mv' in frames[first]:
            mv = load_dds(frames[first]['mv'])
            write_png(os.path.join(out_dir, 'p_mv.png'),
                      np.stack([np.abs(mv[..., 0]), np.abs(mv[..., 1]), np.zeros(mv.shape[:2])], -1) * 0.25)
        print('\nPNG previews written to', out_dir)


if __name__ == '__main__':
    main()
