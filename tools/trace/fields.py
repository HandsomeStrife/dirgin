"""What the trace hook logs each frame, where it lives, and why we believe it.

Bases are resolved from the save state's EE memory the way the game resolves them
(resolve_bases), then each field is an absolute address copied per frame by the hook.
Confidence: D = decoded (code read, meaning clear), L = likely (code read, meaning
inferred), G = guess (layout seen in memory only). README.md has the same table.
"""
import struct

G_GAME = 0x46E290      # DAT_0046e290: game/world manager; +4 step, +0x40 camera, +0x44 view
G_OBJTAB = 0x46E880    # DAT_0046e880: object table owner (FUN_0053bb08)
STEP = 0x4EEB24        # DAT_004eeb24: logic step in ticks (FUN_00322f88)
DT = 0x4EEB28          # DAT_004eeb28: step in seconds = step / 60 (FUN_00322f88)
FLAGS = 0x46E298       # DAT_0046e298: script flags (FUN_00326298)
G_GSTATE = 0x46E294    # DAT_0046e294: game-state bit set (FUN_00326068 / FUN_00326018 / FUN_00326040)
G_AISCHED = 0x46F4EC   # DAT_0046f4ec: AI scheduler (FUN_00365dd8 creates, FUN_00365008 runs)
RETICLE = 0x471380     # DAT_00471380: reticle sprite object (FUN_003b0440 / FUN_003b0ac0)


def u32(mem, a):
    return struct.unpack_from("<I", mem, a & 0x1FFFFFF)[0]


def resolve_bases(mem):
    """Player P: FUN_0053bb08(DAT_0046e880) -> entry *(M+0x18) of the table at M+0x1164
    (FUN_0053e040) -> vtable+0x94 (here 0x579ae8, `move v0,a0`) -> FUN_00333150 calls
    vtable+0x9c (0x579b18, `move v0,a0`). Both getters return `this`, checked below."""
    m = u32(mem, G_OBJTAB)
    idx = u32(mem, m + 0x18)
    obj = u32(mem, u32(mem, m + 0x1164) + 4 * idx)
    vt = u32(mem, obj + 0x18)
    for slot, adj in ((0x94, 0x90), (0x9C, 0x98)):
        fn = u32(mem, vt + slot)
        if (struct.unpack_from("<h", mem, vt + adj)[0] != 0 or u32(mem, fn) != 0x03E00008
                or u32(mem, fn + 4) != 0x0080102D):        # jr ra; daddu v0,a0,zero
            raise RuntimeError("player getter at vtable+%#x is not `return this`" % slot)
    P = obj
    C = u32(mem, P + 0xE8)
    CAM = u32(mem, u32(mem, G_GAME) + 0x40)
    MP = u32(mem, C + 0x7C8)
    MP2 = u32(mem, MP + 0x3E8)
    if u32(mem, CAM + 0x54) != P:
        raise RuntimeError("camera +0x54 is not the player")
    HUD = u32(mem, u32(mem, G_GAME) + 0x44)     # view/HUD object (FUN_0033c060, FUN_0033c868)
    RET = u32(mem, RETICLE)                       # reticle sprite object (FUN_003b0440)
    BM = u32(mem, MP + 0x3F8)                     # bone matrices (FUN_005a87a0 -> FUN_005b2d58)
    bases = dict(P=P, C=C, CAM=CAM, MP=MP, MP2=MP2, HUD=HUD, RET=RET, BM=BM)
    if FIELDSET == "path":
        bases.update(find_enemies(mem))
    elif FIELDSET == "enemy":
        bases.update(find_enemies(mem))
        # game-state bit set: FUN_00326068(n) -> FUN_0036b610(DAT_0046e294, n) tests word n>>5
        # of *(*DAT_0046e294 + 4). Bit 6 set = AI scheduler paused (FUN_00365008 via FUN_00325b40).
        bases["GSB"] = u32(mem, u32(mem, u32(mem, G_GSTATE)) + 4)
        bases["AIQ"] = u32(mem, G_AISCHED)
        bases["PS"] = u32(mem, P + 0x17C)          # player status record (FUN_0055a370)
        bases["EVF"] = u32(mem, 0x47DBB0)        # DAT_0047dbb0: event-flag block (FUN_003ff658 / FUN_003ff738)
    else:
        bases.update(find_cape(mem))
    return bases


ENEMY_VT = 0x58E1F8    # enemy actor class vtable, set by the constructor FUN_00558790
ENEMY_MAX = int(__import__("os").environ.get("TRACE_ENEMIES", 7))   # active e027s in the sea-of-flames save (notes/enemy-research.md); TRACE_ENEMIES=2 traces #66 / #67 only (longer logs)


ERECS = [int(x) for x in __import__("os").environ.get("TRACE_ERECS", "").split(",") if x]


