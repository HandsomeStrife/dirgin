#!/usr/bin/env python3
"""export_z128_room.py -- assemble the whole z128 (Kalm "sea-of-flames") room from
all 13 geometry chunks using a chosen per-packet FileCode binding, merged GLOBALLY
by FileCode (one DMSH per distinct runtime texture + one 'unbound' group), so the
result fits the GPU renderer's group limit and draws as one coherent room.

This is the z128 twin of export_room_bound.py (which targets z143 Mako Reactor).
The binding dir (--pf) holds pf_<chunk>.json: a per-packet list of FileCodes (or
null for an unbound packet). Sources, in order of trust:
  - out/z128_pktfc/        : seed, direct per-dump UV-triple match (phaseA/phaseB).
  - out/z128_pktfc_union/  : seed grown across all 8 GS dumps (build_union, 99.6%).
  - out/z128_pktfc_v3/     : union + spatial recovery of dump-unseen surfaces
                             (recover_z128_deck.py -- the walkway deck, etc.).

  python3 -I export_z128_room.py --pf out/z128_pktfc_v3 -o out/z128_room_v2/room

Output: <OUTPREFIX>_gNN.mesh, <OUTPREFIX>.list, <OUTPREFIX>_manifest.json.
Trusted repo data + bounds-checked raw BMD read (via export_bmd_textured); run -I.
"""
import os, sys, json, glob, argparse
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_bmd_textured import find_bmd_payload, decode_packets, EXTRACT
from export_bmd_bound import build_flat, emit_group, tex_png_for

# Packets dropped by name (2026-10-24 investigation, review 27): big flat quads at
# x=-829 with generic 0..1 UVs. They sit in front of the real x=-760 wall/roof, and
# no hero-dump (163235) vertex lies within 3 units of them though 70-79% are in view,
# so the game does not draw them in this shot. Explicit list on purpose: there is
# deliberately no general "no dump match" rule (hidden surfaces would also fail it).
EXCLUDE_PACKETS = {('06800a12', 29), ('06800a12', 31),
                   ('06800c12', 70), ('06800c12', 71)}

# Packet bindings set by name, overriding the pf json. 06800212 pkt 370 (2 tris)
# holds the light-grey facet (old room_g42 tri 7880): the hero dump draws that
# surface at the same depth (ratio 1.01) with TBP 12996, whose decoded texture is
# FileCode 0680ff85 (correlation 1.000). Binding is per packet, so its second,
# smaller triangle gets the same texture.
# 06800112 pkts 585/986/1027 (the only 0680ff06 packets; 585 is the strip at
# Vincent's feet, y=-98): the hero dump draws pkt 585's pixels with TBP 10792, whose
# texture is 0680ff07 (corr 1.000; 0680ff06 corr 0.06, mean 15 = near-black), and with
# the same vertex colours (RGB 12-18/12-18/18-26, A 0-66). 986/1027 share that exact
# colour/UV signature but are out of this view (dump-unverified). All three are ABE,
# so with 0680ff07 in ALPHA_TEST they blend as a faint wash instead of opaque black.
# 06800212 pkts 0/4 (1 tri each): the vertical outer edge of the walkway (z=-1117.6,
# y -98.4..-94.5), the "grey banner" along Vincent's railing (review 34). Unbound: the
# UV-triple seed missed them and recover_z128_deck.py only takes near-horizontal faces.
# The hero dump draws them with TBP 13228 = 0680ff87 (deck planks, corr 1.000): pkt 0's
# UV triple is identical to dump draw 3408's, pkt 4's two dump tris (3412/3413) share
# its UVs, and the vertex colours agree within 6 (e.g. 108,63,53 vs 113,69,59).
#
# Each entry: (FileCode to bind, expected pf binding, expected ABE flag). Indices are
# packet positions, so a decoder or pf change could point an entry at another packet.
# If the packet's pf binding or ABE flag no longer matches, the export warns and does
# NOT apply that override (the pf binding stays); re-check the evidence and update it.
BIND_OVERRIDE = {('06800212', 370): ('0680ff85', None, False),
                 ('06800112', 585): ('0680ff07', '0680ff06', True),
                 ('06800112', 986): ('0680ff07', '0680ff06', True),
                 ('06800112', 1027): ('0680ff07', '0680ff06', True),
                 ('06800212', 0): ('0680ff87', None, False),
                 ('06800212', 4): ('0680ff87', None, False)}

