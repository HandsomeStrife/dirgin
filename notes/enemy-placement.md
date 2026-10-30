# Enemy placement on the page (worker 0118, 2026-10-29)

Enemy job 2: z128's stage-load soldiers on play.html, standing idle. Code:
`native/assets/models/export_web_placements.py` (build step `enemy_placements` in
tools/byo/build_web.sh -> gitignored `native/web/data/enemies/placements.json`),
`native/web/enemies.js` (play.js hook; `?enemies=0` = off).

## Which placements (decoded rule, one GUESS input)

FUN_00324e70 -> **FUN_00319190**: for each of 256 statuses, if the placement flags (status +0xc =
section-1 +0x40) have bit 3 AND **FUN_00376938** finds the placement tag (+0x44, via FUN_00316d40)
in the event-actor table (DAT_0046f850 + 0x1800 + k*0x200, 64 entries, name at +0xa5), it runs
FUN_003159b8 (status +0x60 |= 0x41). FUN_005453b0 -> FUN_00558f68 then activates it (+0x144).
So flag bit 3 alone is not enough: 59 records have it, but the save state's table holds only
`e_Sol_11, e_Sol_12, e_Sol_13, w_Gun_11..13, p_Vin` -> records 0, 41, 42, 43 load. 43 is e048,
so the page draws **records 41 and 42** (e027). The table is the event system's actor list
(FUN_003772c8 / FUN_00376308 / FUN_00377b40); which event file fills it is **not decoded**
(two attempts). Stand-in: `EVENT_ACTORS` in the exporter (GUESS).

## Matrix (decoded, exact)

FUN_00315050 copies record +0xc / +0x1c / +0x2c to status +0x30 / +0x40 / +0x50. FUN_0055a168
builds the actor matrix (FUN_0054b770 -> actor +0x60): row0 = (+0x2c) x (+0x1c), row1 = +0x2c
(always (0,-1,0) in z128), row2 = +0x1c (facing), row3 = position. Body C+0x00 carries the same
matrix. Page: heading = atan2(face.x, face.z), rotation.y = heading + pi (play.js bodyYaw).

## Checks

- Save state, actor +0x60 and body C vs the placement matrix, the 7 idle soldiers (records
  44-49, 66): rotation 0.0, position 0.000.
- Records 41 / 42 / 43 (load-activated) are dead in the save state and have moved
  9.26 / 9.63 / 4.84 units from their placements. Not an opening event: see "Dead at the start" below.
- Hero GS dump 163235 (e027 triangles unprojected, w = 1/q): the one var-0 e027 is record 42's
  body lying flat (y -142.8..-139.5), 11.7 units (xz) from its placement. The five var-2 ones
  are records 44, 45, 46, 47, 66: bbox centre vs placement 0.23-0.71 units xz, feet y <= 0.03.
- Page (enemies.js) draws 41 / 42 exactly at their placement (no transform beyond the matrix).
- Lighting: hero dump e027 vertex colours (45,49,61) = program D ambient x 122/128 and
  (48,52,64) = x 128/128 (rec 66, MP+0x450 = 128), so enemies take Vincent's rig (env slot 0,
  MP+0x44e = 0 in the save). Light 2 at records 41/42's actor points: the grid pick finds none.
- Save state, idle e027 motion players: MP+0x40c = 0.5, MP+0x408 = 1.0, +0x44b = 2.
  SCALE_BYTE (MP+0x450 = 122) and ACTOR_CENTRE are read from these idle soldiers (records 44-49,
  66), not from 41 / 42 (dead in the save state).
- Up row: every z128 placement has up = (0,-1,0), so the page uses only the face as a yaw;
  enemies.js warns if a placement's up differs.

## Draw order (worker 0120)

FUN_005a8e28 draws every motion player in the list at DAT_005b3788 in link order (MP+0x428 bit 8
clear; FUN_005a8ec0 then draws the bit-8 ones). FUN_005a7978 links a new MP right after the
head, so newer actors draw before older ones (the head first). Hero dump 163235 (enemy_dump_check
`draw order`): event soldiers 44-47 / 66 (seq 56322-59261, far to near) -> Vincent (59465-62977)
-> record 42 (64378-66678). Record 42's MP is older than Vincent's, so the page's order (enemies
added after vin.body, sortObjects = false) is the game's. 41 vs 42: not traced (GUESS).

Screenshot (worker 0118 scratchpad): enemies_vs_game.png = hero dump render | page at the start
deck with the dump's e027 triangles in cyan.

## Dead at the start (worker 0126)

The page starts at the save state: SPAWN (-1395.63, -1110.51) is the hero-dump placement, 0.7 u
from the save's Vincent MP (-1395.15, -98.64, -1111.01); the traces start from the save. Records
41-43 (objects 63-65 = record + 22) in the save's RAM: status +0x20 HP **0**, actor +0x28 bit 0
**clear** (inactive), body +0xe8 = 0, AI +0x184 = 0, MP +0x1a0 = 0, no motion player near the deck
in the DAT_005b3788 list, actor state +0x104 = 7 (dead), +0x118 = 0x103 (bit 0x100 = corpse
settled, FUN_00539570 event 0x36), hit part +0x142 = 6 / 10 / 4 and zone +0x19c set (FUN_0055bda8):
they were **shot**. Their scripts 105-107 (event 3, run at activation) send 0x193 (target the
player) + 0x143 (AI mode 1 or 2): they are the start deck's guards, alive and in combat at stage
load, killed in play before the save. Event flags (DAT_0047dbb0 +0x38) are all 0, so script 0
event 0x22's flag-0 == 2 block that deactivates records 41-54 (cmd 0x130, @919-1043) did not run.
FUN_00559748 returns before building a body when the status is dead (FUN_00316cf8), so nothing
brings one back. **Page: not drawn** (export_web_placements.py `SAVE_STATE_DEAD`, placements.json
`dead`, enemies.js filter); the page draws the AI's records 44 / 45 only.

Actor +0x60 rows 3 in the save: 63 (-1395.59, -140, -1029.76), 64 (-1405.26, -140, -1030.94),
65 (-1386.16, -141.45, -1028.08); up row (0,-1,0) for all (lying bodies keep an upright actor matrix).
Hero dump 163235 (earlier than the save): the var-0 e027 drawn there is object 64 (record 42):
corner mean (-1405.23, -140.72, -1029.12) vs actor 64's position, 1.8 u (body length). It is
off-screen (drawable x -119..-61, guard band only). 41 and 43 have no body even then (63 / 65
project at drawable x -31 / +8; none of their triangles are sent). Pose fit to that corpse
(enemy_dump_check.py, similarity over all 2427 corners, best frame): r_stlaf0 f15 RMS 1.52 u,
r_stbf0 f24 2.40, r_stlld0 f5 2.76, r_gst f117 1.92; so r_stlaf0 is the likeliest clip (L).

Not decoded (two attempts: FUN_00558f68, FUN_00542ab8 and the +0x118 bit-0x100 readers): what
deactivates a settled corpse between the dump and the save (record 42's went; 41 / 43's earlier).

## Open

Event-actor table source; the AI's activation-time motion request (r_gst vs r_gst_lk); obj +0x44b;
records activated by event scripts (44-49, 66: cmd 0x108); e048 model for record 43.
