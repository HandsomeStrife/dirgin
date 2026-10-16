#!/usr/bin/env python3
# Demux PS2 PSS private_stream_1 (0xBD) audio payloads into one raw stream.
# The stream carries an SShd header + SSbd body (PS2 ADPCM). Usage: demux_audio.py <in.pss> <out.bin>
import sys, struct
data = open(sys.argv[1], 'rb').read()
out = bytearray()
i = 0; n = len(data); npkt = 0
while i < n - 6:
    if data[i] == 0 and data[i+1] == 0 and data[i+2] == 1 and data[i+3] == 0xBD:
        plen = struct.unpack('>H', data[i+4:i+6])[0]
        # MPEG-2 PES: byte i+6 flags10, i+7 flags, i+8 pes_hdr_data_len
        hdrlen = data[i+8]
        payload_start = i + 9 + hdrlen
        payload_end = i + 6 + plen
        out += data[payload_start:payload_end]
        npkt += 1
        i = i + 6 + plen
    else:
        i += 1
open(sys.argv[2], 'wb').write(out)
# report SShd header if present
p = out.find(b'SShd')
info = ""
if p >= 0:
    hsz, _flag, srate, chans, interleave = struct.unpack('<IIIII', out[p+4:p+24])
    info = f" SShd: hdr=0x{hsz:x} rate={srate} chans={chans} interleave=0x{interleave:x}"
print(f"packets={npkt} audio_bytes={len(out)}{info}")