def find_enemies(mem):
    """Active enemy actors at load: object table *(M+0x1164)[0..255] (FUN_0053e040), class
    vtable ENEMY_VT, with a body (+0xe8), status (+0x17c, FUN_0055a370) and AI (+0x184), i.e.
    already activated by vtable +0x144 = FUN_00559748. Enemies activated later in the run have
    no body yet and are NOT traced (the hook copies fixed addresses)."""
    m = u32(mem, G_OBJTAB)
    tab = u32(mem, m + 0x1164)
    out = {}
    n = 0
    for i in range(256):
        o = u32(mem, tab + 4 * i)
        if not o or u32(mem, o + 0x18) != ENEMY_VT:
            continue
        if ERECS and i - 22 not in ERECS:      # TRACE_ERECS: stage records (object index = record + 22)
            continue
        C, ST, AI = u32(mem, o + 0xE8), u32(mem, o + 0x17C), u32(mem, o + 0x184)
        if not (C and ST and AI) or not u32(mem, o + 0x28) & 1:
            continue
        out.update({"E%d" % n: o, "EC%d" % n: C, "ES%d" % n: ST, "EA%d" % n: AI, "EI%d" % n: i,
                    "EM%d" % n: u32(mem, C + 0x7C8)})     # motion player, as the player's MP (C+0x7c8)
        n += 1
        if n == ENEMY_MAX:
            break
    if n == 0:
        raise RuntimeError("no active enemy actors in this state")
    for k in range(n, ENEMY_MAX):     # pad with the first enemy so the record layout is fixed
        for p in ("E", "EC", "ES", "EA", "EI", "EM"):
            out["%s%d" % (p, k)] = out["%s0" % p]
    return out


CAPE_N = 82            # cloth vertices in Vincent's cape block (p000_cloth.py)
CAPE_VERTS = (62, 63, 64, 73)   # free (unpinned) hem vertices, picked from the group list


def find_cape(mem):
    """Vincent's cape mesh block ("SM2", +0xe = 82 cloth vertices, +0x10 = 1 group).
    Found by signature, NOT through the game's pointer chain (the draw path reaches it as
    *(model+0x10)[i] in FUN_005b2ef8; that model pointer isn't traced yet). Runtime buffer
    +0x1c: velocity[n], position[n] at +n*16, target[n] at +n*32 (FUN_005b0d30 ->
    FUN_005b0b38(group, vel, pos, target)). Group +0 gravity, +8 damping."""
    hits = []
    for a in range(0x100000, 0x1FFF000, 16):
        if mem[a:a + 4] == b"SM2\0" and struct.unpack_from("<hh", mem, a + 0xE) == (CAPE_N, 1):
            hits.append(a)
    if len(hits) != 1:
        raise RuntimeError("expected one cape block, found %d" % len(hits))
    blk = hits[0]
    grp = u32(mem, u32(mem, blk + 0x24))
    if abs(struct.unpack_from("<f", mem, grp)[0] - 0.2) > 1e-6:
        raise RuntimeError("cape group gravity is not 0.2")
    return dict(CAPE=blk, CAPEBUF=u32(mem, blk + 0x1C))


