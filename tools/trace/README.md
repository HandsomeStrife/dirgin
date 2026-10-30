# tools/trace: per-frame gameplay oracle from the real game

The GS dumps check rendering; these traces check gameplay. `trace.py` boots PCSX2 headless
from the z128 (sea-of-flames) save state, plays a scripted pad input, logs the player,
motion, cape and camera every game frame, and writes JSONL. Traces are game-derived, so they
go to `tools/trace/out/` (ignored by the default-deny `.gitignore`). Never commit them.

```
python3 -I tools/trace/trace.py tools/trace/scripts/b_walk.json        # -> out/b_walk.jsonl
python3 -I tools/trace/summarize.py out/b_walk.jsonl --from 0 --to 60   # compact table
python3 -I tools/trace/summarize.py --diff out/b_walk.jsonl out/b_walk2.jsonl
```

## How it works

1. **Input injection (the least invasive way that is also deterministic).** The game reads
   port 0 once per frame in FUN_00257de0 with `jal 0x443b50` (scePadRead) at **0x257ebc**;
   the 32-byte buffer is the pad record at 0x4e1900 (FUN_00257a68, called from the per-frame
   pad update FUN_00254638). `hook.py` rewrites that one `jal` to a small MIPS routine at
   0x96000. The routine calls the real 0x443b50 first, so the IOP side is unchanged. It then
   copies 18 bytes from the script table over buffer +2..+19 (buttons active-low, RX RY LX
   LY, DualShock 2 pressure bytes) and appends one log record. Everything lives in EE RAM
   0x96000-0xfffff (zero in the state, below the ELF's 0x100000 load address), written into
   a copy of the .p2s **before** it is loaded. Input therefore starts on the first pad poll
   after the load, with no host timing involved.
   - Not used: xdotool key presses (documented in notes/pcsx2-headless-setup.md; host
     timing jitter, and the keyboard only gives full-deflection sticks); PINE writes to the
     pad buffer (they race the IOP DMA). PINE is used only to *read* the log back.
2. **Logging.** The hook copies a table of (address, words) pairs (`fields.py`) into a
   record per frame: frame index, the injected pad bytes, then the fields. The log is read
   over PINE when the frame counter at CTRL+4 reaches the script length. Records carry their
   index; `trace.py` rejects a log whose indices don't match.
3. **PCSX2.** v2.9.127 AppImage (extracted), OpenGL renderer on Mesa llvmpipe, under a
   private Xvfb (:97) started by `pcsx2_service.sh`. PCSX2 must run as a systemd user unit,
   because the Claude Code Bash tool kills a PCSX2 launched from a command (see the setup
   notes). The unit `~/.config/systemd/user/pcsx2trace.service` sets
   `XDG_CONFIG_HOME=<scratch>/cfg` (its own PCSX2.ini: BIOS folder, `EnablePINE = true`,
   `Renderer = 12`). It reads `PCSX2_DIR`, `ISO`, `STATEFILE` and `LOG` from
   `<scratch>/trace.env`; `trace.py` rewrites STATEFILE per run and starts and stops the
   unit. The unit file text is at the end of this README. The save state was written by
   PCSX2 2.8.2 (savestate version 0x9a59); 2.9.127 loads it.

Script format (`scripts/*.json`): `frames` (game frames = pad polls), optional `log_from`
(earlier frames get input but no record; the log area holds about 290 records), and
`events` `{from, to, buttons: [...], lx, ly, rx, ry}`, active on frames from <= f < to.
Sticks are -1..1 (up = ly -1). Buttons: cross circle square triangle l1 l2 r1 r2 l3 r3
start select up down left right.

## Timing (PAL SLES_541.85): measured, and decoded from the code

- **One record = one game frame = one 50 Hz vblank.** Every record of every run has
  `step` (DAT_004eeb24) = **1.2** ticks, i.e. 1 vblank per frame × 1.2 (FUN_0040ce28 /
  FUN_0040cb78). The PAL game ran at a constant 50 fps in all runs here, with no dropped
  frames. Emulated time = frame / 50 s; game ticks = Σ step = 1.2 × frame (60 ticks per
  second). The game keeps no tick counter that we know of: the "tick counter" in a trace is
  Σ step.
- Host speed doesn't matter. PCSX2 ran at about 7 game frames per host second here
  (llvmpipe), and the result is the same.

## Measured (sea-of-flames state, PAL; numbers from out/*.jsonl)

| quantity | game | how |
|---|---|---|
| frame rate / step | 50 fps, step 1.2 ticks every frame | `step`, all runs |
| run speed (full stick) | **60.0 u/s = 1.0 u/tick = 1.2 u per 50 Hz frame** | C+0x1d8 and position deltas, c_run |
| walk speed | **15.0 u/s = 0.25 u/tick = 0.30 u/frame**, at stick magnitude 0.21 AND 0.77 (walk/run is a threshold, not continuous) | b_walk, g_ladder_route |
| walk accel/decel | ±1.85 u/s per frame (0 → 15 in 8 frames); walk → run is an instant jump 14.8 → 60 when h_rn starts | b_walk, c_run |
| clip frame advance | `frame += rate × weight/128 × DAT_004fc7e8`, **FUN_005aa2a8**, called by the per-frame motion update **FUN_005aa5b8** | decoded; matches every record |
| rate (MP+0x40c) | **0.5** for h_st, h_rn, h_jp*, h_st_a/_f; h_wk scales with speed (0.4823 at 15 u/s) | mp_rate |
| → clip frames per real second | h_rn **30/s** (0.6 per 50 Hz frame, 0.5 per tick); h_wk 28.9/s; idle 30/s | mp_layers frame deltas |
| loop wrap | frame resets to exactly 0 when it reaches the clip length; the remainder is dropped (FUN_005aa2a8 loop branch) | h_wk 39.93 → 0.00 |
| cape | **cloth-simulated in a settle window, keyed outside it.** Window MP+0x434/0x438 armed at clip start (h_wk 60, h_rn 28, a facing turn 100), counts down **1 per frame (50/s)**. In the window the hem leaves its keyed target by up to 2.7-3.0 u; after it, pos == target exactly. **Not re-armed when a clip loops** (h_rn wrap at frame 48, h_wk at 153) | cape_pos/cape_target, mp_cloth |
| cloth step | FUN_005b2550 → FUN_005b0d30 → FUN_005b0b38 (integrate: v.y += 0.2 once per call, gated on step ≠ 0, NOT scaled by the step; v = Δpos × 0.1), FUN_005b09f8 ×4, FUN_005b0c50 (pull toward target). One step per drawn frame: 50/s on PAL. The sim is skipped when DAT_0047e1e0 ≥ 2 (frame-drop counter, FUN_0040ce28) unless k = 1 | decoded |
| standing jump | lift-off 2 frames after ✕ (state 8), apex **13.09 up** at +17 frames, 36 frames airborne; movement mode stays 0xc | e_jumps |
| running jump | the same arc; horizontal 1.2 u/frame (60 u/s) kept in the air; slides along walls while airborne | e_jumps (attempt 2) |
| fire (R1) | 1st press only raises the gun (state 3, h_st_a); later presses fire (h_st_f + _fu/_fd partners). Mid-air R1: air-shot counter P+0x270 → 1, **no hop** (apex unchanged) | h_fire |
| right stick | orbits the camera only, up to about 3.3°/frame; the character's heading doesn't change while standing | d_turn_circle |
| `ground_speed` caveat | reads the commanded speed (C+0xcc × 60) when blocked: 15.0 while pinned on a wall | d_turn_circle, f_railing |

Scripted runs (`scripts/`): a_idle, b_walk, b_walk2 (determinism), c_run, d_turn_circle,
e_jumps, f_railing, h_fire, g_ladder_route / g_ladder_route2. (g) was **not reached**: two
route attempts. The second froze the player at x = −1186 with the stick held (speed and clip
frozen, step still 1.2; probably a scripted story trigger), 76 units short of the ladder
bottom (−1110.4, −89.1, −1105.6).

## Determinism

`b_walk` run twice (same script, separate PCSX2 boots): **identical over all frames, every
logged field bit for bit** (`summarize.py --diff`). The trace is a pure function of
(state, script).

## Fields

Bases come from the save state, resolved the way the game resolves them
(`fields.resolve_bases`):

- **P (player) = 0x1339e0.** FUN_0053bb08(DAT_0046e880 = 0x128380): entry *(+0x18) = 22 of
  the table at *(+0x1164) (FUN_0053e040). Then vtable+0x94 (0x579ae8) and vtable+0x9c
  (0x579b18, the call in FUN_00333150) are both `jr ra; daddu v0,a0,zero`, so they return
  `this`. The resolver checks those two instructions.
- **C (character/body) = *(P+0xe8) = 0x919560.** FUN_00333150 and FUN_00575768 read it.
- **Body (physics, mode 0xc) = C+0x3b0**, passed to FUN_003590c8 by FUN_0034c388.
- **CAM = *(DAT_0046e290 + 0x40) = 0x95efe0.** FUN_00333150 and FUN_00326920; CAM+0x54 =
  P (FUN_0032ace0 line 209), which the resolver checks.
- **MP (motion player) = *(C+0x7c8) = 0x14d8180** (FUN_003449a0 → FUN_005a91c8).
  MP2 = *(MP+0x3e8) (child player; FUN_005a91c8 recurses into MP+1000..).
- **CAPE block = 0x18f9d60**, runtime buffer *(+0x1c) = 0x14eeec0. Found by signature
  (`SM2`, 82 cloth vertices, 1 group, group gravity 0.2), not through a game pointer chain:
  that chain (FUN_005b2ef8: *(model+0x10)[i]) isn't traced to a global yet.

Confidence: D = decoded (code read, meaning clear); L = likely (code read, meaning
inferred); G = guess (layout seen in memory only).

| field | address | type | conf | source |
|---|---|---|---|---|
| step | 0x4eeb24 | f32 | D | FUN_00322f88 writes DAT_004eeb24 = vblank step x 1.2 (PAL), cap 5 |
| dt | 0x4eeb28 | f32 | D | FUN_00322f88: DAT_004eeb28 = step x 0.016666668 |
| pad_held | 0x4e0fd8 | u32 | D | FUN_00254638: lo16 = held (FUN_002584b0, pad+0x20), hi16 = pressed (pad+0x24); last frame's |
| lstick | 0x4e0fe8 | bytes:8 | D | FUN_00254638 -> FUN_00258218: left stick x,y floats, dead zone 0x30, scale 1/81; last frame's |
| script_flags | 0x46e298 | u32 | D | FUN_00326298 tests DAT_0046e298 bits; bit 13 = physics-body walk (FUN_0034db80) |
| p_state | P+0x100 | bytes:16 | D | FUN_00333150: P+0x104 = player state byte (8 jump, 10 ladder...); P+0x100 u64 flag bits |
| p_airshots | P+0x270 | u32 | D | FUN_00575768 clears byte P+0x270 on take-off (air-shot counter) |
| p_flags | P+0x29c | bytes:16 | L | FUN_00333150 tests P+0x2a8 bits 1/4; P+0x29c pointer |
| matrix | C+0x0 | mat4 | D | FUN_0034c388 writes row 3 (+0x30) = position from body+0x00; rows 0-2 = orientation (row 2 = facing, FUN_00575768 reads C+0x20) |
| move_dir | C+0x90 | vec4 | D | FUN_0034c388 writes the normalised move direction to C+0x90 |
| want_dir_speed | C+0xc0 | vec4 | D | FUN_0034c388 mode 0xc: step = C+0xc0 (xyz dir) x C+0xcc (units/tick) |
| mflags | C+0x1c0 | u32 | D | C+0x1c0 bit 0x200 = on ground (FUN_003543b8, FUN_00575768), 0x2000/0x4000/0x400000 tested in FUN_0034c388 |
| mflags2 | C+0x1c4 | u32 | D | FUN_0034db80 sets bit 1, clears bit 4 |
| move_mode | C+0x1c8 | u32 | D | FUN_0034db80 writes; FUN_0034c388 switches on it (10 walk, 0xb air, 0xc physics body, 0x10) |
| mode_t | C+0x1cc | f32 | L | FUN_0034db80 zeroes; FUN_0034c388 path-follow time |
| ground_speed | C+0x1d8 | f32 | D | FUN_0034c388: C+0x1d8 = distance moved x 60 / step (units/s) |
| body_pos | C+0x3b0 | vec4 | D | FUN_0034c388: body = C+0x3b0, FUN_003590c8(body); C+0x30 copied from it |
| body_10 | C+0x3c0 | vec4 | L | FUN_00357d48: body+0x10 vs body+0x00 y within 3.5 (previous/ground point) |
| body_80 | C+0x430 | vec4 | L | FUN_0034c388 tests /C+0x430/ == 0 (body+0x80; velocity-like) |
| walk_poly | C+0x4c0 | u32 | D | FUN_00357d48: body+0x110 = current walk-mesh polygon (handoff walk-mesh entry) |
| anim_flags | C+0x564 | bytes:8 | D | FUN_00344608/FUN_00340bb8 motion flags C+0x564; C+0x568 |
| motion | C+0x570 | name8 | D | FUN_003449a0: C+0x570 = current motion (8-char clip name) |
| motion_flags | C+0x580 | bytes:16 | D | FUN_003449a0: C+0x580 = play flags, C+0x588 = -1; C+0x591 blend frames (FUN_00340bb8) |
| mp_head | MP+0x440 | bytes:16 | D | FUN_005a91c8: MP+0x440 flags, +0x448 head layer, +0x449 layer count |
| mp_rate | MP+0x400 | bytes:16 | D | FUN_005aa5b8 passes f32 MP+0x40c as the rate to FUN_005aa2a8 (frame += rate x weight/128 x DAT_004fc7e8); MP+0x408 = 1.0 when no blend |
| mp_cloth | MP+0x430 | bytes:16 | D | cloth settle window: MP+0x434 total, MP+0x438 remaining (FUN_005a95f0 arms, FUN_005aa5b8 decrements once per frame, FUN_005b1128 reads) |
| cloth_k | 0x5b3b50 | f32 | D | DAT_005b3b50 = (1 - rem/total)^2 or 1.0 (FUN_005b1128); FUN_005b2550 passes it to the cloth step FUN_005b0d30 |
| mp_layers | MP+0x200 | layers | L | FUN_005a91c8: 8 layers x 0x30 at MP+0x200: +0 clip, +8 partner clip, +0x18 frame (zeroed at start), +0x1d blend frames, +0x1e flags, +0x1f weight byte, +0x24 blend length |
| mp2_head | MP2+0x440 | bytes:16 | L | child motion player MP+0x3e8 (FUN_005a91c8 recurses into MP+1000..) |
| mp2_layers | MP2+0x200 | layers | L | same layout as mp_layers |
| cape_vel | CAPEBUF+vel[62,63,64,73] | cape | L | FUN_005b0b38: velocity of cape vertices 62,63,64,73 (v.y += gravity once per call; v = dpos x damping 0.1) |
| cape_pos | CAPEBUF+pos[62,63,64,73] | cape | L | FUN_005b0b38/FUN_005b09f8/FUN_005b0c50: simulated position (world) |
| cape_target | CAPEBUF+target[62,63,64,73] | cape | L | FUN_005b0c50 pulls pos toward this skinned (keyed) target by max(k, blend) |
| cam_matrix | CAM+0x60 | mat4 | L | FUN_0032ace0: CAM+0x90 = eye (w set to 1.0 at the end), CAM+0x80 = forward (yaw from FUN_00260f00); rows +0x60 right, +0x70 up |
| cam_a0 | CAM+0xa0 | vec4 | G | point with w=1 next to the eye (target?) |
| cam_fov_pitch_yaw | CAM+0x170 | vec4 | L | FUN_0032ace0: +0x170 divided by DAT_0046e3f0 (base FOV 65) -> FOV deg; +0x174 pitch (clamped +-54 deg); +0x178 yaw |

Notes on the log:
- A record is taken at the pad poll, **before** that frame's logic. Its state is the result
  of the previous frame; `pad_in` is the input this frame's logic will see. `pad_held` and
  `lstick` are the processed values from the previous frame (FUN_00254638 runs after the
  poll).
- Motion layers: `mp_head` byte 8 = head layer index (MP+0x448), byte 9 = layer count. The
  playing clip is `mp_layers[head]`.

## Undecoded or partial (stand-ins are labelled in the code)

- Camera: eye (CAM+0x90) and the basis rows are L; CAM+0xa0 is G; FOV (CAM+0x170 = 73.5 at
  the start, read with DAT_0046e3f0 = 65) is L. The render-side view object
  *(DAT_0046e290+0x44) isn't decoded.
- Layer fields +0x10, +0x14, +0x20 and +0x28.. have no decoded meaning (logged raw).
- MP2 (child motion player): role not decoded.
- Cape: the block is found by signature (see above).

## Unit file

```
[Unit]
Description=PCSX2 DoC trace oracle (dirge-port tools/trace)
[Service]
Type=simple
Environment=XDG_CONFIG_HOME=<scratch>/cfg
EnvironmentFile=<scratch>/trace.env
ExecStart=/srv/games/dirge-port/tools/trace/pcsx2_service.sh
KillSignal=SIGKILL
TimeoutStopSec=5
Restart=no
```

### Enemy set (TRACE_FIELDSET=enemy, worker 0116)

Logs the enemies that are active in the loaded state (`fields.find_enemies`, up to 7) as
`e<k>_*` fields; the header's `bases` gives each one's object-table index (`EI<k>`).
Enemies activated during the run are not traced (fixed addresses). See
notes/enemy-research.md §6. Script: scripts/k_enemy_engage*.json. k_enemy_engage7 (worker
0119) climbs the ladder and crosses trigger volume 95 (x -1074, y -146): e027 #66/#67 wake at
frame 784 (AI mode 2, target player) and fire; see §8 and tools/stage_events.py. k_enemy_engage12 (worker 0124) kills #66, sweeps the aim onto
#67 (new fields `cam_fwd`, `cam_eye`) and records its non-lethal hit and death (§10).

### Call logger (TRACE_CALLLOG, worker 0127)

`TRACE_CALLLOG=<set>` (sets in `calllog.py`: `bullet`, `spread`) wraps chosen game calls, either a
`jal` site inside a function or a vtable slot, with a stub. The stub calls the real function and logs
a0, a1, v0, f0, f12 and up to 25 words read through the argument pointers after the call. Entries
are only taken in `TRACE_CALLLOG_FRAMES=lo:hi` and are written to `<name>.calls.jsonl`. The log uses
the top 0x30000 bytes of the frame-log area, and scripts must be ≤ 1000 frames. Used for
notes/enemy-research.md §11 (scripts k_bullet1 / k_bullet2).
