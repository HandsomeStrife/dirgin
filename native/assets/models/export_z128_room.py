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
from bmd_env import read_env, fog_directive
from bmd_chains import packet_chains

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
                 ('06800212', 4): ('0680ff87', None, False),
                 # Chain audit (review 43): within-chain disagreements checked against the hero
                 # dump. Evidence = dump tris with the same UV triple, vertex colour within 12 AND
                 # screen position within 4 px of our hero-camera projection; dump TBP -> FileCode
                 # by texture correlation (all 138 0680ff* textures, not only bound ones).
                 # 06800012 pkt 179 (16 tris, battlement ABE): 2 tris drawn with TBP 11288 =
                 # 0680ff32 (corr 1.000); its chain-mates 178/180 are 0680ff32 (dump agrees).
                 ('06800012', 179): ('0680ff32', '0680ff31', True),
                 # 06800112 pkts 1095/1096/1107/1108: drawn with TBP 10756 = 0680ff05 (corr 1.000;
                 # bound nowhere before) on 8/12/6/4 tris; none on their pf textures.
                 ('06800112', 1095): ('0680ff05', '0680ff32', False),
                 ('06800112', 1096): ('0680ff05', '0680ff24', False),
                 ('06800112', 1107): ('0680ff05', '0680ff87', False),
                 ('06800112', 1108): ('0680ff05', '0680ff33', False),
                 # Chain anchors: the only 0680ff24-bound packets of chains whose other packets the
                 # dump also draws as 0680ff05; without these, chain fill would copy 0680ff24 onto
                 # them. 06800112 pkt 1093 (9/10 tris on TBP 10756), 1082 (1/5), 06800212 pkt 1048
                 # (12/16); dump-confirmed unbound mates: 1094, 1080/1081/1083, 1044-1047.
                 ('06800112', 1093): ('0680ff05', '0680ff24', False),
                 ('06800112', 1082): ('0680ff05', '0680ff24', False),
                 ('06800212', 1048): ('0680ff05', '0680ff24', False),
                 # 06800212 pkt 579 (2 tris, off-screen in the hero view): both drawn with TBP
                 # 11620 = 0680ff2b (corr 1.000), like chain-mate 578.
                 ('06800212', 579): ('0680ff2b', '0680ff27', False),
                 # Dump-contradicted pf bindings (review 45): every packet whose dump evidence (same
                 # standard as above) names a FileCode other than its binding. Each matched tri is
                 # at 0.1-0.6 px with vertex colour diff 0; none of them hits the old FileCode at
                 # its position. All non-ABE in the BMD and in the dump. "n/m" = matched/total tris.
                 # 0680ff24 -> 0680ff05 (TBP 10756): 06800112 1049 4/8, 1050 10/16, 1051 2/6, 1062 4/4;
                 # 06800212 588 (pf 0680ff33) 3/9.
                 ('06800112', 1049): ('0680ff05', '0680ff24', False),
                 ('06800112', 1050): ('0680ff05', '0680ff24', False),
                 ('06800112', 1051): ('0680ff05', '0680ff24', False),
                 ('06800112', 1062): ('0680ff05', '0680ff24', False),
                 ('06800212', 588): ('0680ff05', '0680ff33', False),
                 # 0680ff24 -> 0680ff08 (TBP 10924): 06800112 904 10/10, 06800212 1035 1/1.
                 ('06800112', 904): ('0680ff08', '0680ff24', False),
                 ('06800212', 1035): ('0680ff08', '0680ff24', False),
                 # 0680ff31 -> 0680ff2e: 06800112 776 1/1 (straddles the screen edge), 1443 1/1.
                 ('06800112', 776): ('0680ff2e', '0680ff31', False),
                 ('06800112', 1443): ('0680ff2e', '0680ff31', False),
                 # Singles: 06800112 705 0680ff32 -> 0680ff85 2/2 (grey facet, like 06800212/370);
                 # 778 0680ff24 -> 0680ff27 8/12; 1111 0680ff87 -> 0680ff37 1/1; 06800212 1040
                 # 0680ff2d -> 0680ff2b 4/8; 1148 0680ff24 -> 0680ff47 2/2; 1064 0680ff32 -> 0680ff33
                 # on 1 of 10 tris only (the other 9 have no dump tri with their UVs: weakest entry).
                 ('06800112', 705): ('0680ff85', '0680ff32', False),
                 ('06800112', 778): ('0680ff27', '0680ff24', False),
                 ('06800112', 1111): ('0680ff37', '0680ff87', False),
                 ('06800212', 1040): ('0680ff2b', '0680ff2d', False),
                 ('06800212', 1148): ('0680ff47', '0680ff24', False),
                 ('06800212', 1064): ('0680ff33', '0680ff32', False)}
