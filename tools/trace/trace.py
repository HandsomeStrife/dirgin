#!/usr/bin/env python3
"""Per-frame gameplay trace from the real game (PCSX2), driven by a scripted pad.

    python3 -I tools/trace/trace.py tools/trace/scripts/b_walk.json [--out DIR] [--state P2S]

Builds a patched copy of the z128 save state (hook.py), starts the `pcsx2trace` systemd
user unit on it, waits (over PINE) until the script has played, reads the log out of EE
memory and writes DIR/<script name>.jsonl (one JSON object per game frame). See README.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fields  # noqa: E402
import hook  # noqa: E402
from p2s import read_p2s, write_p2s  # noqa: E402

DEFAULT_STATE = "/srv/shared/dirge-port/save-states/sea-of-flames.p2s"
DEFAULT_OUT = os.path.join(HERE, "out")       # gitignored (default-deny .gitignore)
UNIT = "pcsx2trace"

# Pad byte 2/3 bit numbers (libpad; active low). byte 2 = bits 0..7, byte 3 = bits 8..15.
BUTTONS = {"select": 0, "l3": 1, "r3": 2, "start": 3, "up": 4, "right": 5, "down": 6, "left": 7,
           "l2": 8, "r2": 9, "l1": 10, "r1": 11, "triangle": 12, "circle": 13, "cross": 14,
           "square": 15}
# DualShock 2 pressure bytes (pad +8..+19), as offsets into our 18-byte payload (+2..+19).
PRESSURE = {"right": 6, "left": 7, "up": 8, "down": 9, "triangle": 10, "circle": 11,
            "cross": 12, "square": 13, "l1": 14, "r1": 15, "l2": 16, "r2": 17}


def stick_byte(v):
    """-1..1 -> 0..255 with 0 at 0x80 (libpad centre)."""
    return max(0, min(255, int(128 + 127.5 * v)))


def build_frames(script):
    n = int(script["frames"])
    frames = []
    for f in range(n):
        held, lx, ly, rx, ry = set(), 0.0, 0.0, 0.0, 0.0
        for ev in script.get("events", []):
            if ev["from"] <= f < ev["to"]:
                held |= set(ev.get("buttons", []))
                lx = ev.get("lx", lx)
                ly = ev.get("ly", ly)
                rx = ev.get("rx", rx)
                ry = ev.get("ry", ry)
        bits = 0
        for b in held:
            bits |= 1 << BUTTONS[b]
        bits ^= 0xFFFF
        pl = bytearray(18)
        pl[0], pl[1] = bits & 0xFF, bits >> 8
        pl[2], pl[3], pl[4], pl[5] = stick_byte(rx), stick_byte(ry), stick_byte(lx), stick_byte(ly)
        for b in held:
            if b in PRESSURE:
                pl[PRESSURE[b]] = 0xFF
        frames.append(bytes(pl))
    return frames


def systemctl(*a):
    return subprocess.run(["systemctl", "--user", *a], capture_output=True, text=True)


def run(script_path, out_dir, state, scratch, timeout_s):
    script = json.load(open(script_path))
    name = script["name"]
    mem = read_p2s(state)
    ee = bytearray(mem["eeMemory.bin"])
    bases = fields.resolve_bases(ee)
    table = fields.field_table(bases)
    frames = build_frames(script)
    log_from = int(script.get("log_from", 0))
    clog = os.environ.get("TRACE_CALLLOG")     # optional call logger (calllog.py)
    if clog:
        import calllog
        log_end = hook.LOG_END
        hook.LOG_END = log_end - calllog.CLOG_BYTES
    rec = hook.patch_memory(ee, frames, table, log_from)
    # TRACE_POKE="addr=word,addr=word" (hex): static EE writes before the state loads (experiments:
    # zero a constant, NOP an instruction). Each old word is printed so the run can be described.
    for item in filter(None, os.environ.get("TRACE_POKE", "").split(",")):
        a_, w_ = (int(x, 16) for x in item.split("="))
        print("poke %#x: %#010x -> %#010x" % (a_, struct.unpack_from("<I", ee, a_)[0], w_))
        struct.pack_into("<I", ee, a_, w_)
    if clog:
        hook.LOG_END = log_end
        lo, hi = (int(x) for x in os.environ.get("TRACE_CALLLOG_FRAMES", "0:100000").split(":"))
        cstart, _, chooks = calllog.patch(ee, clog, lo, hi, log_end)
    os.makedirs(scratch, exist_ok=True)
    tstate = os.path.join(scratch, "trace_%s.p2s" % name)
    write_p2s(tstate, {"eeMemory.bin": bytes(ee)}, state)

    env = os.path.join(scratch, "trace.env")
    lines = [l for l in open(env).read().splitlines() if not l.startswith("STATEFILE=")]
    lines.append("STATEFILE=" + tstate)
    open(env, "w").write("\n".join(lines) + "\n")

    systemctl("stop", UNIT)
    sock = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "pcsx2.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    t_start = time.time()
    r = systemctl("start", UNIT)
    if r.returncode != 0:
        raise RuntimeError("systemctl start failed: " + r.stderr)
    from pine import Pine
    p = None
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            p = p or Pine()
            if p.read32(hook.CTRL + 4) >= len(frames):
                break
        except (OSError, IOError):
            p = None
        time.sleep(0.5)
    else:
        systemctl("stop", UNIT)
        raise RuntimeError("timed out waiting for the script to finish")
    wall = time.time() - t_start
    nlog = len(frames) - log_from
    raw = p.read_block(hook.LOG, ((rec * nlog + 7) // 8) * 8)
    if clog:
        wp = p.read32(calllog.CLOG_CTRL + 8)
        craw = p.read_block(cstart, max(8, wp - cstart)) if wp > cstart else b""
    systemctl("stop", UNIT)

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, name + ".jsonl")
    with open(path, "w") as fh:
        hdr = dict(meta=dict(script=name, frames=len(frames), log_from=log_from, record_bytes=rec,
                             bases={k: hex(v) for k, v in bases.items()},
                             host_wall_s=round(wall, 2), state=os.path.basename(state),
                             note="one record per pad poll (= per game frame); logged before "
                                  "this frame's logic, so state is the previous frame's result"))
        fh.write(json.dumps(hdr) + "\n")
        for i in range(nlog):
            d = fields.decode_record(raw[i * rec:(i + 1) * rec])
            if d["frame"] != i + log_from:
                raise RuntimeError("log record %d is corrupt (index %d)" % (i, d["frame"]))
            fh.write(json.dumps(d) + "\n")
    if clog:
        with open(os.path.join(out_dir, name + ".calls.jsonl"), "w") as fh:
            fh.write(json.dumps(dict(meta=dict(set=clog, frames=[lo, hi], hooks=[
                dict(id=h["id"], name=h["name"], sites=h["sites"]) for h in chooks]))) + "\n")
            for e in calllog.decode(craw, chooks):
                fh.write(json.dumps(e) + "\n")
    os.unlink(tstate)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scripts", nargs="+")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--state", default=DEFAULT_STATE)
    ap.add_argument("--scratch", default=os.environ.get("TRACE_SCRATCH", "/tmp/dirge-trace"))
    ap.add_argument("--timeout", type=float, default=600)
    a = ap.parse_args()
    for s in a.scripts:
        print(run(s, a.out, a.state, a.scratch, a.timeout))


if __name__ == "__main__":
    main()
