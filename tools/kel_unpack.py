import sys, struct, zlib, os

# usage: kel_unpack.py FILELIST.BIN KEL.DAT OUTDIR [ext1,ext2,...  | ALL]
flp, datp, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
filt = sys.argv[4] if len(sys.argv) > 4 else "ALL"
exts = None if filt == "ALL" else set("." + e.lstrip(".") for e in filt.split(","))

fl = open(flp, 'rb').read()
cio, cdo, fcount = struct.unpack_from('<III', fl, 0)
num_chunks = (cdo - cio) // 0xC
chunks = []
for c in range(num_chunks):
    usize, csize, cpos = struct.unpack_from('<III', fl, cio + c*0xC)
    chunks.append(zlib.decompress(fl[cdo+cpos:cdo+cpos+csize]))

def read_str(buf, pos):
    end = pos
    while end < len(buf) and buf[end] not in (0x0A, 0x00):
        end += 1
    return buf[pos:end].decode('latin-1', 'replace')

dat = open(datp, 'rb')
written = raw = comp = fail = 0
for i in range(fcount):
    fc, cn, pc = struct.unpack_from('<IHH', fl, 0xC + i*8)
    s = read_str(chunks[cn], pc)
    parts = s.split(':')
    pos = int(parts[0], 16); usize = int(parts[1], 16); csize = int(parts[2], 16)
    vpath = parts[3].strip() if len(parts) >= 4 else ""
    path = vpath if vpath else f"noPath/{fc:08x}.bin"
    if exts is not None:
        ext = os.path.splitext(path)[1].lower()
        if ext not in exts:
            continue
    dat.seek(pos * 0x800)
    blob = dat.read(csize)
    if usize == csize:
        data = blob; raw += 1
    else:
        try:
            data = zlib.decompress(blob); comp += 1
        except Exception as e:
            print(f"  FAIL {path}: {e}"); fail += 1; continue
    op = os.path.join(outdir, path)
    if os.path.exists(op):  # collision-safe (e.g. duplicate virtual paths)
        op = f"{op}.dup{fc:08x}"
    os.makedirs(os.path.dirname(op), exist_ok=True)
    open(op, 'wb').write(data)
    written += 1
    if written <= 30:
        print(f"  {path}  ({len(data)} bytes, {'raw' if usize==csize else 'zlib'})")
print(f"\nwritten={written} raw={raw} zlib={comp} fail={fail}")