# (name, base, offset, type, conf, source). Types: f32 / u32 / i32 / vec4 / mat4 / name8 /
# bytes:N. Every entry is copied as whole words; offsets are 4-aligned.
FIELDS = [
    ("step", None, STEP, "f32", "D", "FUN_00322f88 writes DAT_004eeb24 = vblank step x 1.2 (PAL), cap 5"),
    ("dt", None, DT, "f32", "D", "FUN_00322f88: DAT_004eeb28 = step x 0.016666668"),
    ("pad_held", None, 0x4E0FD8, "u32", "D", "FUN_00254638: lo16 = held (FUN_002584b0, pad+0x20), hi16 = pressed (pad+0x24); last frame's"),
    ("lstick", None, 0x4E0FE8, "bytes:8", "D", "FUN_00254638 -> FUN_00258218: left stick x,y floats, dead zone 0x30, scale 1/81; last frame's"),
    ("script_flags", None, FLAGS, "u32", "D", "FUN_00326298 tests DAT_0046e298 bits; bit 13 = physics-body walk (FUN_0034db80)"),
    ("p_state", "P", 0x100, "bytes:16", "D", "FUN_00333150: P+0x104 = player state byte (8 jump, 10 ladder...); P+0x100 u64 flag bits"),
    ("p_airshots", "P", 0x270, "u32", "D", "FUN_00575768 clears byte P+0x270 on take-off (air-shot counter)"),
    ("p_flags", "P", 0x29C, "bytes:16", "L", "FUN_00333150 tests P+0x2a8 bits 1/4; P+0x29c pointer"),
    ("matrix", "C", 0x00, "mat4", "D", "FUN_0034c388 writes row 3 (+0x30) = position from body+0x00; rows 0-2 = orientation (row 2 = facing, FUN_00575768 reads C+0x20)"),
    ("move_dir", "C", 0x90, "vec4", "D", "FUN_0034c388 writes the normalised move direction to C+0x90"),
    ("want_dir_speed", "C", 0xC0, "vec4", "D", "FUN_0034c388 mode 0xc: step = C+0xc0 (xyz dir) x C+0xcc (units/tick)"),
    ("mflags", "C", 0x1C0, "u32", "D", "C+0x1c0 bit 0x200 = on ground (FUN_003543b8, FUN_00575768), 0x2000/0x4000/0x400000 tested in FUN_0034c388"),
    ("mflags2", "C", 0x1C4, "u32", "D", "FUN_0034db80 sets bit 1, clears bit 4"),
    ("move_mode", "C", 0x1C8, "u32", "D", "FUN_0034db80 writes; FUN_0034c388 switches on it (10 walk, 0xb air, 0xc physics body, 0x10)"),
    ("mode_t", "C", 0x1CC, "f32", "L", "FUN_0034db80 zeroes; FUN_0034c388 path-follow time"),
    ("ground_speed", "C", 0x1D8, "f32", "D", "FUN_0034c388: C+0x1d8 = distance moved x 60 / step (units/s)"),
    ("body_pos", "C", 0x3B0, "vec4", "D", "FUN_0034c388: body = C+0x3b0, FUN_003590c8(body); C+0x30 copied from it"),
    ("body_10", "C", 0x3C0, "vec4", "L", "FUN_00357d48: body+0x10 vs body+0x00 y within 3.5 (previous/ground point)"),
    ("body_80", "C", 0x430, "vec4", "L", "FUN_0034c388 tests |C+0x430| == 0 (body+0x80; velocity-like)"),
    ("walk_poly", "C", 0x4C0, "u32", "D", "FUN_00357d48: body+0x110 = current walk-mesh polygon (handoff walk-mesh entry)"),
    ("anim_flags", "C", 0x564, "bytes:8", "D", "FUN_00344608/FUN_00340bb8 motion flags C+0x564; C+0x568"),
    ("motion", "C", 0x570, "name8", "D", "FUN_003449a0: C+0x570 = current motion (8-char clip name)"),
    ("motion_flags", "C", 0x580, "bytes:16", "D", "FUN_003449a0: C+0x580 = play flags, C+0x588 = -1; C+0x591 blend frames (FUN_00340bb8)"),
    ("mp_head", "MP", 0x440, "bytes:16", "D", "FUN_005a91c8: MP+0x440 flags, +0x448 head layer, +0x449 layer count"),
    ("mp_rate", "MP", 0x400, "bytes:16", "D", "FUN_005aa5b8 passes f32 MP+0x40c as the rate to FUN_005aa2a8 (frame += rate x weight/128 x DAT_004fc7e8); MP+0x408 = 1.0 when no blend"),
    ("mp_cloth", "MP", 0x430, "bytes:16", "D", "cloth settle window: MP+0x434 total, MP+0x438 remaining (FUN_005a95f0 arms, FUN_005aa5b8 decrements once per frame, FUN_005b1128 reads)"),
    ("cloth_k", None, 0x5B3B50, "f32", "D", "DAT_005b3b50 = (1 - rem/total)^2 or 1.0 (FUN_005b1128); FUN_005b2550 passes it to the cloth step FUN_005b0d30"),
    ("mp_layers", "MP", 0x200, "layers", "L", "FUN_005a91c8: 8 layers x 0x30 at MP+0x200: +0 clip, +8 partner clip, +0x18 frame (zeroed at start), +0x1d blend frames, +0x1e flags, +0x1f weight byte, +0x24 blend length"),
    ("mp2_head", "MP2", 0x440, "bytes:16", "L", "child motion player MP+0x3e8 (FUN_005a91c8 recurses into MP+1000..)"),
    ("mp2_layers", "MP2", 0x200, "layers", "L", "same layout as mp_layers"),
    ("cape_vel", "CAPEBUF", ("cape", 0), "cape", "L", "FUN_005b0b38: velocity of cape vertices 62,63,64,73 (v.y += gravity once per call; v = dpos x damping 0.1)"),
    ("cape_pos", "CAPEBUF", ("cape", 1), "cape", "L", "FUN_005b0b38/FUN_005b09f8/FUN_005b0c50: simulated position (world)"),
    ("cape_target", "CAPEBUF", ("cape", 2), "cape", "L", "FUN_005b0c50 pulls pos toward this skinned (keyed) target by max(k, blend)"),
    ("cam_matrix", "CAM", 0x60, "mat4", "L", "FUN_0032ace0: CAM+0x90 = eye (w set to 1.0 at the end), CAM+0x80 = forward (yaw from FUN_00260f00); rows +0x60 right, +0x70 up"),
    ("cam_a0", "CAM", 0xA0, "vec4", "G", "point with w=1 next to the eye (target?)"),
    ("cam_fov_pitch_yaw", "CAM", 0x170, "vec4", "L", "FUN_0032ace0: +0x170 divided by DAT_0046e3f0 (base FOV: 73.5 in RAM, ELF 65) -> FOV deg; +0x174 pitch (clamped to DAT_0046e3f8 = +50 deg in RAM (ELF 54) / DAT_0046e3fc = -54 deg); +0x178 yaw"),
]