# GS alpha-tested passes, from the hero dump (163235, last frame, FBP 224), 2026-10-24.
# Each FileCode below is matched to its dump TBP by texture correlation 1.000. Its ABE
# draws use TEST ATE=1 ATST=GEQUAL AREF=96 AFAIL=RGB_ONLY with ALPHA (Cs-Cd)*As+Cd,
# drawn AFTER the sky dome; its non-ABE draws are opaque with ATE off. In the dump, the
# ATE draws are the triangles that cover the cut-out texels (alpha < 96) and the opaque
# draws cover none, and the BMD packet ABE flag splits them the same way. So ABE
# packets of these FileCodes go to a separate group, listed after the sky, with this
# threshold in .list column 4. Gap texels then write no depth and the sky shows through.
# Threshold = (AREF - 0.5)/128: the decoder stores alpha as a*255/128, and the shader
# computes As = texel * vertex alpha * 2, which is 1.0 at GS 128.
# Not listed, on purpose: 0680ff36 (16% cut-out texels, but the game draws it opaque with
# no test), the sky 0680ff35 (ATST NEVER + FB_ONLY, not a cut-out), and the GREATER-100
# ATE passes at TBP 11440/11476/11748/11880/12156, whose textures match nothing bound
# here (corr <= 0.56).
# 0680ff07 (TBP 10792): its 37 pf-bound packets are non-ABE and stay opaque; its ABE
# packets are the three BIND_OVERRIDE rebinds above (dump ATE draws: same TEST/ALPHA).
# Their vertex alpha <= 66 keeps As below the ref, so they blend and write no depth.
# Vertex alpha (As = At*Av/128): every ATE vertex of 0680ff31 and 0680ff32 is 128, and
# 153/156 of 0680ff28; 0680ff30 also has Av 76-78 (BMD and dump agree), which caps As
# below the ref, so those faces blend semi-transparent and write no depth, as in-game.
# Known gap: 0680ff30 and 0680ff28 also have a few ABE draws with ATE off (36 and 38
# tris); the BMD can't tell them apart, so they get the test too.
ALPHA_TEST = {
    '0680ff31': (96, 'TBP 11220: battlements; 53 ATE tris, 15% cut-out texels'),
    '0680ff30': (96, 'TBP 10960: 84 ATE tris, 55% cut-out texels'),
    '0680ff28': (96, 'TBP 10828: 52 ATE tris, 3% cut-out texels'),
    '0680ff32': (96, 'TBP 11288: 8 ATE tris, no cut-out texels'),
    '0680ff54': (96, 'TBP 11800: 16 ATE tris, no cut-out texels'),
    '0680ff07': (96, 'TBP 10792: deck wash at Vincent\'s feet, Av 0-66 (blends, no depth)'),
}
ATEST_SUFFIX = '@atest'

