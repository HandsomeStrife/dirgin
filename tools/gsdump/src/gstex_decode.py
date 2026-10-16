#!/usr/bin/env python3
"""Decode captured GS texture uploads to PNG. CT32 direct; T8/T4 via CLUT match.
python3 -I src/gstex_decode.py <dump.bin> <outdir>"""
import sys, struct, numpy as np, os, hashlib
sys.path.insert(0,__file__.rsplit('/',1)[0])
from gstex import capture, PSM, BPP
from PIL import Image

def decode_ct32(data,w,h):
    a=np.frombuffer(data,dtype=np.uint8)[:w*h*4].reshape(h,w,4).copy()
    return a  # RGBA

def decode_ct24(data,w,h):
    a=np.frombuffer(data,dtype=np.uint8)[:w*h*3].reshape(h,w,3).copy()
    out=np.zeros((h,w,4),np.uint8); out[:,:,:3]=a; out[:,:,3]=128
    return out

def decode_ct16(data,w,h):
    v=np.frombuffer(data,dtype=np.uint16)[:w*h].reshape(h,w).astype(np.uint32)
    r=((v)&0x1f)<<3; g=((v>>5)&0x1f)<<3; b=((v>>10)&0x1f)<<3; a=((v>>15)&1)*255
    out=np.stack([r,g,b,np.full_like(r,255)],-1).astype(np.uint8); return out

def unswizzle8(src_bytes, w, hgt):
    """Canonical PS2 PSMT8 deswizzle (numpy port of dtex_unswizzle8).
    src_bytes = the raw upload (written to VRAM as CT32, laid out in PSMT8 page
    order); returns a (hgt,w) array of palette indices."""
    y,x=np.meshgrid(np.arange(hgt),np.arange(w),indexing='ij')
    block=(y&~0xf)*w+(x&~0xf)*2
    swap=(((y+2)>>2)&1)*4
    pos_y=(((y&~3)>>1)+(y&1))&7
    col=pos_y*w*2+((x+swap)&7)*4
    byte=((y>>1)&1)+((x>>2)&2)
    idx=np.clip((block+col+byte).astype(np.int64),0,len(src_bytes)-1)
    return np.frombuffer(src_bytes,dtype=np.uint8)[idx]

def apply_clut(idx, pal):
    """idx=(h,w) indices; pal=Nx4 CT32 CLUT. Applies CSM1 index unswizzle for 256."""
    if len(pal)>=256:
        order=np.arange(256); order=((order&0xE7)|((order&0x08)<<1)|((order&0x10)>>1))
        pal=pal[order]
    else:
        pal=np.vstack([pal,np.zeros((max(0,256-len(pal)),4),np.uint8)])[:256]
    return pal[idx]

def clut_from(data):
    # CLUT uploaded as CT32; return Nx4 palette
    n=len(data)//4
    return np.frombuffer(data,dtype=np.uint8)[:n*4].reshape(n,4).copy()

def decode_t8(data,w,h,clut):
    idx=np.frombuffer(data,dtype=np.uint8)[:w*h].reshape(h,w)
    pal=clut if clut is not None and len(clut)>=256 else None
    if pal is None:
        g=idx; return np.stack([g,g,g,np.full_like(g,255)],-1)
    # PS2 CT32 CLUT for 8-bit uses a swizzled order (CSM1): swap bits 3<->4 of index
    order=np.arange(256); order=((order&0xE7)|((order&0x08)<<1)|((order&0x10)>>1))
    pal2=pal[order]
    return pal2[idx]

def decode_t4(data,w,h,clut):
    b=np.frombuffer(data,dtype=np.uint8)[:w*h//2]
    idx=np.zeros(w*h,np.uint8); idx[0::2]=b&0x0f; idx[1::2]=b>>4
    idx=idx.reshape(h,w)
    if clut is None or len(clut)<16:
        g=(idx*17).astype(np.uint8); return np.stack([g,g,g,np.full_like(g,255)],-1)
    return clut[:16][idx]

def main():
    ups=capture(sys.argv[1]); outdir=sys.argv[2]
    os.makedirs(outdir,exist_ok=True)
    # collect candidate CLUTs: small CT32 uploads (<=256 entries)
    cluts={}  # dbp -> palette ; also keep most-recent
    recent_clut=None
    saved=0; seen=set()
    thumbs=[]
    for i,u in enumerate(ups):
        psm=u['dpsm']; w=u['rrw']; h=u['rrh']; data=u['data']
        name=PSM.get(psm,hex(psm))
        # treat tiny CT32 (<=256 px, and w*h matches palette sizes) as potential CLUT
        if psm==0x00 and w*h in (16,256) :
            recent_clut=clut_from(data); cluts[u['dbp']]=recent_clut
        try:
            if psm==0x00: img=decode_ct32(data,w,h)
            elif psm==0x01: img=decode_ct24(data,w,h)
            elif psm==0x02: img=decode_ct16(data,w,h)
            elif psm==0x13: img=decode_t8(data,w,h,recent_clut)
            elif psm==0x14: img=decode_t4(data,w,h,recent_clut)
            else: continue
        except Exception as e:
            continue
        hh=hashlib.md5(data).hexdigest()[:8]
        if hh in seen: continue
        seen.add(hh)
        # skip trivial (solid color / tiny clut-like)
        if w<8 or h<8: continue
        rgb=img[:,:,:3]
        fn=f"{outdir}/t{i:04d}_{name}_{w}x{h}_dbp{u['dbp']:04x}_{hh}.png"
        Image.fromarray(rgb,'RGB').save(fn); saved+=1
        thumbs.append((fn,w,h,name))
    print(f"saved {saved} unique textures to {outdir}")
    # montage up to 64 largest
    thumbs=sorted(thumbs,key=lambda t:-(t[1]*t[2]))[:64]
    cols=8; cell=128
    rows=(len(thumbs)+cols-1)//cols
    sheet=Image.new('RGB',(cols*cell,rows*cell),(30,30,30))
    for k,(fn,w,h,name) in enumerate(thumbs):
        im=Image.open(fn).resize((cell-4,cell-4))
        sheet.paste(im,((k%cols)*cell+2,(k//cols)*cell+2))
    sheet.save(f"{outdir}/_montage.png")
    print(f"montage -> {outdir}/_montage.png ({len(thumbs)} tiles)")

if __name__=='__main__':
    main()