# Opt-in set for aim/shoot research (TRACE_FIELDSET=aim): drops the cape, adds the weapon
# state (P+0x290..), the camera aim ray and view mode, the HUD reticle and the camera types.
# notes/aim-research.md explains each offset.
AIM_FIELDS = [f for f in FIELDS if f[0] in (
    "step", "pad_held", "p_state", "p_airshots", "matrix", "mflags", "move_mode",
    "ground_speed", "anim_flags", "motion", "motion_flags", "mp_head", "mp_layers")] + [
    ("p_face", "P", 0x118, "bytes:24", "D", "FUN_00562ef8: P+0x118 flags (bit 0/1), P+0x120 = face-toward point"),
    ("p_188", "P", 0x188, "bytes:8", "D", "FUN_00565ff0 sets/clears P+0x188 bit 38"),
    ("p_weapon", "P", 0x290, "bytes:48", "D", "FUN_00579570: +0x290 timer, +0x294 accumulator, +0x29c, +0x2a0, +0x2a4 state, +0x2a8 flags, +0x2b0 aim point (FUN_005784d8)"),
    ("cam_view", "CAM", 0x28, "bytes:8", "D", "FUN_00337338: CAM+0x28 camera type, +0x2c transition phase"),
    ("cam_typeptr", "CAM", 0x48, "u32", "D", "FUN_00337338: CAM+0x48 = camera type object"),
    ("cam_60", "CAM", 0x60, "bytes:128", "L", "CAM+0x60..0xdf: basis rows, +0x90 eye, +0xa0 aim hit, +0xb0 far point, +0xc0 hit normal (FUN_00329778)"),
    ("cam_f0", "CAM", 0xF0, "bytes:32", "L", "FUN_00329778 reads CAM+0xf0 (look-at point); FUN_0032a420 writes CAM+0x100"),
    ("cam_170", "CAM", 0x170, "bytes:192", "D", "FUN_0032ace0 / FUN_00329778 / FUN_0032a420: +0x170 FOV, +0x17c ramp, +0x184 hit dist, +0x1a0 shoulder, +0x1b0/+0x1b8 range, +0x1e0 fade, +0x1e4 target ids, +0x1e8 flags, +0x21c view mode, +0x224 kick"),
    ("hud", "HUD", 0x60, "bytes:80", "D", "FUN_0033c060 / FUN_0033c868: +0x60 reticle mode, +0x64 flags, +0x70 lock blend, +0x78/+0x7c offset, +0x90 target screen pos"),
    ("ret", "RET", 0x1B0, "bytes:96", "D", "FUN_003b0ac0: +0x1b0 kind, +0x1b1 flags, +0x1b2+k phase, +0x1bc+4k time, +0x1dc+2k length, +0x1ec/+0x1fc x/y"),
    ("camtype_c", None, 0x4EEBE0, "bytes:80", "D", "FUN_003314e0: camera type 0xc object at 0x4eebd0 (+0x10..)"),
    ("camtype_d", None, 0x4EEC40, "bytes:80", "D", "camera type 0xd object at 0x4eec30 (+0x10..)"),
    ("camtype_e", None, 0x4EECA0, "bytes:80", "D", "camera type 0xe object at 0x4eec90 (+0x10..)"),
]

# Opt-in set for body-pose research (TRACE_FIELDSET=pose): the motion player's bone
# matrices, 93 x 0x40 at *(MP+0x3f8). FUN_005a87a0 hands MP+0x3f8 to FUN_005b2d58 as the
# model's matrix buffer; FUN_005a9db0 -> FUN_005afa30/FUN_005afa58 fill it; FUN_005b06d0
# reads it (*(model+0x20) + bone*0x40) for the cape target. Model space, row-vector
# (row 3 = translation); checked against anim_rot.compose for h_st in the save state (0.01).
# 6.4 KB per record: set "log_from" so the log fits (about 60 frames).
POSE_BONES = 93
POSE_FIELDS = [f for f in FIELDS if f[0] in (
    "step", "pad_held", "matrix", "ground_speed", "mp_head", "mp_rate", "mp_layers", "cape_target")] + [
    ("bones", "BM", 0x0, "bytes:%d" % (POSE_BONES * 0x40), "D", "*(MP+0x3f8): bone matrices (FUN_005b2d58 / FUN_005b06d0), model space, row-vector"),
]

# Opt-in set for the follow camera (TRACE_FIELDSET=cam, worker 0089): FUN_0032ace0's state.
# CAM+0x170..0x22f (FOV, pitch +0x174, yaw +0x178, timers +0x1a8/+0x1e2, flags +0x1e8),
# the basis/eye (+0x60..+0xdf), look-at (+0xf0) and the normal type 0xc object.
CAM_FIELDS = [f for f in FIELDS if f[0] in (
    "step", "pad_held", "p_state", "matrix", "move_dir", "want_dir_speed", "ground_speed", "body_pos", "motion")] + [
    f for f in AIM_FIELDS if f[0] in ("cam_view", "cam_typeptr", "cam_60", "cam_f0", "camtype_c")] + [
    ("cam_170", "CAM", 0x170, "bytes:192", "D", "FUN_0032ace0: +0x170 FOV, +0x174 pitch, +0x178 yaw, +0x180 blend, +0x198/+0x19c, +0x1a8 auto-turn gain, +0x1ac, +0x1bc, +0x1c4/+0x1c8, +0x1e2 stick-idle counter, +0x1e8 flags, +0x214, +0x218"),
    ("cam_00", "CAM", 0x0, "bytes:96", "G", "CAM+0x00..0x5f: type +0x28, phase +0x2c, type object +0x48, player +0x54"),
    ("mp_matrix", "MP", 0x10, "mat4", "D", "MP+0x10: the draw matrix; FUN_003477c8 (camera base when C+0x564 & 0x18000000) = MP+0x10 x bone 0"),
    ("c_564", "C", 0x564, "u32", "D", "FUN_003422f0: C+0x564 & 0x18000000 picks the camera base (MP path vs C+0x54c vtable+0x24)"),
    # worker 0107 (jump camera): P+0x2a8 bit 0 gates FUN_00575768's FUN_0032a0f8(CAM, 1, 0)
    ("p_2a8", "P", 0x2A8, "u32", "D", "FUN_00574808 bits; FUN_00575768 bit 0 -> FUN_0032a0f8(CAM, 1, 0)"),
]