SKY_FC = '0680ff35'   # TBP 11504 sky-dome texture (correlation 1.000), see export_z128_sky.py


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pf', required=True, help='dir of per-packet FileCode pf_<chunk>.json')
    ap.add_argument('-o', '--out', required=True, help='output prefix, e.g. out/z128_room_v2/room')
    ap.add_argument('--sky', help='sky mesh from export_z128_sky.py (basename, next to the '
                    '.list); appended as the last group, textured with ' + SKY_FC)
    a = ap.parse_args()

    cids = [os.path.basename(p)[3:11]
            for p in sorted(glob.glob(os.path.join(a.pf, 'pf_*.json')))]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)

    flat_by = defaultdict(list); ntri_by = defaultdict(int)
    abe_by = defaultdict(bool);  npk_by = defaultdict(int)
    overrides_applied = set()
    for cid in cids:
        binp = os.path.join(EXTRACT, cid + '.bin')
        if not os.path.exists(binp):
            continue
        buf = open(binp, 'rb').read(); pay = find_bmd_payload(buf)
        if pay is None:
            continue
        packets = decode_packets(buf, pay)
        pf = json.load(open(os.path.join(a.pf, 'pf_%s.json' % cid)))
        pf = [f if f else None for f in pf]
        n = min(len(pf), len(packets))
        for (ecid, pi) in EXCLUDE_PACKETS | set(BIND_OVERRIDE):
            if ecid == cid and pi >= n:
                sys.exit('named packet %s/%d out of range (%d packets)' % (cid, pi, n))
        groups = defaultdict(list)
        for i in range(n):
            if (cid, i) in EXCLUDE_PACKETS:
                continue
            fc = pf[i]
            if (cid, i) in BIND_OVERRIDE:
                to, want_pf, want_abe = BIND_OVERRIDE[(cid, i)]
                if pf[i] == want_pf and bool(packets[i]['abe']) == want_abe:
                    fc = to; overrides_applied.add((cid, i))
                else:
                    print('warning: BIND_OVERRIDE %s/%d expects pf %s abe %s, found pf %s abe %s; '
                          'NOT applied (kept pf binding)' % (cid, i, want_pf, want_abe, pf[i],
                          bool(packets[i]['abe'])), file=sys.stderr)
            if fc in ALPHA_TEST and packets[i]['abe']:
                fc += ATEST_SUFFIX
            groups[fc if fc else 'unbound'].append(i)
        for key, idxs in groups.items():
            flat, ntri = build_flat(packets, idxs)
            if ntri == 0:
                continue
            flat_by[key].extend(flat); ntri_by[key] += ntri; npk_by[key] += len(idxs)
            if any(packets[i]['abe'] for i in idxs):
                abe_by[key] = True

    order = sorted([k for k in flat_by if k != 'unbound' and not k.endswith(ATEST_SUFFIX)],
                   key=lambda k: -ntri_by[k])
    if 'unbound' in flat_by:
        order.append('unbound')
    atest = sorted([k for k in flat_by if k.endswith(ATEST_SUFFIX)], key=lambda k: -ntri_by[k])
    for fc in sorted(ALPHA_TEST):
        if fc + ATEST_SUFFIX not in flat_by:
            print('warning: ALPHA_TEST %s matched no blended (ABE) packet; entry does nothing '
                  '(%s)' % (fc, ALPHA_TEST[fc][1]), file=sys.stderr)

    manifest = {'method': 'z128 whole-room union; per-packet FileCode from 8-dump '
                'UV-triple oracle (seed+grow, 99.6%) + spatial recovery of '
                'dump-unseen surfaces (recover_z128_deck.py)',
                'pf_dir': a.pf, 'chunks': cids, 'groups': []}
    total = bound = 0
    def add_group(gi, key):
        nonlocal total, bound
        fc = key[:-len(ATEST_SUFFIX)] if key.endswith(ATEST_SUFFIX) else key
        mesh_name, bbmin, bbmax = emit_group(flat_by[key], ntri_by[key], a.out, gi)
        cont = png = None
        if fc != 'unbound':
            png, cont = tex_png_for(fc); bound += ntri_by[key]
        g = {'mesh': mesh_name, 'filecode': (None if fc == 'unbound' else fc),
             'packets': npk_by[key], 'tris': ntri_by[key], 'texture': png,
             'texture_container': cont, 'abe': abe_by[key]}
        if key.endswith(ATEST_SUFFIX):
            aref, why = ALPHA_TEST[fc]
            g['alpha_ref'] = round((aref - 0.5) / 128, 5); g['alpha_test_reason'] = why
        manifest['groups'].append(g)
        total += ntri_by[key]
    for gi, key in enumerate(order):
        add_group(gi, key)
    manifest['excluded_packets'] = sorted('%s/%d' % k for k in EXCLUDE_PACKETS)
    manifest['bind_override'] = {'%s/%d' % k: {'filecode': v[0], 'expected_pf': v[1],
                                               'expected_abe': v[2], 'applied': k in overrides_applied}
                                 for k, v in BIND_OVERRIDE.items()}
    if a.sky:
        skyp = os.path.join(os.path.dirname(os.path.abspath(a.out)), a.sky)
        if not os.path.exists(skyp):
            sys.exit('sky mesh not found: %s (run export_z128_sky.py first)' % skyp)
        manifest['groups'].append({'mesh': a.sky, 'filecode': SKY_FC, 'sky': True,
            'texture_container': os.path.join(EXTRACT, SKY_FC + '.bin'), 'abe': True})
    # alpha-tested passes after the sky, as in the game's draw order
    for k, key in enumerate(atest):
        add_group(len(order) + k, key)
    json.dump(manifest, open(a.out + '_manifest.json', 'w'), indent=2)
    with open(a.out + '.list', 'w') as f:
        for g in manifest['groups']:
            f.write('%s\t%s\t%d\t%g\n' % (g['mesh'], g['texture_container'] or 'NONE',
                                          1 if g['abe'] else 0, g.get('alpha_ref', -1)))
    nb = len({g['filecode'] for g in manifest['groups'] if g['filecode'] and not g.get('sky')})
    print('z128 room: %d chunks, %d textures (+%s unbound), %d/%d tris textured (%.2f%%) -> %s.list'
          % (len(cids), nb, '1' if 'unbound' in flat_by else '0',
             bound, total, 100 * bound / total, a.out))


if __name__ == '__main__':
    main()
