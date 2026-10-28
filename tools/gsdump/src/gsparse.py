#!/usr/bin/env python3
"""PCSX2 GS dump parser -> draw calls (primitives, vertices, GS context state).

Format (verified against PCSX2 GSDump.cpp/.h and raw bytes of our dumps):
  u32 marker 0xFFFFFFFF
  u32 header_size = sizeof(GSDumpHeader=36) + serial_size + screenshot_size
  GSDumpHeader (36 bytes): state_version, state_size, serial_offset, serial_size,
      crc, ss_width, ss_height, ss_offset, ss_size   (all u32 LE)
  serial bytes (serial_size)
  screenshot bytes (ss_size = w*h*4)
  freeze/state data (state_size bytes)   <- GSState freeze (regs + 4MB GS mem + ...)
  GSPrivRegSet regs (0x2000 bytes)
  packets...
    type u8: 0=Transfer, 1=VSync, 2=ReadFIFO2, 3=Registers
      Transfer: u8 path, u32 size, size bytes payload (GIF data)
      VSync:    u8 field
      ReadFIFO2:u32 size (no payload)
      Registers:0x2000 bytes

Run isolated: python3 -I src/gsparse.py <dump.bin> [--limit N]
"""
import sys, struct

PRIV_REGSET_SIZE = 0x2000

# GS register addresses we track via A+D writes
R_PRIM=0x00; R_RGBAQ=0x01; R_ST=0x02; R_UV=0x03; R_XYZF2=0x04; R_XYZ2=0x05
R_TEX0_1=0x06; R_TEX0_2=0x07; R_FOG=0x0a; R_XYZF3=0x0c; R_XYZ3=0x0d
R_XYOFFSET_1=0x18; R_XYOFFSET_2=0x19; R_PRMODECONT=0x1a; R_PRMODE=0x1b
R_SCISSOR_1=0x40; R_SCISSOR_2=0x41; R_FRAME_1=0x4c; R_FRAME_2=0x4d
R_ZBUF_1=0x4e; R_ZBUF_2=0x4f; R_BITBLTBUF=0x50; R_TRXPOS=0x51; R_TRXREG=0x52; R_TRXDIR=0x53

PRIM_NAMES={0:'point',1:'line',2:'linestrip',3:'tri',4:'tristrip',5:'trifan',6:'sprite',7:'reserved'}

def read_header(buf):
    marker, = struct.unpack_from('<I', buf, 0)
    assert marker==0xFFFFFFFF, f"bad marker {marker:#x}"
    header_size, = struct.unpack_from('<I', buf, 4)
    (state_version,state_size,serial_offset,serial_size,crc,
     ssw,ssh,ss_offset,ss_size)=struct.unpack_from('<9I', buf, 8)
    serial = buf[8+serial_offset:8+serial_offset+serial_size].decode('ascii','replace')
    h=dict(header_size=header_size,state_version=state_version,state_size=state_size,
           serial=serial,crc=crc,ssw=ssw,ssh=ssh,ss_offset=ss_offset,ss_size=ss_size)
    h['screenshot_at']=8+ss_offset
    h['freeze_at']=8+header_size
    h['regs_at']=h['freeze_at']+state_size
    h['packets_at']=h['regs_at']+PRIV_REGSET_SIZE
    return h

