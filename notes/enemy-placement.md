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
  9.26 / 9.63 / 4.84 units from their placements (the opening event).
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

## Open

Event-actor table source; the AI's activation-time motion request (r_gst vs r_gst_lk); obj +0x44b;
records activated by event scripts (44-49, 66: cmd 0x108); e048 model for record 43.
