#!/usr/bin/env python3
"""probe_draworder.py -- is the GS dump's texture draw order usable for binding?

Tests whether textures are drawn in CONTIGUOUS runs (=> draw-order binding viable)
or interleaved (=> dead). Walks the dump's triangles in emission order, collapses to
runs of constant TEX0, and reports per-texture run counts (runs==1 is perfectly
contiguous).

RESULT (doc_gsdump_03, zone z128 Kalm, 2026-10-16): heavily interleaved -- dominant
texture tbp 0x2e50 = 10003 tris across 2202 runs. The oracle's room atlases are only
moderately contiguous (4-24 runs); the hugely fragmented textures are the character
(Deepground soldiers / Vincent) + effect draws. Room total ~6180 visible tris vs one
BMD chunk's 12647 => this dump is a culled, multi-chunk, character-laden view, so
per-submesh binding is NOT reliably recoverable from it. See notes/DECODER-STATUS.md
(5th pass) for the two real fix paths.

Run: python3 -I src/probe_draworder.py <dump.bin>
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gsparse import read_header
from gsrender import build


def main():
    buf = open(sys.argv[1], 'rb').read()
    gif = build(buf, read_header(buf))
    seq = []
    for a, b, c, ctx in gif.tris:
        seq.append((ctx['tex0'] & 0x3fff, (ctx['tex0'] >> 20) & 0x3f) if ctx['tme'] else (-1, -1))
    runs = []
    for key in seq:
        if runs and runs[-1][0] == key:
            runs[-1][1] += 1
        else:
            runs.append([key, 1])
    from collections import defaultdict
    tex_runs = defaultdict(int); tex_tris = defaultdict(int)
    for key, n in runs:
        if key[0] < 0:
            continue
        tex_runs[key] += 1; tex_tris[key] += n
    print("total tris=%d  distinct textures=%d  runs=%d"
          % (len(seq), len(tex_tris), len(runs)))
    print("\n%-14s %8s %8s   (runs=1 => perfectly contiguous)" % ("tbp0/psm", "tris", "runs"))
    for key in sorted(tex_tris, key=lambda k: -tex_tris[k]):
        print("tbp=0x%04x p%02d %8d %8d" % (key[0], key[1], tex_tris[key], tex_runs[key]))


if __name__ == '__main__':
    main()