class GIFInterp:
    """Interprets a GIF data stream, accumulating GS context and emitting vertices."""
    def __init__(self):
        # context state
        self.prim=0
        self.tex0=[0,0]   # TEX0_1, TEX0_2 raw 64-bit
        self.xyoffset=[0,0]
        self.frame=[0,0]
        self.scissor=[0,0]
        self.prmodecont=1
        # current vertex attribute latches
        self.st=(0.0,0.0); self.q=1.0; self.rgba=(0,0,0,0); self.uv=(0,0); self.fog=0
        # output: list of draws. each draw = dict(prim, verts[list], tex0, ctx, xyoffset)
        self.draws=[]
        self._cur=None
        self.nverts=0
        # transfer (image upload) tracking
        self.transfers=[]  # (bitbltbuf, trxpos, trxreg, trxdir, nbytes)
        self._img_remaining=0
        self.warnings=[]

    def ctxidx(self):
        return self.prim & 1  # CTXT bit? actually PRIM bit 9 picks ctx; bit 0 is type

    def prim_ctx(self):
        return (self.prim>>9)&1  # CTXT

    def _new_draw(self):
        c=self.prim_ctx()
        # PRIM bits: 0-2 type, 3 IIP, 4 TME, 5 FGE, 6 ABE, 7 AA1, 8 FST, 9 CTXT.
        self._cur=dict(prim=self.prim, ptype=self.prim&7, verts=[],
                       tex0=self.tex0[c], frame=self.frame[c],
                       scissor=self.scissor[c], xyoffset=self.xyoffset[c],
                       fst=(self.prim>>8)&1, tme=(self.prim>>4)&1,
                       abe=(self.prim>>6)&1)
        self.draws.append(self._cur)

    def set_prim(self, val):
        self.prim=val & 0x7ff
        self._new_draw()

    def _emit_vertex(self, x, y, z, f=None, drawkick=True, adc=False):
        # x,y are 16-bit from XYZ2 (12.4 fixed, includes xyoffset)
        c=self.prim_ctx()
        if self._cur is None or self._cur['prim']!=self.prim:
            # prim set via A+D not rekicked; keep current draw
            if self._cur is None:
                self._new_draw()
        v=dict(x=x, y=y, z=z, f=f, s=self.st[0], t=self.st[1], q=self.q,
               u=self.uv[0], vv=self.uv[1],
               r=self.rgba[0], g=self.rgba[1], b=self.rgba[2], a=self.rgba[3],
               adc=adc)
        if drawkick:
            self._cur['verts'].append(v)
        self.nverts+=1

    # ---- A+D register write ----
    def write_reg(self, addr, data):
        if addr==R_PRIM:
            self.set_prim(data & 0x7ff)
        elif addr==R_RGBAQ:
            r=data&0xff; g=(data>>8)&0xff; b=(data>>16)&0xff; a=(data>>24)&0xff
            qf=struct.unpack('<f', struct.pack('<I',(data>>32)&0xffffffff))[0]
            self.rgba=(r,g,b,a); self.q=qf
        elif addr==R_ST:
            s=struct.unpack('<f',struct.pack('<I',data&0xffffffff))[0]
            t=struct.unpack('<f',struct.pack('<I',(data>>32)&0xffffffff))[0]
            self.st=(s,t)
        elif addr==R_UV:
            u=(data&0x3fff); vv=((data>>16)&0x3fff)
            self.uv=(u,vv)
        elif addr==R_XYZ2 or addr==R_XYZF2:
            x=data&0xffff; y=(data>>16)&0xffff
            if addr==R_XYZ2:
                z=(data>>32)&0xffffffff; f=None
            else:
                z=(data>>32)&0xffffff; f=(data>>56)&0xff
            self._emit_vertex(x,y,z,f,drawkick=True)
        elif addr==R_XYZ3 or addr==R_XYZF3:
            x=data&0xffff; y=(data>>16)&0xffff
            if addr==R_XYZ3:
                z=(data>>32)&0xffffffff; f=None
            else:
                z=(data>>32)&0xffffff; f=(data>>56)&0xff
            self._emit_vertex(x,y,z,f,drawkick=False)  # no draw kick
        elif addr==R_TEX0_1: self.tex0[0]=data
        elif addr==R_TEX0_2: self.tex0[1]=data
        elif addr==R_XYOFFSET_1: self.xyoffset[0]=data
        elif addr==R_XYOFFSET_2: self.xyoffset[1]=data
        elif addr==R_FRAME_1: self.frame[0]=data
        elif addr==R_FRAME_2: self.frame[1]=data
        elif addr==R_SCISSOR_1: self.scissor[0]=data
        elif addr==R_SCISSOR_2: self.scissor[1]=data
        elif addr==R_PRMODECONT: self.prmodecont=data&1
        elif addr==R_FOG: self.fog=(data>>56)&0xff
        # image transfer registers tracked loosely
        elif addr==R_BITBLTBUF: self._bitbltbuf=data
        elif addr==R_TRXPOS: self._trxpos=data
        elif addr==R_TRXREG: self._trxreg=data
        elif addr==R_TRXDIR:
            self._trxdir=data&3
        # others ignored

    # ---- PACKED register descriptors ----
    def packed(self, reg, qw):
        # qw = 128-bit int (little endian from 16 bytes)
        w0=qw&0xffffffff; w1=(qw>>32)&0xffffffff; w2=(qw>>64)&0xffffffff; w3=(qw>>96)&0xffffffff
        if reg==0x0:  # PRIM
            self.set_prim(qw & 0x7ff)
        elif reg==0x1:  # RGBAQ
            r=w0&0xff; g=w1&0xff; b=w2&0xff; a=w3&0xff
            qf=struct.unpack('<f',struct.pack('<I',w1 if False else (qw>>96)&0xffffffff))[0] if False else self.q
            # Q comes from the packed ST's Q actually; in PACKED RGBAQ, Q is w3? No.
            # PACKED RGBAQ: R=w0[7:0],G=w1[7:0],B=w2[7:0],A=w3[7:0]; Q is the internal Q (from ST)
            self.rgba=(r,g,b,a)
        elif reg==0x2:  # ST
            s=struct.unpack('<f',struct.pack('<I',w0))[0]
            t=struct.unpack('<f',struct.pack('<I',w1))[0]
            qf=struct.unpack('<f',struct.pack('<I',w2))[0]
            self.st=(s,t); self.q=qf
        elif reg==0x3:  # UV
            u=w0&0x3fff; vv=w1&0x3fff
            self.uv=(u,vv)
        elif reg==0x4:  # XYZF2
            x=w0&0xffff; y=w1&0xffff
            z=(w2>>4)&0xffffff; f=(w3>>4)&0xff
            adc=(w3>>15)&1
            self._emit_vertex(x,y,z,f,drawkick=(adc==0),adc=bool(adc))
        elif reg==0x5:  # XYZ2
            x=w0&0xffff; y=w1&0xffff; z=w2
            adc=(w3>>15)&1
            self._emit_vertex(x,y,z,None,drawkick=(adc==0),adc=bool(adc))
        elif reg==0x6: self.tex0[0]=qw & ((1<<64)-1)
        elif reg==0x7: self.tex0[1]=qw & ((1<<64)-1)
        elif reg==0xa:  # FOG
            self.fog=(w3>>4)&0xff
        elif reg==0xe:  # A+D
            data=qw & ((1<<64)-1)
            addr=(qw>>64)&0xff
            self.write_reg(addr,data)
        elif reg==0xf:  # NOP
            pass
        # 0x8 CLAMP1,0x9 CLAMP2,0xb skip,0xc XYZF3,0xd XYZ3 handled minimally
        elif reg==0xc:
            x=w0&0xffff; y=w1&0xffff; z=(w2>>4)&0xffffff; f=(w3>>4)&0xff
            self._emit_vertex(x,y,z,f,drawkick=False)
        elif reg==0xd:
            x=w0&0xffff; y=w1&0xffff; z=w2
            self._emit_vertex(x,y,z,None,drawkick=False)

    def feed(self, data):
        """Parse a GIF transfer payload (sequence of GIFtag + register data)."""
        off=0; n=len(data)
        while off+16<=n:
            if self._img_remaining>0:
                take=min(self._img_remaining, n-off)
                off+=take; self._img_remaining-=take
                continue
            tag=int.from_bytes(data[off:off+16],'little'); off+=16
            nloop=tag & 0x7fff
            eop=(tag>>15)&1
            pre=(tag>>46)&1
            prim=(tag>>47)&0x7ff
            flg=(tag>>58)&3
            nreg=(tag>>60)&0xf
            if nreg==0: nreg=16
            regs=(tag>>64)&0xffffffffffffffff
            reglist=[(regs>>(4*i))&0xf for i in range(nreg)]
            if pre:
                self.set_prim(prim)
            if flg==0:  # PACKED
                for _ in range(nloop):
                    for ri in reglist:
                        if off+16>n: return off
                        qw=int.from_bytes(data[off:off+16],'little'); off+=16
                        self.packed(ri, qw)
            elif flg==1:  # REGLIST
                total=nloop*nreg
                for i in range(total):
                    if off+8>n: return off
                    val=int.from_bytes(data[off:off+8],'little'); off+=8
                    ri=reglist[i%nreg]
                    # reglist uses the same descriptors but data is raw 64-bit per reg
                    self._reglist_write(ri,val)
                # pad to qword if odd count handled by caller? GIF pads the final
                if total & 1:
                    # one extra 64-bit slot of padding to align to qword
                    pass
            elif flg==2:  # IMAGE
                nbytes=nloop*16
                self._img_remaining=nbytes
            elif flg==3:  # disabled
                pass
            if eop and off>=n:
                break
        return off

    def _reglist_write(self, ri, val):
        # REGLIST: each reg is 64-bit. Map descriptor to a write.
        if ri==0x0: self.set_prim(val&0x7ff)
        elif ri==0x1:
            r=val&0xff; g=(val>>8)&0xff; b=(val>>16)&0xff; a=(val>>24)&0xff
            self.rgba=(r,g,b,a)
            self.q=struct.unpack('<f',struct.pack('<I',(val>>32)&0xffffffff))[0]  # RGBAQ bits 63:32
        elif ri==0x2:
            s=struct.unpack('<f',struct.pack('<I',val&0xffffffff))[0]
            t=struct.unpack('<f',struct.pack('<I',(val>>32)&0xffffffff))[0]
            self.st=(s,t)
        elif ri==0x3:
            self.uv=(val&0x3fff,(val>>16)&0x3fff)
        elif ri==0x4:
            x=val&0xffff;y=(val>>16)&0xffff;z=(val>>32)&0xffffff;f=(val>>56)&0xff
            self._emit_vertex(x,y,z,f,drawkick=True)
        elif ri==0x5:
            x=val&0xffff;y=(val>>16)&0xffff;z=(val>>32)&0xffffffff
            self._emit_vertex(x,y,z,None,drawkick=True)
        elif ri==0x6: self.tex0[0]=val
        elif ri==0x7: self.tex0[1]=val
        elif ri==0xe:
            data=val  # in reglist A+D is still 64 data + addr? Actually reglist can't do A+D well
        # minimal

