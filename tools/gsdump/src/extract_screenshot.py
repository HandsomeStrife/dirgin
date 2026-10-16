#!/usr/bin/env python3
"""Extract the embedded 640x480 screenshot from a GS dump -> PNG.
python3 -I src/extract_screenshot.py <dump.bin> <out.png>"""
import sys, struct, numpy as np
from PIL import Image
sys.path.insert(0,__file__.rsplit('/',1)[0])
from gsparse import read_header

buf=open(sys.argv[1],'rb').read()
h=read_header(buf)
w,hh=h['ssw'],h['ssh']
off=h['screenshot_at']
px=np.frombuffer(buf[off:off+w*hh*4],dtype=np.uint8).reshape(hh,w,4).copy()
# PCSX2 stores RGBA (little-endian u32 = R,G,B,A byte order). Alpha often 0/127; force opaque.
rgb=px[:,:,:3]
Image.fromarray(rgb,'RGB').save(sys.argv[2])
print(f"saved {sys.argv[2]} {w}x{hh}  alpha range {px[:,:,3].min()}-{px[:,:,3].max()}")