# Not overridden: 06800212 pkt 818 (pf 0680ff31, 1 tri, mostly off-screen). Its UV triple sits
# 0.8 px from a 0680ff2e dump tri, but the vertex colours differ by 31 (limit 12): unproven.

# Chain fill (data-sources-2026-10-24 item 4): the texture binding unit is the chain
# (bmd_chains.py), not the packet. An unbound packet takes its chain's FileCode when every
# bound packet of that chain (after BIND_OVERRIDE, excluded packets ignored) agrees. Mixed
# chains are left alone and listed in the manifest. Never touches excluded or overridden
# packets; ALPHA_TEST/ABE grouping then applies to the filled packet as to any other.
# Warnings: fills whose chain has only ONE bound packet (one wrong binding spreads to the
# whole chain; listed in the manifest, summarised on stderr), and chains holding a
# BIND_OVERRIDE that was NOT applied: those are not filled at all, because the override
# says that packet's pf binding is wrong and the fill would copy it to its chain-mates.

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

# z128's chunks do NOT agree on env slot 0 (fog): 06800012/112/212 '38 38 47, 0 810, floor 51',
# 06800312 '39 39 46, 0 640, 128', 06800412-0a12 '39 39 46, 0 640, 240', 06800b12/0c12
# '20 20 29, 0 1280, 240'. Which chunk's env the game makes live is not decoded. 06800012 is
# chosen because it is the one the hero dump confirms: FOGCOL 38,38,47 and floor 51 exact,
# ramp F = 254.56 - 0.3150*w (far 810). Not by triangle count, not by glob order.
ENV_CHUNK = '06800012'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pf', required=True, help='dir of per-packet FileCode pf_<chunk>.json')
    ap.add_argument('-o', '--out', required=True, help='output prefix, e.g. out/z128_room_v2/room')
    ap.add_argument('--sky', help='sky mesh from export_z128_sky.py (basename, next to the '
                    '.list); appended as the last group, textured with ' + SKY_FC)
    ap.add_argument('--env-chunk', default=None, help='chunk whose BMD env slot gives the fog '
                    '(default %s, the one the hero dump confirms; see ENV_CHUNK)' % ENV_CHUNK)
    ap.add_argument('--env-slot', type=int, default=0, help='BMD env slot (0 = level default)')
    ap.add_argument('--no-chain-fill', action='store_true', help='leave unbound packets unbound '
                    'even when their chain agrees on one FileCode')
    a = ap.parse_args()

    cids = [os.path.basename(p)[3:11]
            for p in sorted(glob.glob(os.path.join(a.pf, 'pf_*.json')))]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)

    flat_by = defaultdict(list); ntri_by = defaultdict(int)
    abe_by = defaultdict(bool);  npk_by = defaultdict(int)
    ntri_chunk = defaultdict(int)
    overrides_applied = set()
    chain_filled = {}; chain_mixed = []; chain_single = {}; chain_skipped = []
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
        cwarn = []
        chain = packet_chains(buf, pay, packets, cwarn)
        for w in cwarn:
            print('warning: chains %s: %s' % (cid, w), file=sys.stderr)
        groups = defaultdict(list); final = {}; not_applied = set()
        for i in range(n):
            if (cid, i) in EXCLUDE_PACKETS:
                continue
            fc = pf[i]
            if (cid, i) in BIND_OVERRIDE:
                to, want_pf, want_abe = BIND_OVERRIDE[(cid, i)]
                if pf[i] == want_pf and bool(packets[i]['abe']) == want_abe:
                    fc = to; overrides_applied.add((cid, i))
                else:
                    not_applied.add(i)
                    print('warning: BIND_OVERRIDE %s/%d expects pf %s abe %s, found pf %s abe %s; '
                          'NOT applied (kept pf binding)' % (cid, i, want_pf, want_abe, pf[i],
                          bool(packets[i]['abe'])), file=sys.stderr)
            final[i] = fc
        members = defaultdict(list)
        for i in final:
            members[chain[i]].append(i)
        for c, idxs in members.items():
            fcs = sorted({final[i] for i in idxs if final[i]})
            unb = [i for i in idxs if not final[i]]
            if not unb or not fcs:
                continue
            if len(fcs) > 1:
                chain_mixed.append({'chunk': cid, 'packets': idxs, 'unbound': unb,
                                    'filecodes': {f: sum(1 for i in idxs if final[i] == f) for f in fcs}})
            elif a.no_chain_fill:
                continue
            elif any(i in not_applied for i in idxs):
                skip = [i for i in idxs if i in not_applied]
                chain_skipped.append({'chunk': cid, 'packets': idxs, 'unbound': unb,
                                      'override_not_applied': skip, 'filecode': fcs[0]})
                print('warning: chain fill skipped for %s packets %s: BIND_OVERRIDE on %s not '
                      'applied, so its %s binding would spread to %d unbound packet(s)'
                      % (cid, idxs, skip, fcs[0], len(unb)), file=sys.stderr)
            else:
                anchors = [i for i in idxs if final[i]]
                for i in unb:
                    final[i] = fcs[0]; chain_filled['%s/%d' % (cid, i)] = fcs[0]
                    if len(anchors) == 1:
                        chain_single['%s/%d' % (cid, i)] = '%s/%d' % (cid, anchors[0])
        for i in sorted(final):
            fc = final[i]
            if fc in ALPHA_TEST and packets[i]['abe']:
                fc += ATEST_SUFFIX
            groups[fc if fc else 'unbound'].append(i)
        for key, idxs in groups.items():
            flat, ntri = build_flat(packets, idxs)
            if ntri == 0:
                continue
            flat_by[key].extend(flat); ntri_by[key] += ntri; npk_by[key] += len(idxs)
            ntri_chunk[cid] += ntri
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
    manifest['chain_fill'] = {'enabled': not a.no_chain_fill, 'filled': chain_filled,
                              'single_anchor': chain_single,
                              'mixed_chains_left_unbound': chain_mixed,
                              'override_skipped_chains_left_unbound': chain_skipped}
    if chain_single:
        print('warning: %d of %d chain fills rest on a single bound packet (%d anchors); one '
              'wrong anchor binding spreads to all of them. List: manifest chain_fill.single_anchor'
              % (len(chain_single), len(chain_filled), len(set(chain_single.values()))), file=sys.stderr)
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
    # Fog from the BMD env block (bmd_env.py), not from constants. The sky dome is
    # exempt: the game draws it after the room with its own additive blend.
    env_cid = a.env_chunk or ENV_CHUNK
    if env_cid not in cids:
        sys.exit('env chunk %s is not one of this room\'s chunks (%s)' % (env_cid, ' '.join(cids)))
    def chunk_env(cid):
        p = os.path.join(EXTRACT, cid + '.bin')
        return read_env(open(p, 'rb').read(), a.env_slot) if os.path.exists(p) else None
    try:
        env = chunk_env(env_cid)
    except ValueError as x:
        sys.exit('%s: %s' % (env_cid, x))
    if env is None:
        sys.exit('no BMD env table in %s slot %d' % (env_cid, a.env_slot))
    fog, notes = fog_directive(env)
    for n_ in notes:
        print('warning: %s env slot %d: %s' % (env_cid, a.env_slot, n_), file=sys.stderr)
    # The other chunks' slot: list the ones whose fog disagrees with the chosen one.
    differ = defaultdict(list); agree = [env_cid]
    for cid in cids:
        if cid == env_cid:
            continue
        try:
            e = chunk_env(cid)
            d = 'no env table' if e is None else (fog_directive(e)[0] or 'no @fog')
        except ValueError as x:
            d = 'refused (%s)' % x
        if d != fog:
            differ[d].append(cid)
        else:
            agree.append(cid)
    if differ:
        why = '--env-chunk' if a.env_chunk else 'ENV_CHUNK in export_z128_room.py, dump-confirmed'
        print('warning: chunks disagree on env slot %d fog; using %s (%s; why: %s). Agreeing: '
              '%s (%d room tris). Differing:' % (a.env_slot, env_cid, fog, why, ' '.join(agree),
              sum(ntri_chunk[c] for c in agree)), file=sys.stderr)
        for d, cs in differ.items():
            print('warning:   %s: %s (%d room tris)' % (d, ' '.join(cs),
                  sum(ntri_chunk[c] for c in cs)), file=sys.stderr)
    manifest['env'] = dict(env, chunk=env_cid, fog_directive=fog,
                           chunks_differing={d: cs for d, cs in differ.items()})
    json.dump(manifest, open(a.out + '_manifest.json', 'w'), indent=2)
    with open(a.out + '.list', 'w') as f:
        if fog:
            f.write(fog + '\n')
            if a.sky:
                f.write('@nofog %s\n' % a.sky)
        for g in manifest['groups']:
            f.write('%s\t%s\t%d\t%g\n' % (g['mesh'], g['texture_container'] or 'NONE',
                                          1 if g['abe'] else 0, g.get('alpha_ref', -1)))
    nb = len({g['filecode'] for g in manifest['groups'] if g['filecode'] and not g.get('sky')})
    print('chain fill: %s, %d packets filled (%d on a single anchor); %d mixed chains with '
          'unbound packets left alone; %d chains skipped (override not applied)'
          % ('on' if not a.no_chain_fill else 'OFF', len(chain_filled), len(chain_single),
             len(chain_mixed), len(chain_skipped)))
    print('z128 room: %d chunks, %d textures (+%s unbound), %d/%d tris textured (%.2f%%) -> %s.list'
          % (len(cids), nb, '1' if 'unbound' in flat_by else '0',
             bound, total, 100 * bound / total, a.out))


if __name__ == '__main__':
    main()