# Opt-in set for the jump (TRACE_FIELDSET=jump, worker 0095): no cape; the whole physics
# body (C+0x3b0..+0x4cf: state +0xa8, take-off t/y0/dy/v0/rise +0xd4..+0xe8, poly +0x110),
# the jump gates (FUN_00575698 / FUN_00577230) and the motion-state object pointer.
JUMP_FIELDS = [f for f in FIELDS if f[0] in (
    "step", "pad_held", "lstick", "p_state", "p_airshots", "matrix", "move_dir", "want_dir_speed",
    "mflags", "move_mode", "ground_speed", "body_pos", "motion", "motion_flags", "mp_head",
    "mp_layers", "cam_matrix")] + [
    ("body", "C", 0x3B0, "bytes:288", "D", "physics body C+0x3b0: +0xa8 state (FUN_00357860), +0xac flags, +0xd0 gravity, +0xd4 t, +0xd8 y0, +0xdc dy, +0xe0 v0, +0xe4 -h, +0xe8 rise (FUN_00357168)"),
    ("c_79c", "C", 0x79C, "u32", "D", "FUN_00575768: C+0x79c bit 0x20 = second jump used"),
    ("c_664", "C", 0x664, "u32", "D", "FUN_00575698: short C+0x664 must be 0, 1 or 2"),
    ("c_7d0", "C", 0x7D0, "u32", "D", "FUN_003419c0: *(C+2000) = motion-state object, its +0x70 = class id"),
    ("p_118", "P", 0x118, "u32", "D", "FUN_00577230 / FUN_005771c8: P+0x118 bits 0x200 / 0x40000"),
    ("p_2a8", "P", 0x2A8, "u32", "D", "FUN_00574808 bits; FUN_00575698 tests 8 and 0x10 in the air"),
    # M = *DAT_0046e880 = 0x128380 in the save state (worker 0131)
    ("m_gimmick", None, 0x128380 + 0x10AC, "u32", "D", "FUN_0053be88: M+0x10ac u16 = current gimmick id (0xffff none), chosen per frame by FUN_0053b500 -> FUN_0053bf30 -> FUN_00589978; the ✕ ladder check FUN_00544200 uses only it"),
]

# Opt-in set for enemy AI research (TRACE_FIELDSET=enemy, worker 0116): no cape; a little of
# the player, then the same block for each of the ENEMY_MAX actors found by find_enemies.
# notes/enemy-research.md §3 explains the objects.
def _enemy_fields(k):
    A, C, S, I = "E%d" % k, "EC%d" % k, "ES%d" % k, "EA%d" % k
    p = "e%d_" % k
    return [
        (p + "flags", A, 0x28, "u32", "D", "actor+0x28 bit 0 = active (FUN_00541c98, FUN_00558f68 test it before vtable +0x144)"),
        (p + "target", A, 0x118, "bytes:8", "D", "FUN_00563200: actor+0x118 bit 1 set -> target = object id u16 actor+0x11c (FUN_0053e040)"),
        (p + "face_pos", C, 0x20, "bytes:32", "D", "body C+0x20 = facing row, C+0x30 = position (same layout as the player's, FUN_0034c388)"),
        (p + "mflags", C, 0x1C0, "u32", "D", "C+0x1c0 move flags (as the player's); FUN_00361690 reads bit 0x100000"),
        (p + "move_mode", C, 0x1C8, "u32", "D", "C+0x1c8 move mode (FUN_0034c388)"),
        (p + "speed", C, 0x1D8, "f32", "D", "C+0x1d8 ground speed units/s (FUN_0034c388)"),
        (p + "motion", C, 0x570, "name8", "D", "C+0x570 current clip (FUN_003449a0)"),
        (p + "hp", S, 0x20, "u32", "D", "status+0x20 HP (FUN_0055c7f8 subtracts damage via FUN_003168c8)"),
        (p + "ai", I, 0x14, "bytes:48", "D", "AI +0x14 flags, +0x18 next think time, +0x20 interval/sub-state, +0x24 state, +0x34..+0x40 target distance/angle floats (FUN_00365110, FUN_00361690)"),
        (p + "ai_mode", I, 0xD0, "u32", "D", "AI +0xd0 brain mode (FUN_0035f300 / FUN_0035f268)"),
        (p + "ai_hit", I, 0x4C, "u32", "D", "AI +0x4c time of last hit taken (FUN_00360870, from FUN_0055db50)"),
        (p + "ai_hits", I, 0x70, "u32", "D", "AI +0x70 short = hits taken (FUN_00360870); +0x72 next"),
        # worker 0121 (combat AI, notes/enemy-research.md §9)
        (p + "ai_ammo", I, 0x7C, "u32", "D", "AI +0x7c short = rounds left (FUN_00360418 loads weapon +0x48; FUN_003605d8 -1 per shot); +0x7e"),
        (p + "ai_timer", I, 0xFC, "u32", "D", "AI +0xfc = fire-timer entry on the scheduler's 2nd list (FUN_003607a8 / FUN_00365478), 0 = not firing"),
        (p + "astate", A, 0x104, "u32", "D", "actor +0x104 byte = actor state (FUN_0055b8e0; 3 = gun up; 0x80-0xbf = attack, FUN_0055bb40)"),
        (p + "cflags", C, 0x564, "u32", "D", "C+0x564 motion flags: 0x4000 / 0x200000 tested by the shot FUN_003605d8; C+0x565 bit 7 = busy (FUN_0055ab08)"),
        # worker 0123 (why #66 takes no damage, notes/enemy-research.md §9.10)
        (p + "zone", A, 0x19C, "u32", "D", "actor +0x19c = hit-zone record (status +0xa8 -> +0x30 + part*16) set by the hit test FUN_0055bda8; byte 0 scales damage x0.05 (FUN_0055d2c0)"),
        (p + "part", A, 0x140, "u32", "D", "actor +0x142 byte = part index hit (FUN_0055bda8, from FUN_005ad118)"),
        (p + "st64", S, 0x64, "u32", "D", "status +0x64: bits 0/1 = no damage (FUN_0055f0e0 in FUN_0055db50), 0x20 (FUN_00317b68)"),
        (p + "ai_90", I, 0x90, "u32", "D", "AI +0x90 = shooter (+0x4c<<16 | +0x20) by FUN_003608d8; set only on the FUN_0055db50/0055e498 path, not the masked-part path FUN_0055df90"),
        (p + "pmask", C, 0x77C, "bytes:8", "D", "C+0x77c part mask; a hit on a set bit goes to FUN_0055df90 (hit counted, no damage) in FUN_0055bda8"),
    ]


