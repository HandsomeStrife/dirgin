#!/usr/bin/env python3
# decode_movie_audio.py - Dirge of Cerberus (PS2) FMV audio -> stereo WAV.
#
# The movie audio demuxed from the MPEG-2 Program Stream (private_stream_1 0xBD)
# is a PS2 "SShd"/"SSbd" streaming-audio container. Reverse-engineering (see the
# evidence below) shows this movie's body is LINEAR 16-bit signed little-endian
# PCM, stereo, 48000 Hz, block-interleaved at the SShd "interleave" granularity
# (0x200 bytes per channel): [L 0x200][R 0x200][L 0x200][R 0x200]...
#
# Evidence it is PCM, not SPU-ADPCM:
#   * body bytes / video duration = 23809024 / 124.0 s = 192000 B/s, which is
#     exactly 48000 * 2ch * 2 bytes (uncompressed). PS2 SPU-ADPCM would be ~3.5x
#     smaller (16 bytes -> 28 samples).
#   * reading the body as int16 gives lag-1 autocorrelation ~0.97 (a real
#     waveform); the "ADPCM control nibbles" are uniformly random and include
#     filter indices 5..15 that are invalid for SPU-ADPCM.
#   * deinterleaving at 0x200 blocks removes a periodic discontinuity that a
#     per-sample deinterleave leaves at every 256-sample boundary.
#
# SShd header (little-endian), magic at start of this movie's audio payload:
#   +0x00  'SShd'
#   +0x04  u32  header size (0x18)
#   +0x08  u32  version/flags (1)
#   +0x0C  u32  sample rate (48000)
#   +0x10  u32  channels (2)
#   +0x14  u32  interleave block size in bytes per channel (0x200)
#   +0x18  u32  loop start (-1 = none)
#   +0x1C  u32  loop end   (-1 = none)
# Then 'SSbd' + u32 body size, then the interleaved PCM body.
#
# Purist native-port tool: READ-ONLY on the input, fully bounds-checked.
# Run with:  python3 -I decode_movie_audio.py IN.sshd OUT.wav
import sys, struct, wave

def die(msg):
    sys.stderr.write("error: %s\n" % msg); sys.exit(1)

def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: decode_movie_audio.py IN.sshd OUT.wav\n"); sys.exit(2)
    data = open(sys.argv[1], 'rb').read()
    if len(data) < 0x2C:
        die("file too small")

    ph = data.find(b'SShd')
    if ph < 0 or ph > 0x1000:
        die("SShd header not found")
    (hsz, ver, rate, chans, interleave, loop0, loop1) = struct.unpack_from('<IIIIIII', data, ph + 4)
    if chans not in (1, 2):
        die("unsupported channel count %d" % chans)
    if interleave == 0 or interleave & 1:
        die("implausible interleave 0x%x" % interleave)
    if not (8000 <= rate <= 192000):
        die("implausible sample rate %d" % rate)

    pb = data.find(b'SSbd', ph)
    if pb < 0:
        die("SSbd body marker not found")
    body_size = struct.unpack_from('<I', data, pb + 4)[0]
    body_start = pb + 8
    avail = len(data) - body_start
    if body_size > avail:
        body_size = avail                      # tolerate truncated/padded tail
    body = memoryview(data)[body_start:body_start + body_size]

    sys.stderr.write(
        "SShd: hdr=0x%x ver=%d rate=%d chans=%d interleave=0x%x loop=(%d,%d)\n"
        % (hsz, ver, rate, chans, interleave, loop0, loop1))
    sys.stderr.write("SSbd: body=0x%x (%d bytes), avail=0x%x\n"
                     % (body_size, body_size, avail))

    # Deinterleave block-interleaved PCM into per-channel byte buffers.
    stride = interleave * chans                 # one full round = all channels
    nframes_round = len(body) // stride
    chan_buf = [bytearray() for _ in range(chans)]
    for r in range(nframes_round):
        base = r * stride
        for c in range(chans):
            s = base + c * interleave
            chan_buf[c] += body[s:s + interleave]
    # trailing partial round (keep whole 16-bit samples per channel)
    tail = len(body) - nframes_round * stride
    if tail >= interleave:
        base = nframes_round * stride
        got = 0
        c = 0
        while got + interleave <= tail and c < chans:
            s = base + got
            chan_buf[c] += body[s:s + interleave]
            got += interleave; c += 1

    # Equalize channel lengths to the shortest whole frame, re-interleave L/R/...
    import array
    chans_i16 = []
    for c in range(chans):
        a = array.array('h')
        b = bytes(chan_buf[c])
        b = b[:len(b) & ~1]                     # whole samples
        a.frombytes(b)
        if sys.byteorder == 'big':
            a.byteswap()                        # source is little-endian
        chans_i16.append(a)
    nper = min(len(a) for a in chans_i16)
    inter = array.array('h', bytes(2 * nper * chans))
    for c in range(chans):
        ac = chans_i16[c]
        inter[c::chans] = ac[:nper]

    out = sys.argv[2]
    w = wave.open(out, 'wb')
    w.setnchannels(chans); w.setsampwidth(2); w.setframerate(rate)
    pcm = inter
    if sys.byteorder == 'big':
        pcm = array.array('h', inter); pcm.byteswap()
    w.writeframes(pcm.tobytes())
    w.close()

    dur = nper / rate
    # stats
    peak = 0; sumsq = 0.0
    step = max(1, (nper * chans) // 2000000)    # subsample for speed
    cnt = 0
    for i in range(0, nper * chans, step):
        v = inter[i]; av = -v if v < 0 else v
        if av > peak: peak = av
        sumsq += float(v) * float(v); cnt += 1
    rms = (sumsq / cnt) ** 0.5 if cnt else 0.0
    sys.stderr.write("wrote %s: %d frames, %.3fs @ %dHz %dch, peak=%d rms=%.0f\n"
                     % (out, nper, dur, rate, chans, peak, rms))

if __name__ == '__main__':
    main()
