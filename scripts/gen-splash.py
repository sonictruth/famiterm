#!/usr/bin/env python3
"""Generate the splash CHR tiles + nametable: gen-splash.py IMAGE.png OUT.chr OUT_pic.h

IMAGE is a 256x240 PNG (8-bit RGB or RGBA, non-interlaced) or 24-bit BMP.  Colours are mapped to the NES palette, each 16x16 block
is assigned one of four 4-colour background palettes (shared backdrop colour 0), 8x8
tiles are deduplicated (max 256, one 4 KB pattern table) and written as 2bpp CHR.
"""
import struct
import sys
import zlib

# 2C02 palette (RGB), index = NES colour number
NES = [
 0x626262,0x001FB2,0x2404C8,0x5200B2,0x730076,0x800024,0x730B00,0x522800,0x244400,0x005700,0x005C00,0x005324,0x003C76,0x000000,0x000000,0x000000,
 0xABABAB,0x0D57FF,0x4B30FF,0x8A13FF,0xBC08D6,0xD21269,0xC72E00,0x9D5400,0x607B00,0x209800,0x00A300,0x009942,0x007DB4,0x000000,0x000000,0x000000,
 0xFFFFFF,0x53AEFF,0x9085FF,0xD365FF,0xFF57FF,0xFF5DCF,0xFF7757,0xFA9E00,0xBDC700,0x7AE700,0x43F611,0x26EF7E,0x2CD5F6,0x4E4E4E,0x000000,0x000000,
 0xFFFFFF,0xB6E1FF,0xCED1FF,0xE9C3FF,0xFFBCFF,0xFFBDF4,0xFFC6C3,0xFFD59A,0xE9E681,0xCEF481,0xB6FB9A,0xA9FAC3,0xA9F0F4,0xB8B8B8,0x000000,0x000000,
]
BACKDROP = 0x0F

def read_png(path):
    """Read a non-interlaced 8-bit RGB/RGBA PNG into rows of (r, g, b); alpha is ignored."""
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat = 8, []
    while pos < len(d):
        n, = struct.unpack_from('>I', d, pos)
        kind, body = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat.append(body)
        pos += 12 + n
    assert w == 256 and h == 240 and bd == 8 and ct in (2, 6) and il == 0, (w, h, bd, ct, il)
    bpp = 4 if ct == 6 else 3
    stride = w * bpp
    raw = zlib.decompress(b''.join(idat))
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out += line
        prev = line
    return [[(out[y * stride + x * bpp], out[y * stride + x * bpp + 1], out[y * stride + x * bpp + 2])
             for x in range(w)] for y in range(h)]