ENEMY_FIELDS = [f for f in FIELDS if f[0] in ("step", "pad_held", "p_state", "matrix", "motion")]
ENEMY_FIELDS += [
    ("p_aim", "P", 0x2B0, "vec4", "D", "P+0x2b0 = weapon aim point = CAM+0xa0 (FUN_005784d8; notes/aim-research.md)"),
    ("cam_lock", "CAM", 0x1E4, "bytes:8", "D", "CAM+0x1e4 = locked actor id, CAM+0x1e8 bit 2 = lock mode (FUN_00328968)"),
    ("p_hp", "PS", 0x20, "u32", "D", "player status (P+0x17c, FUN_0055a370) +0x20 = HP (FUN_0055c7f8 subtracts damage; 780 in the save)"),
    ("gstate", "GSB", 0, "bytes:8", "D", "game-state bits 0..63 (FUN_0036b610); bit 6 = AI paused (FUN_00325b10, set by the event VM FUN_0037def0 / FUN_0037cf30)"),
    ("ai_now", "AIQ", 0, "u32", "D", "AI scheduler clock in ticks (FUN_00365008: += int(step) accumulated)"),
    ("evt_obj", None, 0x47DBB0, "u32", "D", "DAT_0047dbb0: event-flag block; FUN_0037e3c0 waits until flag N == value (FUN_003ff738)"),
    ("evt_flags", "EVF", 0x38, "bytes:64", "D", "event flags 0..63, stored XOR DAT_0047dbf0[n] (FUN_003ff658 writes, FUN_003ff738 reads)"),
    ("evt_key", None, 0x47DBF0, "bytes:64", "D", "DAT_0047dbf0: XOR key for evt_flags"),
    # worker 0124 (steer the aim onto #67): camera forward +0x80 and eye +0x90 (FUN_0032ace0)
    ("cam_fwd", "CAM", 0x80, "vec4", "L", "CAM+0x80 = camera forward (FUN_0032ace0); the aim ray runs eye + fwd * far (FUN_00329778)"),
    ("cam_eye", "CAM", 0x90, "vec4", "L", "CAM+0x90 = camera eye (FUN_0032ace0)"),
]
for _k in range(ENEMY_MAX):
    ENEMY_FIELDS += _enemy_fields(_k)
# Opt-in with TRACE_LOCK=1 (worker 0141, lock-on FUN_00328968 / FUN_00329778): the camera's
# lock state and the inputs of the picker, plus the per-soldier fields it reads. Off by default so
# the enemy record layout of the older traces stays the same.
if __import__("os").environ.get("TRACE_LOCK"):
    ENEMY_FIELDS += [
        ("cam_170", "CAM", 0x170, "bytes:192", "D", "CAM+0x170 FOV, +0x174/+0x178 view pitch/yaw, +0x190/+0x194 look stick, +0x1b0/+0x1b4/+0x1b8 lock range / cone / range, +0x1cc reticle angle, +0x1e4/+0x1e6 lock ids, +0x1e8 flags, +0x218 turn gain (FUN_00328968 / FUN_0032ace0)"),
        ("cam_a0", "CAM", 0xA0, "bytes:96", "D", "CAM+0xa0 aim point, +0xb0 far point, +0xc0 hit normal, +0xf0 look-at (FUN_00329778)"),
        ("p_8", "P", 0x8, "u32", "L", "P+8 = parent object (FUN_0054a720: 0 -> world position P+0x90); FUN_00577480's lead runs only when set"),
        ("p_272", "P", 0x270, "u32", "D", "P+0x272 short = aimed-at actor id (FUN_00573378)"),
        ("p_vel", "C", 0x410, "vec4", "D", "C+0x410 = (pos - last pos) / step (FUN_0034ea18, not in move mode 0xc)"),
        ("p_st64", "PS", 0x64, "u32", "D", "player status +0x64 bit 4 = no lock-on (FUN_0055f0e0(P, 4) in FUN_00328968)"),
        ("hud_60", "HUD", 0x60, "bytes:16", "D", "HUD+0x60 reticle mode, +0x64 flags (bit 0 lock option, 2 on target, 0x10 locked, 0x40 camera turn: FUN_0033c018)"),
        ("ret_1b0", "RET", 0x1B0, "bytes:64", "D", "RET+0x1b0 kind, +0x1b1 on-target/locked bits, +0x1b3.. phases, +0x1c0.. end times, +0x1de.. lengths (FUN_003b0ac0)"),
    ]
    for _k in range(ENEMY_MAX):
        _p = "e%d_" % _k
        ENEMY_FIELDS += [
            (_p + "a38", "E%d" % _k, 0x38, "u32", "D", "actor +0x38 probe bits (bit 2 = lockable: FUN_005449f0 probe 2, FUN_0055ef38)"),
            (_p + "par", "E%d" % _k, 0x8, "u32", "L", "actor +8 parent (FUN_00329778 adds 0.5 x C+0x410 only when set)"),
            (_p + "c764", "EC%d" % _k, 0x764, "u32", "D", "C+0x765 byte = lock bone (FUN_00340760 <- status +0xa8 +0x14; FUN_00563720)"),
            (_p + "vel", "EC%d" % _k, 0x410, "vec4", "D", "C+0x410 velocity per tick (FUN_0034ea18)"),
        ]

