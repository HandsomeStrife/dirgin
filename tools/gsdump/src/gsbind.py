#!/usr/bin/env python3
"""Ground-truth texture<->geometry binding from a GS dump.
For each distinct texture a draw samples (TEX0), decode it from the linear upload
at TBP0 + the CLUT upload at CBP, and report how many triangles bind to it.
This is the runtime texture-binding ORACLE (the room decoder's missing piece),
read directly off a real frame.

python3 -I src/gsbind.py <dump.bin> <outdir>
"""
import sys, struct, numpy as np, os, hashlib
sys.path.insert(0,__file__.rsplit('/',1)[0])
from gsparse import read_header
from gstex import capture, PSM, BPP
from gsrender import build
from gstex_decode import decode_ct32, decode_ct24, decode_ct16, clut_from, unswizzle8, apply_clut
from PIL import Image

def tex0_fields(t):
    return dict(tbp0=t&0x3fff, tbw=(t>>14)&0x3f, psm=(t>>20)&0x3f,
                tw=(t>>26)&0xf, th=(t>>30)&0xf, cbp=(t>>37)&0x3fff,
                cpsm=(t>>51)&0xf, csa=(t>>56)&0x1f)

def main():
    dump=sys.argv[1]; outdir=sys.argv[2]; os.makedirs(outdir,exist_ok=True)
    buf=open(dump,'rb').read(); h=read_header(buf)
    ups=capture(dump)
    # index uploads by dest addr: keep the one whose rrw*rrh is largest at that dbp
    # (full texture), plus keep a map for CLUT lookups (any upload to that addr).
    by_dbp={}
    for u in ups:
        by_dbp.setdefault(u['dbp'],[]).append(u)
    def best_at(dbp):
        lst=by_dbp.get(dbp)
        if not lst: return None
        return max(lst,key=lambda u:u['rrw']*u['rrh'])
    # draws
    gif=build(buf,h)
    # group triangles by full tex0
    from collections import defaultdict
    tex_tris=defaultdict(int); tex_bbox=defaultdict(lambda:[1e9,1e9,-1e9,-1e9])
    for a,b,c,ctx in gif.tris:
        t=ctx['tex0'] if ctx['tme'] else 0
        tex_tris[t]+=1
    print(f"distinct sampled TEX0: {len(tex_tris)}  (tme draws)")
    rows=[]; saved=[]
    for t,ntri in sorted(tex_tris.items(),key=lambda kv:-kv[1]):
        if t==0:
            rows.append((ntri,'(untextured)','','')); continue
        f=tex0_fields(t)
        w=1<<f['tw']; hgt=1<<f['th']
        up=best_at(f['tbp0'])
        status='resident/no-upload'
        img=None
        spsm=f['psm']   # SAMPLED pixel format (from TEX0), may differ from upload PSM
        if up is not None:
            data=up['data']; uw,uh=up['rrw'],up['rrh']
            try:
                if spsm==0x13:   # PSMT8: upload (CT32 fast path) deswizzled to linear
                    idx=unswizzle8(data,w,hgt)
                    cu=best_at(f['cbp'])
                    img=apply_clut(idx, clut_from(cu['data'])) if cu else None
                    status=f'T8 {w}x{hgt} (deswizzled)'
                elif spsm==0x00: img=decode_ct32(data,uw,uh); status=f'CT32 {uw}x{uh}'
                elif spsm==0x01: img=decode_ct24(data,uw,uh); status=f'CT24 {uw}x{uh}'
                elif spsm==0x02: img=decode_ct16(data,uw,uh); status=f'CT16 {uw}x{uh}'
                elif spsm==0x14:
                    status=f'T4 {w}x{hgt} (needs dtex_unswizzle4; skipped)'
                else:
                    status=f'{PSM.get(spsm,hex(spsm))} (unhandled)'
            except Exception as e:
                status=f'decode-err {e}'
        rows.append((ntri,f"TBP0={f['tbp0']:#06x}",f"PSM={PSM.get(f['psm'],hex(f['psm']))}",status))
        if img is not None:
            hh=hashlib.md5(img.tobytes()).hexdigest()[:6]
            fn=f"{outdir}/mat_{ntri:06d}tri_tbp{f['tbp0']:04x}_{hh}.png"
            Image.fromarray(img[:,:,:3],'RGB').save(fn); saved.append((fn,ntri,status))
    # report
    print(f"{'tris':>8}  {'texture':<16} {'psm':<10} status")
    for ntri,a,b,st in rows[:30]:
        print(f"{ntri:>8}  {a:<16} {b:<10} {st}")
    tot=sum(tex_tris.values()); bound=sum(n for t,n in tex_tris.items() if t!=0 and best_at(tex0_fields(t)['tbp0']))
    print(f"\ntotal textured tris={tot}  with decodable upload={bound} ({100*bound/max(tot,1):.0f}%)")
    # montage of per-material textures, sorted by tri count (importance)
    saved=sorted(saved,key=lambda s:-s[1])[:48]
    if saved:
        cols=8; cell=112; rows_n=(len(saved)+cols-1)//cols
        sheet=Image.new('RGB',(cols*cell,rows_n*cell),(25,25,25))
        for k,(fn,ntri,st) in enumerate(saved):
            im=Image.open(fn).resize((cell-6,cell-6))
            sheet.paste(im,((k%cols)*cell+3,(k//cols)*cell+3))
        sheet.save(f"{outdir}/_materials.png")
        print(f"material montage (by tri-count) -> {outdir}/_materials.png")

if __name__=='__main__':
    main()
