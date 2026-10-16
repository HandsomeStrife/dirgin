#!/usr/bin/env python3
"""Capture texture/CLUT uploads (host->local IMAGE transfers) from a GS dump.
Host->local source data is LINEAR raster order (pre-swizzle), so we decode directly.

python3 -I src/gstex.py <dump.bin> <outdir> [--table]
"""
import sys, struct, numpy as np, os
sys.path.insert(0,__file__.rsplit('/',1)[0])
from gsparse import read_header

PSM={0x00:'CT32',0x01:'CT24',0x02:'CT16',0x0a:'CT16S',0x13:'T8',0x14:'T4',
     0x1b:'T8H',0x24:'T4HL',0x2c:'T4HH',0x30:'Z32',0x31:'Z24',0x32:'Z16',0x3a:'Z16S'}
BPP={0x00:32,0x01:24,0x02:16,0x0a:16,0x13:8,0x14:4,0x1b:8,0x24:4,0x2c:4,0x30:32,0x31:24,0x32:16,0x3a:16}

def capture(path):
    buf=open(path,'rb').read(); h=read_header(buf)
    off=h['packets_at']; n=len(buf)
    st={'bitbltbuf':0,'trxreg':0,'trxpos':0,'trxdir':0}
    uploads=[]   # dict(dbp,dbw,dpsm,rrw,rrh,dsax,dsay,data)
    pending=None
    def write_reg(addr,data):
        nonlocal pending
        if addr==0x50: st['bitbltbuf']=data
        elif addr==0x51: st['trxpos']=data
        elif addr==0x52: st['trxreg']=data
        elif addr==0x53:
            st['trxdir']=data&3
    def feed(data):
        nonlocal pending
        o=0;N=len(data);rem=0; imgbuf=None; imgmeta=None
        while o+16<=N:
            if rem>0:
                take=min(rem,N-o)
                imgbuf.append(data[o:o+take]); o+=take; rem-=take
                if rem==0:
                    raw=b''.join(imgbuf)
                    uploads.append({**imgmeta,'data':raw})
                    imgbuf=None
                continue
            tag=int.from_bytes(data[o:o+16],'little');o+=16
            nloop=tag&0x7fff;pre=(tag>>46)&1;flg=(tag>>58)&3
            nreg=(tag>>60)&0xf or 16
            regs=(tag>>64)
            reglist=[(regs>>(4*i))&0xf for i in range(nreg)]
            if flg==0:
                for _ in range(nloop):
                    for ri in reglist:
                        if o+16>N: return
                        qw=int.from_bytes(data[o:o+16],'little');o+=16
                        if ri==0xe:
                            d=qw&((1<<64)-1); a=(qw>>64)&0xff; write_reg(a,d)
            elif flg==1:
                o+=nloop*nreg*8
            elif flg==2:
                # image transfer begins; snapshot dest state
                bb=st['bitbltbuf']; tr=st['trxreg']; tp=st['trxpos']
                dbp=(bb>>32)&0x3fff; dbw=((bb>>48)&0x3f); dpsm=(bb>>56)&0x3f
                rrw=(tr&0xfff); rrh=((tr>>32)&0xfff)
                dsax=(tp>>32)&0x7ff; dsay=(tp>>48)&0x7ff
                imgmeta=dict(dbp=dbp,dbw=dbw*64,dpsm=dpsm,rrw=rrw,rrh=rrh,dsax=dsax,dsay=dsay)
                rem=nloop*16; imgbuf=[]
    off2=off
    while off2<n:
        t=buf[off2];off2+=1
        if t==0:
            pth=buf[off2];off2+=1;size,=struct.unpack_from('<I',buf,off2);off2+=4
            feed(buf[off2:off2+size]);off2+=size
        elif t==1: off2+=1
        elif t==2: off2+=4
        elif t==3: off2+=0x2000
        else: break
    return uploads

def main():
    path=sys.argv[1]; outdir=sys.argv[2] if len(sys.argv)>2 else None
    ups=capture(path)
    print(f"{len(ups)} uploads")
    # table
    from collections import Counter
    fmt=Counter()
    for u in ups:
        exp=u['rrw']*u['rrh']*BPP.get(u['dpsm'],0)//8
        fmt[(PSM.get(u['dpsm'],hex(u['dpsm'])),u['rrw'],u['rrh'])]+=1
    print("PSM counts:",Counter(PSM.get(u['dpsm'],hex(u['dpsm'])) for u in ups))
    if '--table' in sys.argv:
        for u in ups[:40]:
            exp=u['rrw']*u['rrh']*BPP.get(u['dpsm'],0)//8
            print(f"  dbp={u['dbp']:#06x} dbw={u['dbw']:4d} psm={PSM.get(u['dpsm'],hex(u['dpsm'])):5s} "
                  f"{u['rrw']:4d}x{u['rrh']:4d} dsax={u['dsax']} dsay={u['dsay']} "
                  f"bytes={len(u['data']):7d} exp={exp:7d} {'OK' if len(u['data'])==exp else 'MISMATCH'}")

if __name__=='__main__':
    main()