def read_bmp(path):
    """Read an uncompressed 1/4/8-bit paletted or 24-bit BMP into rows of (r, g, b)."""
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    hdr, w, h, _, bpp, comp, _, _, _, ncol = struct.unpack_from('<IiiHHIIiiI', d, 14)
    assert w == 256 and abs(h) == 240 and comp == 0 and bpp in (1, 4, 8, 24), (w, h, bpp, comp)
    pal = []
    if bpp <= 8:
        n = ncol or 1 << bpp
        pal = [(d[p + 2], d[p + 1], d[p]) for p in (14 + hdr + 4 * i for i in range(n))]
    stride = (w * bpp + 31) // 32 * 4
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        r = d[off + sy * stride: off + (sy + 1) * stride]
        if bpp == 24:
            rows.append([(r[x*3+2], r[x*3+1], r[x*3]) for x in range(w)])
        else:
            m = (1 << bpp) - 1
            rows.append([pal[r[x * bpp // 8] >> (8 - bpp - x * bpp % 8) & m] for x in range(w)])
    return rows

CAND = [i for i in range(64) if i % 16 < 13 or i == BACKDROP]  # drop the duplicate blacks
RGB = {i: ((NES[i] >> 16) & 255, (NES[i] >> 8) & 255, NES[i] & 255) for i in CAND}

def dist(a, b):
    return (a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2

def nearest(c, pal):
    return min(pal, key=lambda i: dist(c, RGB[i]))

def block_error(hist, pal):
    return sum(n * min(dist(c, RGB[i]) for i in pal) for c, n in hist.items())

def fit_palette(hist, seed):
    """Lloyd iterations on the free colours; the backdrop stays a fixed extra centre."""
    pal = list(seed)
    for _ in range(8):
        groups = [[] for _ in pal]
        for c, n in hist.items():
            k = min(range(len(pal) + 1), key=lambda j: dist(c, RGB[pal[j] if j < len(pal) else BACKDROP]))
            if k < len(pal):
                groups[k].append((c, n))
        new = []
        for k, g in enumerate(groups):
            if not g:
                new.append(pal[k]); continue
            t = sum(n for _, n in g)
            m = tuple(sum(c[j] * n for c, n in g) / t for j in range(3))
            new.append(min(CAND, key=lambda i: dist(m, RGB[i])))
        if new == pal:
            break
        pal = new
    return [BACKDROP] + pal

def fit_palettes(hists):
    """k-means over blocks: 4 palettes of (backdrop + 3 colours) minimising colour error."""
    def seedpal(i):
        return [BACKDROP] + [CAND[(i * 7 + j * 19) % len(CAND)] for j in range(3)]
    # seed: bright, distinct hue picks from the most colourful blocks
    allh = {}
    for h in hists:
        for c, n in h.items():
            allh[c] = allh.get(c, 0) + n
    cols = sorted((c for c in allh if max(c) - min(c) > 40 or max(c) > 160), key=lambda c: -allh[c])
    seeds = []
    for c in cols:
        i = nearest(c, CAND)
        if i not in seeds and all(dist(RGB[i], RGB[j]) > 3000 for j in seeds):
            seeds.append(i)
        if len(seeds) == 12:
            break
    seeds += [CAND[k] for k in range(12 - len(seeds))]
    pals = [seeds[0:3], seeds[3:6], seeds[6:9], seeds[9:12]]
    pals = [[BACKDROP] + p for p in pals]
    for _ in range(12):
        owner = [min(range(4), key=lambda k: block_error(h, pals[k])) for h in hists]
        new = []
        for k in range(4):
            mine = {}
            for h, o in zip(hists, owner):
                if o == k:
                    for c, n in h.items():
                        mine[c] = mine.get(c, 0) + n
            new.append(fit_palette(mine, pals[k][1:]) if mine else pals[k])
        if new == pals:
            break
        pals = new
    return pals, owner

def reduce_colours(px):
    """Quantise the whole image to backdrop + 12 NES colours (the most 4 palettes can hold)."""
    hist = {}
    for row in px:
        for c in row:
            hist[c] = hist.get(c, 0) + 1
    seeds = []
    for c in sorted((c for c in hist if max(c) > 40), key=lambda c: -hist[c]):
        i = nearest(c, CAND)
        if i not in seeds and all(dist(RGB[i], RGB[j]) > 3000 for j in seeds):
            seeds.append(i)
        if len(seeds) == 12:
            break
    seeds += [i for i in CAND if i not in seeds][:12 - len(seeds)]
    pal = fit_palette(hist, seeds)
    return [[RGB[nearest(c, pal)] for c in row] for row in px]

def main(src, out_chr, out_h):
    reader = read_png if src.lower().endswith('.png') else read_bmp
    px = reduce_colours(reader(src))
    keys = [(bx, by) for by in range(15) for bx in range(16)]
    hists = []
    for bx, by in keys:
        h = {}
        for y in range(16):
            for c in px[by*16+y][bx*16:bx*16+16]:
                h[c] = h.get(c, 0) + 1
        hists.append(h)
    full, owner = fit_palettes(hists)
    pals = full
    blockpal = dict(zip(keys, owner))

    attrs = [0] * 64
    for (bx, by), n in blockpal.items():
        attrs[(by // 2) * 8 + bx // 2] |= n << (((by & 1) * 2 + (bx & 1)) * 2)

    # 2bpp pixel indices per tile, then dedupe; over 256 -> merge the rarest into the nearest
    pats = []
    for ty in range(30):
        for tx in range(32):
            pal = full[blockpal[tx // 2, ty // 2]]
            pats.append(tuple(min(range(4), key=lambda j: dist(px[ty*8+y][tx*8+x], RGB[pal[j]]))
                              for y in range(8) for x in range(8)))
    count = {}
    for t in pats:
        count[t] = count.get(t, 0) + 1
    unique = len(count)
    remap = {}
    bits = {t: (sum((v & 1) << i for i, v in enumerate(t)), sum((v >> 1) << i for i, v in enumerate(t)))
            for t in count}
    def ham(a, b):
        return ((bits[a][0] ^ bits[b][0]) | (bits[a][1] ^ bits[b][1])).bit_count()
    def best(r):
        return min((ham(r, n), n) for n in count if n != r)
    near = {t: best(t) for t in count} if len(count) > 256 else {}
    while len(count) > 256:  # cheapest merge: fewest tiles x fewest differing pixels
        rare = min(count, key=lambda t: count[t] * near[t][0])
        into = near[rare][1]
        count[into] += count.pop(rare)
        del near[rare]
        remap[rare] = into
        for t in count:
            if near[t][1] == rare or t == into:
                near[t] = best(t)
    def final(t):
        while t in remap:
            t = remap[t]
        return t
    order = {}
    for t in pats:
        order.setdefault(final(t), len(order))
    names = [order[final(t)] for t in pats]
    tiles = []
    for t in order:
        lo = [sum((t[y*8+x] & 1) << (7 - x) for x in range(8)) for y in range(8)]
        hi = [sum((t[y*8+x] >> 1) << (7 - x) for x in range(8)) for y in range(8)]
        tiles.append(bytes(lo + hi))

    chr_ = b''.join(tiles).ljust(4096, b'\0')
    open(out_chr, 'wb').write(chr_)

    def arr(name, data):
        lines = [','.join('0x%02X' % v for v in data[i:i+16]) + ',' for i in range(0, len(data), 16)]
        return 'static const unsigned char %s[%d] = {\n  %s\n};\n' % (name, len(data), '\n  '.join(lines))
    with open(out_h, 'w') as f:
        f.write('/* Generated by scripts/gen-splash.py -- do not edit. */\n#ifndef SPLASH_PIC_H\n#define SPLASH_PIC_H\n\n')
        f.write(arr('splash_pic_tiles', names) + '\n' + arr('splash_pic_attrs', attrs) + '\n')
        f.write(arr('splash_pic_pal', [c for p in full for c in p]) + '\n#endif\n')
    print('splash: %d unique tiles -> %d, palettes %s' % (unique, len(tiles), ['%02X,%02X,%02X' % tuple(p[1:]) for p in pals]))

if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