# Opt-in with TRACE_EMP=1 (worker 0149, enemy fire / blend clips): each enemy's motion player
# head and layer stack (MP = C+0x7c8, same layout as the player's mp_head / mp_layers).
if __import__("os").environ.get("TRACE_EMP"):
    for _k in range(ENEMY_MAX):
        _p = "e%d_" % _k
        ENEMY_FIELDS += [
            (_p + "mp_head", "EM%d" % _k, 0x440, "bytes:16", "D", "MP+0x440 flags, +0x448 head layer, +0x449 layer count (FUN_005a91c8)"),
            (_p + "mp_layers", "EM%d" % _k, 0x200, "layers", "L", "8 layers x 0x30 at MP+0x200 (as mp_layers)"),
        ]

# Opt-in with TRACE_PUSH=1 (worker 0154, actor push-out FUN_00542ab8): the push vector each actor
# gets (actor +0x130, cleared and summed by FUN_00542ab8), its cylinder and the flags the pair test reads,
# for the player and each traced soldier.
if __import__("os").environ.get("TRACE_PUSH"):
    ENEMY_FIELDS += [
        ("p_a28", "P", 0x28, "u32", "D", "actor +0x28 flags (FUN_00542ab8 tests 0x20 / 4)"),
        ("p_cyl", "P", 0x2C, "bytes:16", "D", "actor +0x2c radius, +0x30 height, +0x38 u16 (bit 9 = takes part in FUN_00542ab8)"),
        ("p_a118", "P", 0x118, "u32", "D", "actor +0x118 flags (0x8000000 / 0x4000000 / 0x10000000 / 0x20000 in FUN_00542ab8)"),
        ("p_push", "P", 0x130, "vec4", "D", "actor +0x130 = push vector summed by FUN_00542ab8"),
        ("p_c430", "C", 0x430, "vec4", "D", "C+0x430 = copy of actor +0x130 (FUN_00343160)"),
        ("p_c1c0", "C", 0x1C0, "u32", "D", "C+0x1c0 move flags"),
    ]
    for _k in range(ENEMY_MAX):
        _p = "e%d_" % _k
        ENEMY_FIELDS += [
            (_p + "cyl", "E%d" % _k, 0x2C, "bytes:16", "D", "actor +0x2c radius, +0x30 height, +0x38 u16"),
            (_p + "a118", "E%d" % _k, 0x118, "u32", "D", "actor +0x118 flags"),
            (_p + "push", "E%d" % _k, 0x130, "vec4", "D", "actor +0x130 push vector (FUN_00542ab8)"),
        ]

# Opt-in with TRACE_PUSHSTEP=1 (worker 0158, where the push enters Vincent's step): the move request
# C+0xc0 (dir, +0xcc speed), C+0x90 and the physics body's first 0xc0 bytes (+0x30 step from
# FUN_00358fb8, +0x40 velocity, +0x80 = C+0x430, +0xa8 state / +0xac flags; FUN_003590c8).
if __import__("os").environ.get("TRACE_PUSHSTEP"):
    ENEMY_FIELDS += [f for f in FIELDS if f[0] in ("move_dir", "want_dir_speed", "ground_speed")] + [
        ("body", "C", 0x3B0, "bytes:192", "D", "physics body C+0x3b0..+0x46f (FUN_003590c8)"),
    ]

# Opt-in set for soldier path walking (TRACE_FIELDSET=path, worker 0159; cmd 0x131 -> FUN_0055a6d8 ->
# FUN_0034edf0(C, 1), per frame FUN_0034ea18 -> FUN_0034f8c0 / FUN_0034fad0 and FUN_0034c388 move mode 9).
# TRACE_ERECS="46,47" picks the soldiers by stage record (object index = record + 22). Kept small so a
# long walk fits in the log: a little of the player and camera (to aim the wake shot), then per soldier the
# move state. C+0x1c0 block: +0x1c0 / +0x1c4 flags, +0x1c8 move mode, +0x1cc segment time, +0x1d0 duration,
# +0x1d4 constant speed, +0x1d8 ground speed, +0x1dc segment length, +0x1e0.. profile, +0x200 / +0x204
# polyline index / count, +0x208 arc position, +0x20c arc length, +0x210 tension, +0x214 turn mode, +0x218.
# C+0x300 block: +0x300 mover, +0x310 path, +0x314 node, +0x318 last node, +0x31c segment length,
# +0x320 length walked, +0x324, +0x328 path length, +0x32c.
PATH_FIELDS = [f for f in FIELDS if f[0] in ("step", "pad_held", "matrix")] + [
    ("cam_fwd", "CAM", 0x80, "vec4", "L", "CAM+0x80 = camera forward (FUN_0032ace0)"),
    ("cam_eye", "CAM", 0x90, "vec4", "L", "CAM+0x90 = camera eye (FUN_0032ace0)"),
    ("cam_lock", "CAM", 0x1E4, "bytes:8", "D", "CAM+0x1e4 = locked actor id (FUN_00328968)"),
    ("cam_a0", "CAM", 0xA0, "bytes:32", "D", "CAM+0xa0 aim point (ray hit), +0xb0 far point (FUN_00329778)"),
]
for _k in range(ENEMY_MAX):
    _A, _C, _I, _M = "E%d" % _k, "EC%d" % _k, "EA%d" % _k, "EM%d" % _k
    _p = "e%d_" % _k
    PATH_FIELDS += [
        (_p + "a100", _A, 0x100, "bytes:8", "D", "actor +0x100 u64: +0x104 byte actor state (FUN_0055b8e0), bits 40-41 gait (FUN_00565bd0)"),
        (_p + "ai_mode", _I, 0xD0, "u32", "D", "AI +0xd0 brain mode"),
        (_p + "cmat", _C, 0x0, "bytes:64", "D", "C+0x00 matrix rows (row 2 facing, row 3 = C+0x30 position; FUN_0034c388)"),
        (_p + "dir", _C, 0x90, "vec4", "D", "C+0x90 move direction (FUN_0034c388 mode 9: FUN_0025fdf0 tangent, normalised)"),
        (_p + "mv", _C, 0x1C0, "bytes:96", "D", "C+0x1c0..0x21f move state (see PATH_FIELDS)"),
        (_p + "path", _C, 0x300, "bytes:48", "D", "C+0x300..0x32f path mover (FUN_0034edf0 / FUN_0034fad0)"),
        (_p + "motion", _C, 0x570, "name8", "D", "C+0x570 current clip (FUN_003449a0)"),
        (_p + "c590", _C, 0x590, "bytes:8", "D", "C+0x590 posture bits 16-19 (FUN_003419e0), bit 24 gun out"),
        (_p + "mp_head", _M, 0x440, "bytes:16", "D", "MP+0x440 flags, +0x448 head layer, +0x449 count"),
    ]
