#!/usr/bin/env python3
"""Match GS-dump-extracted textures against on-disk extracted room textures.
python3 -I src/match_tex.py <gs_tex_dir> <ondisk_glob...> """
import sys, glob, numpy as np
from PIL import Image

def sig(path, n=16):
    try:
        im=Image.open(path).convert('RGB').resize((n,n))
    except Exception: return None
    a=np.asarray(im,dtype=np.float32).reshape(-1)
    a=(a-a.mean())/(a.std()+1e-6)
    return a

def aspect(path):
    try:
        w,h=Image.open(path).size; return w,h
    except: return 0,0

gs_dir=sys.argv[1]
ondisk=[]
for g in sys.argv[2:]:
    ondisk+=glob.glob(g)
print(f"on-disk corpus: {len(ondisk)}")
# precompute on-disk sigs
od_sigs=[]; od_paths=[]
for p in ondisk:
    s=sig(p)
    if s is not None: od_sigs.append(s); od_paths.append(p)
od=np.array(od_sigs)  # (M,768)
print(f"indexed {len(od_paths)} on-disk sigs")

gs=sorted(glob.glob(gs_dir+'/t*.png'))
hits=0
for p in gs:
    s=sig(p)
    if s is None: continue
    # normalized correlation = dot / N
    corr=(od @ s)/len(s)
    j=int(np.argmax(corr)); best=corr[j]
    if best>0.80:
        hits+=1
        print(f"  {p.split('/')[-1][:38]:38s} -> {od_paths[j].split('/')[-2]}/{od_paths[j].split('/')[-1]:30s} corr={best:.3f}")
print(f"\n{hits} strong matches (corr>0.80) of {len(gs)} GS textures")
