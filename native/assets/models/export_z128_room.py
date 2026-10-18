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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pf', required=True, help='dir of per-packet FileCode pf_<chunk>.json')
    ap.add_argument('-o', '--out', required=True, help='output prefix, e.g. out/z128_room_v2/room')
    a = ap.parse_args()

    cids = [os.path.basename(p)[3:11]
            for p in sorted(glob.glob(os.path.join(a.pf, 'pf_*.json')))]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)

    flat_by = defaultdict(list); ntri_by = defaultdict(int)
    abe_by = defaultdict(bool);  npk_by = defaultdict(int)
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
        groups = defaultdict(list)
        for i in range(n):
            groups[pf[i] if pf[i] else 'unbound'].append(i)
        for key, idxs in groups.items():
            flat, ntri = build_flat(packets, idxs)
            if ntri == 0:
                continue
            flat_by[key].extend(flat); ntri_by[key] += ntri; npk_by[key] += len(idxs)
            if any(packets[i]['abe'] for i in idxs):
                abe_by[key] = True

    order = sorted([k for k in flat_by if k != 'unbound'], key=lambda k: -ntri_by[k])
    if 'unbound' in flat_by:
        order.append('unbound')

    manifest = {'method': 'z128 whole-room union; per-packet FileCode from 8-dump '
                'UV-triple oracle (seed+grow, 99.6%) + spatial recovery of '
                'dump-unseen surfaces (recover_z128_deck.py)',
                'pf_dir': a.pf, 'chunks': cids, 'groups': []}
    total = bound = 0
    for gi, fc in enumerate(order):
        mesh_name, bbmin, bbmax = emit_group(flat_by[fc], ntri_by[fc], a.out, gi)
        cont = png = None
        if fc != 'unbound':
            png, cont = tex_png_for(fc); bound += ntri_by[fc]
        manifest['groups'].append({'mesh': mesh_name, 'filecode': (None if fc == 'unbound' else fc),
            'packets': npk_by[fc], 'tris': ntri_by[fc], 'texture': png,
            'texture_container': cont, 'abe': abe_by[fc]})
        total += ntri_by[fc]
    json.dump(manifest, open(a.out + '_manifest.json', 'w'), indent=2)
    with open(a.out + '.list', 'w') as f:
        for g in manifest['groups']:
            f.write('%s\t%s\t%d\n' % (g['mesh'], g['texture_container'] or 'NONE',
                                      1 if g['abe'] else 0))
    nb = sum(1 for g in manifest['groups'] if g['filecode'])
    print('z128 room: %d chunks, %d textures (+%s unbound), %d/%d tris textured (%.2f%%) -> %s.list'
          % (len(cids), nb, '1' if 'unbound' in flat_by else '0',
             bound, total, 100 * bound / total, a.out))


if __name__ == '__main__':
    main()
