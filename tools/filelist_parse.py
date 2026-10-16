import sys, struct, zlib

fl = open(sys.argv[1], 'rb').read()
chunk_info_off, chunk_data_off, file_count = struct.unpack_from('<III', fl, 0)
print(f"chunk_info_off=0x{chunk_info_off:x}  chunk_data_off=0x{chunk_data_off:x}  file_count={file_count}")

# File entries: start at 0xC, 8 bytes each
entries = []
for i in range(file_count):
    fc, chunk_no, pos_in_chunk = struct.unpack_from('<IHH', fl, 0xC + i*8)
    entries.append((fc, chunk_no, pos_in_chunk))

# Chunk info: array of {usize,csize,pos} between chunk_info_off and chunk_data_off
num_chunks = (chunk_data_off - chunk_info_off) // 0xC
print(f"num_chunks={num_chunks}")
chunks_dec = []
for c in range(num_chunks):
    usize, csize, cpos = struct.unpack_from('<III', fl, chunk_info_off + c*0xC)
    comp = fl[chunk_data_off + cpos : chunk_data_off + cpos + csize]
    try:
        dec = zlib.decompress(comp)
    except Exception as e:
        dec = b''
        print(f"  chunk {c}: zlib FAIL ({e}) usize={usize} csize={csize} cpos=0x{cpos:x}")
    chunks_dec.append(dec)
    if c < 3:
        print(f"  chunk {c}: usize={usize} declen={len(dec)} csize={csize} cpos=0x{cpos:x}")

def read_str(buf, pos):
    end = pos
    while end < len(buf) and buf[end] not in (0x0A, 0x00):
        end += 1
    return buf[pos:end].decode('latin-1', 'replace')

# Sample: first 12 entries + any with a path
print("\n=== sample info strings ===")
withpath = 0
for i,(fc,cn,pc) in enumerate(entries):
    if cn >= len(chunks_dec):
        continue
    s = read_str(chunks_dec[cn], pc)
    if i < 12:
        print(f"  [{i}] filecode=0x{fc:08x} chunk={cn} pos={pc}: {s}")
    if ':' in s and '/' in s:
        withpath += 1

print(f"\nentries total={len(entries)}  with-virtual-path={withpath}")
# extension histogram from paths
import collections
ext = collections.Counter()
for (fc,cn,pc) in entries:
    if cn >= len(chunks_dec): continue
    s = read_str(chunks_dec[cn], pc)
    parts = s.split(':')
    if len(parts) >= 4 and '/' in parts[3]:
        e = parts[3].rsplit('.',1)[-1] if '.' in parts[3] else '(noext)'
        ext[e] += 1
print("top path extensions:", ext.most_common(20))