def parse_packets(buf, start, limit=None):
    gif=GIFInterp()
    off=start; n=len(buf)
    counts={'transfer':0,'vsync':0,'readfifo':0,'registers':0}
    paths={0:0,1:0,2:0,3:0,4:0}
    while off<n:
        t=buf[off]; off+=1
        if t==0:  # Transfer
            path=buf[off]; off+=1
            size,=struct.unpack_from('<I',buf,off); off+=4
            payload=buf[off:off+size]; off+=size
            counts['transfer']+=1; paths[path]=paths.get(path,0)+1
            gif.feed(payload)
        elif t==1:  # VSync
            off+=1
            counts['vsync']+=1
        elif t==2:  # ReadFIFO2
            off+=4
            counts['readfifo']+=1
        elif t==3:  # Registers
            off+=PRIV_REGSET_SIZE
            counts['registers']+=1
        else:
            return gif,counts,paths,off,f"bad packet type {t} at {off-1:#x}"
        if limit and counts['transfer']>=limit:
            break
    return gif,counts,paths,off,None

def main():
    args=[a for a in sys.argv[1:] if not a.startswith('--')]
    path=args[0]
    limit=None
    for a in sys.argv:
        if a.startswith('--limit='): limit=int(a.split('=')[1])
    with open(path,'rb') as f: buf=f.read()
    h=read_header(buf)
    print(f"serial={h['serial']} crc={h['crc']:#x} version={h['state_version']}")
    print(f"screenshot {h['ssw']}x{h['ssh']} at {h['screenshot_at']:#x}")
    print(f"freeze_at={h['freeze_at']:#x} state_size={h['state_size']:#x}")
    print(f"regs_at={h['regs_at']:#x} packets_at={h['packets_at']:#x} filesize={len(buf):#x}")
    gif,counts,paths,endoff,err=parse_packets(buf,h['packets_at'],limit)
    print(f"packets: {counts}  paths={paths}")
    print(f"ended at {endoff:#x} (file {len(buf):#x})  err={err}")
    print(f"draws={len(gif.draws)}  total_verts_kicked={gif.nverts}  warnings={len(gif.warnings)}")
    # summarize draw prim types + vertex counts
    from collections import Counter
    pc=Counter(); vc=0
    for d in gif.draws:
        pc[PRIM_NAMES.get(d['ptype'],d['ptype'])]+=1
        vc+=len(d['verts'])
    print(f"draw prim types: {dict(pc)}  verts_in_draws={vc}")
    # distinct textures (TBP0)
    texs=Counter()
    for d in gif.draws:
        tbp0=d['tex0']&0x3fff
        psm=(d['tex0']>>20)&0x3f
        texs[(tbp0,psm)]+=len(d['verts'])
    print(f"distinct (TBP0,PSM) textures across draws: {len(texs)}")
    top=sorted(texs.items(),key=lambda kv:-kv[1])[:20]
    for (tbp0,psm),nv in top:
        print(f"  TBP0={tbp0:#06x} PSM={psm:#04x} verts={nv}")

if __name__=='__main__':
    main()
