#!/usr/bin/env python3
"""Probe: do the 13 z143 room geometry chunks live in a SHARED world space
(global nearest-neighbour valid) or each in LOCAL coords (per-chunk only)?
Also sanity-check decode_packets order == pf_*.json order (same length).
Reads only; prints bounding boxes + centroids per chunk.
"""
import sys, os, json, glob
sys.path.insert(0, '/srv/games/dirge-port/native/assets/models')
import numpy as np
from export_bmd_textured import find_bmd_payload, decode_packets

EXT = '/srv/games/dirge-port/extract/_KEL/noPath'
PF  = '/srv/games/dirge-port/native/assets/models/out/z143_pktfc'

def chunk_ids():
    return [os.path.basename(p)[3:11] for p in sorted(glob.glob(PF + '/pf_*.json'))]

def main():
    gmin = np.array([ 1e18]*3); gmax = np.array([-1e18]*3)
    print("%-10s %6s %6s  %-42s %-42s" % ("chunk","npkt","pf", "bbox min (x,y,z)","bbox max (x,y,z)"))
    for cid in chunk_ids():
        binp = '%s/%s.bin' % (EXT, cid)
        pfp  = '%s/pf_%s.json' % (PF, cid)
        if not os.path.exists(binp):
            print("%-10s  MISSING .bin" % cid); continue
        buf = open(binp,'rb').read()
        pay = find_bmd_payload(buf)
        pk  = decode_packets(buf, pay) if pay is not None else []
        pf  = json.load(open(pfp))
        allv = np.array([v for p in pk for v in p['verts']], dtype=float) if pk else np.zeros((0,3))
        if len(allv):
            mn, mx = allv.min(0), allv.max(0)
            gmin = np.minimum(gmin, mn); gmax = np.maximum(gmax, mx)
            cen = allv.mean(0)
        else:
            mn = mx = cen = np.zeros(3)
        flag = '' if len(pk)==len(pf) else '  <<< LEN MISMATCH pk=%d pf=%d'%(len(pk),len(pf))
        print("%-10s %6d %6d  (%8.1f %8.1f %8.1f)  (%8.1f %8.1f %8.1f)  cen=(%7.1f %7.1f %7.1f)%s"
              % (cid, len(pk), len(pf), mn[0],mn[1],mn[2], mx[0],mx[1],mx[2], cen[0],cen[1],cen[2], flag))
    print("\nGLOBAL bbox: min (%.1f %.1f %.1f)  max (%.1f %.1f %.1f)  extent (%.1f %.1f %.1f)"
          % (*gmin, *gmax, *(gmax-gmin)))

if __name__ == '__main__':
    main()
