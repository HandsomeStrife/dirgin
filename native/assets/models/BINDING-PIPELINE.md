# Room texture-binding pipeline (z143 Mako Reactor)

How a Dirge of Cerberus room goes from raw disc data to a correctly-textured
native render, and how the pieces fit. This is the end-to-end flow behind the
"mangled textures" fix. See `notes/session-2026-10-15-gsdump.md` for the full
derivation and `FORMAT.md §6` for the format evidence.

## The problem

A room's geometry (`extract/_KEL/noPath/068f0N12.bin`, one BMD *chunk* per
sub-object) carries per-vertex position + UV + baked colour, but **no texture
binding**: the per-packet field once read as a material id is the vertex count,
and the runtime material model (group table → selector → material → texture
handle) is built in RAM at load time, not stored in the file. So which of the
room's ~18 TXD atlases (`068fffNN.bin`) textures each geometry packet is not
recoverable from the files alone (proven across files *and* a 32 MB EE-RAM dump).

## The recovered binding (authoritative, 52.3%)

Ground truth comes from GS dumps of the real game (PCSX2), reconciled by
**UV-triple identity matching**: a packet's (U,V) triples, taken as `raw/4096`,
equal the dump's `ST/Q` for the same surface, so each packet is matched to the
texture (FileCode) the GS bound. Unioning camera angles covers every
camera-visible surface: **8018 / 15332 packets (52.3%)**, 18 textures, across all
13 chunks. Output: `out/z143_pktfc/pf_<chunk>.json` (per-packet FileCode, decode
order) + `out/z143_room_uvtriple_binding.json` (per-chunk histograms). This is
the authoritative binding; it is saturated at the camera-visible ceiling.

## The back-face fill (cosmetic, to ~100%)

The remaining ~48% is geometry the retail game never rasterises (back-faces,
structural, far-LOD), so it has no ground truth anywhere. For the enhanced
free-camera, `backface_heuristic.py` fills each unbound packet from its nearest
bound packet (centroid 1-NN): **75.2% exact-match** (vs 26.3% baseline), ~98%
coverage. Output `out/z143_backface/assignments.json` tags each packet
`source=oracle|heuristic` + a `confident` flag. **Never** folded into the
authoritative 52.3% — it is a labelled cosmetic fallback only.

## Into the engine

```
 pf_<chunk>.json  +  (optional) assignments.json      dtex --info <FileCode>.bin
        │                                                        │
        ▼                                                        ▼
 emit_loader_manifest.py  ────────────────────────────── bakes w/h/psm/cpsm/clut
        │   (owns the decode_packets() packet order)
        ▼
 <chunk>.manifest   (line-based: per-slot FileCode+metadata, per-packet slot)
        │
        ▼
 material_loader.c  (engine/scene) — SYNTHESIZES a DocMaterialModel +
        │            DocTextureContext that reproduces the binding:
        │              • one group per FileCode (sharded if >512 packets),
        │                selector → material → single mode-0 layer, tag == slot
        │              • default group (mode-8) for unbound → untextured
        │              • dense texture handles carrying the baked GS metadata
        ▼
 material.c  (UNCHANGED reauthored consumer) — doc_material_resolve_texture(
        │     model, ctx, draw_word) → DocTextureDesc{slot,w,h,psm,cpsm,…}
        ▼
 test_material_loader.c — asserts the resolved per-slot histogram == the
                          recovered binding for all 13 chunks (15332 packets,
                          8018 matched) + texture metadata parity. Mutation-tested.
```

The Python bake owns the fragile VIF packet enumeration; the C loader is
dependency-free (reads a text manifest). Keeping `material.c` untouched means the
loader is validated *against* the already-oracle-tested consumer.

## Rendering the whole room

`export_room_bound.py` unions all 13 chunks (they share one world frame), merges
packets globally by FileCode (18 textures + 1 unbound group), and emits DMSH
meshes + a `.list` for `native/gpu/render_env_textured` (EGL + software GL works
headless). `--heuristic` adds the confident back-face fill.

```
python3 -I export_room_bound.py              -o out/z143_room/room_oracle   # 52.9% tris
python3 -I export_room_bound.py --heuristic  -o out/z143_room/room_heur     # 99.3% tris
render_env_textured -l out/z143_room/room_heur.list -o <render-dir>
```

## Reproduce from scratch

```sh
cd native/assets/textures && make                      # build libdtex + dtex CLI
cd ../models
for pf in out/z143_pktfc/pf_*.json; do                 # bake all 13 manifests
  cid=$(basename "$pf" .json); cid=${cid#pf_}
  python3 -I emit_loader_manifest.py \
    ../../../extract/_KEL/noPath/$cid.bin "$pf" -o out/z143_loader/$cid.manifest
done
cd ../../engine && cmake -S . -B build && cmake --build build && ctest   # 11/11, incl. loader parity
```

## Known limitation — scene placement

The 13 chunks are **not** all pre-placed in world space: several are authored at
the origin and overlap when naively unioned, so the whole-room render is
approximate. Per-chunk world placement is runtime-set (the `payload+0x1990`
transform is a stub), recoverable only from a scene-loaded EE-RAM capture's scene
nodes (node+0x18 model, node+0x28 transform) or by reversing the scene-setup
code — the same runtime-data shape as the binding itself. Tracked as blocker #3.
