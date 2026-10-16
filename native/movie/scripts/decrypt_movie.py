#!/usr/bin/env python3
# Port of DoCMovieTool decryption (Surihix/DoCMovieTool, MIT) for DoC:FFVII FMV archives.
# Decrypts a movie member (or a prefix of it) from an EU movie archive blob.
# Usage: decrypt_movie.py <blob> <entry_index> <out.pss> [max_mb]
import sys, struct

# EU region key array (128 uint32) from DoCMovieTool KeyArrays.KeysEU
KEYS_EU = [
2669815569,3052659657,766706709,3987631823,3118658383,211308469,1423027932,158596005,
1616973176,1961718482,2810754370,3122804681,1494785351,2985968630,1325779876,988857820,
1191673737,1778098628,2400730762,2298856559,1972302523,2570345645,2928718011,3619764100,
395257082,2672382752,3059513275,1231764582,3166542050,1023819232,984394817,4180810863,
501729753,2363504823,1828474199,963648756,2935508130,1474668935,498415289,1561817427,
1055917973,4179012385,3551142830,960370887,2440008075,3135203454,4015449212,1244816944,
4113253730,4292617671,3921550680,3765555576,3383389600,804436909,2773213268,667221909,
3087681998,434892806,3759109237,348173438,219396229,2670343087,3267284891,3265910381,
1564089057,954329196,827829049,3848739902,2994900334,3980806678,3084708858,655528765,
2881302710,3970939392,1273099093,680821770,260894660,589912329,26543045,1626733671,
1924216165,1503264536,3431130671,3010816606,755905303,931166523,2072361729,1637456757,
375501145,536307580,3726638596,1053439777,2851431788,2833802675,956085515,494168740,
4004820322,245659937,3620221224,1746842820,4069032164,2389500963,2439176476,1485101932,
2333601446,182442760,400867147,2581612772,200529041,256632961,3121035395,4112823312,
1097727789,1283032516,839658719,333421746,84568025,2783509018,2246859713,985793028,
1362470974,472967638,2730097232,786101873,1442881204,198197561,2131557805,309775922,
]

M16 = 0xFFFF
M32 = 0xFFFFFFFF

def main():
    blob = sys.argv[1]; idx = int(sys.argv[2]); outp = sys.argv[3]
    max_bytes = int(float(sys.argv[4]) * 1024 * 1024) if len(sys.argv) > 4 else None
    with open(blob, 'rb') as f:
        count = struct.unpack('<I', f.read(4))[0]
        f.seek(4 + idx * 0x20)
        e = f.read(0x20)
    name, off, size = struct.unpack('<III', e[:12])
    mult, k1, k2, k3 = struct.unpack('<4H', e[0x18:0x20])
    start = off * 0x800
    # how many bytes to decrypt (prefix allowed; stream is forward-only so prefix is valid)
    want = size if max_bytes is None else min(size, max_bytes)
    bytes_to_process = want - (want % 16)   # full 16-byte blocks
    leftout = want % 16
    print(f"entry {idx}: hash=0x{name:08x} start=0x{start:x} size=0x{size:x} keys={mult:#06x},{k1:#06x},{k2:#06x},{k3:#06x}")
    print(f"decrypting {want} bytes ({want/1048576:.2f} MB), blocks={bytes_to_process//16}")

    with open(blob, 'rb') as f:
        f.seek(start)
        data = bytearray(f.read(want))

    kai = 0
    pos = 0
    while pos < bytes_to_process:
        v1,v2,v3,v4 = struct.unpack_from('<4I', data, pos)
        # PrepKeyValues
        k1 = (k1 * mult) & M16
        k2 = (k2 * mult) & M16
        k3 = (k3 * mult) & M16
        ck1 = ((k1 << 16) | k2) & M32
        ck2 = ((k2 << 16) | k3) & M32
        struct.pack_into('<4I', data, pos,
                         v1 ^ ck1, v2 ^ ck2, v3 ^ ck1, v4 ^ ck2)
        a = KEYS_EU[kai // 2]
        b = KEYS_EU[kai // 2 + 1]
        mult ^= (a & M16)
        k1   ^= (a >> 16) & M16
        k2   ^= (b & M16)
        k3   ^= (b >> 16) & M16
        kai = (kai + 4) & 255
        pos += 16
    # trailing 4-byte block (only the real last-4 of the whole movie would set IsMovie)
    if leftout == 4 and max_bytes is None:
        v1 = struct.unpack_from('<I', data, pos)[0]
        k1 = (k1 * mult) & M16; k2 = (k2 * mult) & M16; k3 = (k3 * mult) & M16
        ck1 = ((k1 << 16) | k2) & M32
        struct.pack_into('<I', data, pos, v1 ^ ck1)

    with open(outp, 'wb') as f:
        f.write(data)
    print("wrote", outp, len(data), "bytes; first16:", data[:16].hex())

main()