if __import__("os").environ.get("TRACE_PATH_LAYERS"):
    for _k in range(ENEMY_MAX):
        PATH_FIELDS += [("e%d_mp_layers" % _k, "EM%d" % _k, 0x200, "layers", "L", "8 layers x 0x30 at MP+0x200")]

FIELDSET = __import__("os").environ.get("TRACE_FIELDSET")
if FIELDSET == "path":
    FIELDS = PATH_FIELDS
elif FIELDSET == "enemy":
    FIELDS = ENEMY_FIELDS
elif __import__("os").environ.get("TRACE_FIELDSET") == "jump":
    FIELDS = JUMP_FIELDS
elif __import__("os").environ.get("TRACE_FIELDSET") == "cam":
    FIELDS = CAM_FIELDS
elif __import__("os").environ.get("TRACE_FIELDSET") == "aim":
    FIELDS = AIM_FIELDS
elif __import__("os").environ.get("TRACE_FIELDSET") == "pose":
    FIELDS = POSE_FIELDS

LAYER_BYTES = 8 * 0x30


def size_of(t):
    if t in ("f32", "u32", "i32"):
        return 4
    if t in ("vec4", "name8"):
        return 16 if t == "vec4" else 8
    if t == "mat4":
        return 64
    if t == "layers":
        return LAYER_BYTES
    if t == "cape":
        return 16 * len(CAPE_VERTS)
    if t.startswith("bytes:"):
        return int(t[6:])
    raise ValueError(t)


def field_table(bases):
    out = []
    for name, base, off, t, _c, _s in FIELDS:
        if t == "cape":
            arr = bases[base] + off[1] * CAPE_N * 16
            out.extend((arr + 16 * k, 4) for k in CAPE_VERTS)
            continue
        addr = off if base is None else bases[base] + off
        out.append((addr, size_of(t) // 4))
    return out


def _name(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def decode_layers(b):
    out = []
    for i in range(8):
        r = b[i * 0x30:(i + 1) * 0x30]
        out.append(dict(clip=_name(r[0:8]), partner=_name(r[8:16]),
                        f10=struct.unpack_from("<f", r, 0x10)[0],
                        f14=struct.unpack_from("<f", r, 0x14)[0],
                        frame=struct.unpack_from("<f", r, 0x18)[0],
                        b1c=r[0x1C], blend_frames=r[0x1D], lflags=r[0x1E], weight=r[0x1F],
                        u20=struct.unpack_from("<I", r, 0x20)[0],
                        blend_len=struct.unpack_from("<f", r, 0x24)[0],
                        tail=r[0x28:0x30].hex()))
    return out


def decode_record(rec):
    """rec: bytes of one log record -> dict."""
    idx, w0, w1 = struct.unpack_from("<III", rec, 0)
    pad = struct.pack("<II", w0, w1)
    out = dict(frame=idx, pad_in=dict(buttons=(pad[2] << 8 | pad[3]) ^ 0xFFFF,
                                      rx=pad[4], ry=pad[5], lx=pad[6], ly=pad[7], mode=pad[1]))
    p = 12
    for name, _b, _o, t, _c, _s in FIELDS:
        n = size_of(t)
        b = rec[p:p + n]
        p += n
        if t == "f32":
            v = struct.unpack("<f", b)[0]
        elif t in ("u32", "i32"):
            v = struct.unpack("<I" if t == "u32" else "<i", b)[0]
        elif t == "vec4":
            v = list(struct.unpack("<4f", b))
        elif t == "mat4":
            v = [list(struct.unpack_from("<4f", b, 16 * r)) for r in range(4)]
        elif t == "name8":
            v = _name(b)
        elif t == "layers":
            v = decode_layers(b)
        elif t == "cape":
            v = {str(k): list(struct.unpack_from("<4f", b, 16 * i)) for i, k in enumerate(CAPE_VERTS)}
        else:
            v = b.hex()
        out[name] = v
    return out
