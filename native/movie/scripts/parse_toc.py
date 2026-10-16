import sys, struct

path = sys.argv[1]
with open(path, 'rb') as f:
    head = f.read(0x4)
    count = struct.unpack('<I', head)[0]
    print(f"File: {path}")
    print(f"Movie count: {count}")
    entries = []
    f.seek(0x4)
    for i in range(count):
        e = f.read(0x20)
        name, off, size = struct.unpack('<III', e[:12])
        keys = struct.unpack('<4H', e[0x18:0x20])
        reserved = e[0x0c:0x18]
        zero12 = b'\x00' * 12
        entries.append((name, off, size, keys, reserved))
        real_off = off * 0x800
        print(f"[{i:2}] hash=0x{name:08x} off=0x{off:08x} (*0x800=0x{real_off:010x}={real_off/1024/1024:8.2f}MB) "
              f"size=0x{size:08x}={size/1024/1024:7.2f}MB keys={[hex(k) for k in keys]} "
              f"resv_nonzero={reserved != zero12}")
    # Verify next-offset = prev_off + roundup(size,0x800)
    print("\n-- offset consistency check --")
    for i in range(count-1):
        cur = entries[i][1]*0x800
        size = entries[i][2]
        nxt = entries[i+1][1]*0x800
        pred = cur + ((size + 0x7ff) & ~0x7ff)
        print(f"[{i:2}] end=0x{cur+size:010x} rounded=0x{pred:010x} next=0x{nxt:010x} {'OK' if pred==nxt else 'GAP/PAD='+hex(nxt-pred)}")
    import os
    fsize = os.path.getsize(path)
    last = entries[-1]
    print(f"\nfile size = 0x{fsize:010x} ({fsize/1024/1024:.2f}MB); last entry end = 0x{last[1]*0x800+last[2]:010x}")
