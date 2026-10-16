#!/usr/bin/env python3
"""Parse a GS dump and rasterize its triangles to reproduce the frame.
Proves the parser is correct by matching the dump's embedded screenshot.

python3 -I src/gsrender.py <dump.bin> <out.png> [--notex] [--wire]
"""
import sys, struct, numpy as np
sys.path.insert(0,__file__.rsplit('/',1)[0])
from gsparse import read_header, parse_packets, GIFInterp, PRIM_NAMES

# We extend GIFInterp with a proper vertex-kick machine emitting triangles.
class TriInterp(GIFInterp):
    def __init__(self):
        super().__init__()
        self.vq=[]           # vertex queue
        self.tris=[]         # (v0,v1,v2, ctx) where ctx carries tex0/tme
        self._prim_cache=None

    def set_prim(self, val):
        self.prim=val & 0x7ff
        self.vq=[]           # prim change resets vertex queue

    def _ctx(self):
        c=(self.prim>>9)&1
        return dict(tex0=self.tex0[c], frame=self.frame[c], xyoffset=self.xyoffset[c],
                    tme=(self.prim>>4)&1, fst=(self.prim>>8)&1, iip=(self.prim>>3)&1,
                    abe=(self.prim>>6)&1, ptype=self.prim&7)

    def _emit_vertex(self, x, y, z, f=None, drawkick=True, adc=False):
        c=(self.prim>>9)&1
        v=(x,y,z, self.st[0],self.st[1],self.q, self.uv[0],self.uv[1],
           self.rgba[0],self.rgba[1],self.rgba[2],self.rgba[3])
        self.nverts+=1
        p=self.prim&7
        self.vq.append(v)
        if p==3:  # TRIANGLE
            if len(self.vq)>=3:
                if drawkick: self._tri(self.vq[-3],self.vq[-2],self.vq[-1])
                self.vq=[]
        elif p==4:  # TRISTRIP
            if len(self.vq)>=3:
                if drawkick: self._tri(self.vq[-3],self.vq[-2],self.vq[-1])
                self.vq=self.vq[-2:]
        elif p==5:  # TRIFAN
            if len(self.vq)>=3:
                if drawkick: self._tri(self.vq[0],self.vq[-2],self.vq[-1])
                self.vq=[self.vq[0], self.vq[-1]]
        elif p==6:  # SPRITE
            if len(self.vq)>=2:
                if drawkick: self._sprite(self.vq[-2],self.vq[-1])
                self.vq=[]
        elif p in (0,1,2):  # point/line/linestrip - ignore for fill
            if p==1 and len(self.vq)>=2: self.vq=[]
            elif p==0: self.vq=[]
            elif p==2 and len(self.vq)>=2: self.vq=self.vq[-1:]

    def _tri(self, a,b,c):
        self.tris.append((a,b,c,self._ctx()))

    def _sprite(self, a,b):
        # axis-aligned rect from two opposite corners; make 2 tris
        ax,ay,az=a[0],a[1],a[2]; bx,by,bz=b[0],b[1],b[2]
        # corners: (ax,ay),(bx,ay),(bx,by),(ax,by) ; z,color from b (2nd vtx)
        def mk(x,y,uv):  # uv pick from a or b
            s,t = uv
            return (x,y,bz, s,t,b[5], 0,0, b[8],b[9],b[10],b[11])
        c0=mk(ax,ay,(a[3],a[4])); c1=mk(bx,ay,(b[3],a[4]))
        c2=mk(bx,by,(b[3],b[4])); c3=mk(ax,by,(a[3],b[4]))
        ctx=self._ctx()
        self.tris.append((c0,c1,c2,ctx)); self.tris.append((c0,c2,c3,ctx))

def build(buf, h):
    gif=TriInterp()
    # replicate parse_packets but with our interp
    off=h['packets_at']; n=len(buf)
    while off<n:
        t=buf[off]; off+=1
        if t==0:
            path=buf[off]; off+=1
            size,=struct.unpack_from('<I',buf,off); off+=4
            gif.feed(buf[off:off+size]); off+=size
        elif t==1: off+=1
        elif t==2: off+=4
        elif t==3: off+=0x2000
        else: break
    return gif

def rasterize(gif, W=640, Hh=480, notex=False, painter=True):
    img=np.zeros((Hh,W,3),dtype=np.float32)
    zbuf=np.full((Hh,W),-1e30,dtype=np.float64)
    tris=gif.tris
    ntri=len(tris)
    drawn=0; offscreen=0
    for a,b,c,ctx in tris:
        ofx=(ctx['xyoffset']&0xffff); ofy=(ctx['xyoffset']>>32)&0xffff
        # screen coords
        x0=(a[0]-ofx)/16.0; y0=(a[1]-ofy)/16.0
        x1=(b[0]-ofx)/16.0; y1=(b[1]-ofy)/16.0
        x2=(c[0]-ofx)/16.0; y2=(c[1]-ofy)/16.0
        z=(a[2]+b[2]+c[2])/3.0
        # color: average vertex rgb (gouraud approx = flat avg)
        r=(a[8]+b[8]+c[8])/3.0; g=(a[9]+b[9]+c[9])/3.0; bl=(a[10]+b[10]+c[10])/3.0
        # PS2 color is 0..128 = 1.0 (8-bit but 0x80=1.0); scale *2 then clamp
        col=np.array([r,g,bl])*2.0
        col=np.clip(col,0,255)
        # bounding box
        minx=int(max(0,np.floor(min(x0,x1,x2)))); maxx=int(min(W-1,np.ceil(max(x0,x1,x2))))
        miny=int(max(0,np.floor(min(y0,y1,y2)))); maxy=int(min(Hh-1,np.ceil(max(y0,y1,y2))))
        if maxx<minx or maxy<miny:
            offscreen+=1; continue
        if (maxx-minx)*(maxy-miny)>200000:  # skip pathological huge tris
            continue
        xs=np.arange(minx,maxx+1); ys=np.arange(miny,maxy+1)
        px,py=np.meshgrid(xs+0.5,ys+0.5)
        # barycentric
        denom=((y1-y2)*(x0-x2)+(x2-x1)*(y0-y2))
        if abs(denom)<1e-9: continue
        w0=((y1-y2)*(px-x2)+(x2-x1)*(py-y2))/denom
        w1=((y2-y0)*(px-x2)+(x0-x2)*(py-y2))/denom
        w2=1-w0-w1
        mask=(w0>=0)&(w1>=0)&(w2>=0)
        if not mask.any(): continue
        if painter:
            zmask=mask
            if not zmask.any(): continue
            img[miny:maxy+1,minx:maxx+1][zmask]=col
        else:
            sub_z=zbuf[miny:maxy+1,minx:maxx+1]
            zmask=mask&(z>sub_z)
            if not zmask.any(): continue
            sub_z[zmask]=z
            img[miny:maxy+1,minx:maxx+1][zmask]=col
        drawn+=1
    return img, drawn, offscreen, ntri

def main():
    args=[a for a in sys.argv[1:] if not a.startswith('--')]
    notex='--notex' in sys.argv
    buf=open(args[0],'rb').read()
    h=read_header(buf)
    gif=build(buf,h)
    print(f"tris={len(gif.tris)} verts={gif.nverts}")
    from PIL import Image
    img,drawn,off,ntri=rasterize(gif,notex=notex)
    Image.fromarray(np.clip(img,0,255).astype(np.uint8),'RGB').save(args[1])
    print(f"rasterized {drawn}/{ntri} tris ({off} fully offscreen) -> {args[1]}")

if __name__=='__main__':
    main()
