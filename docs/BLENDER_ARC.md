# BLENDER_ARC — VotvIO: the Blender 5.1 addon that imports a VOTV .sav into a full scene

> Living doc for the VotvIO arc. Started 2026-08-29 (user ask, verbatim: «Нужен аддон для блендера 5.1,
> который позволит импортировать .sav сохранение Voices Of The Void — далее аддон прогрузит всю карту и
> все ассеты, которые валяются на карте, все модели и все entity, все npc»; precedent bar named by the
> user: SourceIO — self-contained, no manual export steps). Addon name **VotvIO** (user's pick).
> Design of record: `research/findings/tooling/votv-blender-sav-importer-DESIGN-2026-08-29.md` (local-only,
> like all research/ pointers) + its QF transcript sibling. Status: **P0+P1+P2 BUILT the same day**
> (commits `[blender] VotvIO P0+P1` + `P2`), deployed to `extensions/user_default/votvio` and enabled.

## 0. AS-BUILT (2026-08-29, headless smoke on the real s_1234.sav + untitled_1)

- `.sav` (20.3 MB) parses in **0.4 s**; full import **~106 s cold**: **74,492 objects** — 5,091 save
  props with real meshes, 2,172 map statics, **64,823 ISM/HISM/foliage instances** (native-tail parser,
  self-validating scan anchored on `NumBuiltInstances`), **256/256 landscape components** from
  in-package heightmaps, 352 lights; 906 mesh datablocks, 655 decoded textures, 0 assembly warnings.
- Reconcile (interim v1): level actors implementing `int_save`/`int_primitive` (checked live via the
  class package `Interfaces[]`, inherited) are skipped — 6,812 skipped; save rows re-express them.
  The kismet gatherer table (48 classes) is still the P2-polish item; `loadTransform=false` fixtures
  may sit at row transforms until then.
- Resolution ladder shipped: SCS templates → **CDO `name` → list_props** (the measured universal
  mechanism: every prop_C descendant carries `name` in its CDO; potato/coal_s/drive/paper/wbox
  confirmed) → curated supplement (dirthole mounds). Residual placeholders: 376 (trashBitsPile 264 —
  procedural pile visuals, prop_C 47, barnshelf 23, small tail).
- New measured facts vs the design: `FMeshUVHalf.to_mesh_uv_float` returns RAW half bits (decoded
  ourselves); the "Invalid boolean value" storm on some meshes is a POST-LOD tail failure
  (occluder/speedtree section) — geometry parses, materials come from the property block; ISM
  instance data confirmed as 64-byte FMatrix bulk arrays.
- Renders: scratchpad `votvio_shot.png` / `votvio_wide.png` / `votvio_top.png`.

**v3 same day (user feedback pass):** the resolver now composes the **SCS TREE**
(SCS_Node graph; flattening it was why dish heads sat buried at the actor origin) and
`pose_random.py` gives articulated fixtures seeded working poses (dish axis_Z/axis_Y,
windturbine axis_room/axis_blades, radar rot_Z; measured pivots). Reconcile refined:
a level int_save actor is skipped only when its CLASS has save rows — watchtowers/
fences render again, turbines spawn posed from their 4 rows. `materials.py` is the
census-grounded **family analog of the game's material system** (tex / ag=emissive
mask / ao / normal / rough, color/emissioncolor, Masked/Translucent/Additive, foliage,
triplanar box-mapping, built-in water shader from `w_absorb`), plus a **terrain style**
option (green default / snow / dirt, slope-rock blend) replacing the white ground.
**Import radius** option: 0 = whole map; else meters around the base (origin =
`baseBuilding_C` root): at 150 m the scene is 9,544 objects vs 77,209 full.

**v4 (same day, user field pass): the umap holds only DELTA components of a BP actor**
— radiotower's exported shape was ONE 90 m `misc/cube` imposter named `rend`; its real
mast/top/comm-panel are template-only and were never rendered. Fix: tree-first assembly
for level BP actors WITHOUT visible delta meshes (template tree + instance-delta merge
+ pose), while actors with real delta layouts (base building, doors, boarded windows)
keep the flat path; missing-template pivots (dish `axis_Z/axis_Y` have no export —
empty delta) walk through as identity, which un-buried the dish farm a second time.
Named imposter table (`rend`) + **technical meshes hidden by default**
(`misc/cube`, `misc/qweqwe`; "Show technical meshes" option brings them back).
Final full-smoke: 75,517 objects / 943 meshes / 703 textures / ~117 s.

## 1. What it is

A Blender 5.1 **extension** (`extensions/user_default/votvio`, manifest-based, python 3.13 + numpy,
**zero wheels**) that reads a `.sav` slot + the game's own `VotV-WindowsNoEditor.pak` directly and
reconstructs the world **as the game would load it**: the whole `untitled_1` map (static meshes,
landscape, foliage, lights), every saved prop/entity/vehicle/NPC from the save's `objectsData` /
`primitivesData` / `GObjStack`, reconciled per the game's own `loadObjects` semantics.

## 2. The measured foundation (one line each; details in the design doc)

- Pak: v11, **unencrypted, uncompressed** (footer fact: all 5 compression slots empty), 42,941 files.
- Save: GVAS `saveSlot_C`; own ~200-line reader parses the real 20 MB `s_1234.sav` clean end-to-end.
- `untitled_1.umap`: 50,951 exports parse in ~10 s (pyUE4Parse); `StreamingLevels=[]` — one persistent world.
- pyUE4Parse StaticMesh 4.27 bug root-caused (unconditional `minMobileLODIdx`) + fixed + geometry proven.
- Vendored parser runs under **Blender's own 3.13.9** (headless smoke: pak → cube geometry, 0.4 s).
- Texture census, FULL population: 4,033 textures, 8 formats, **zero BC7** ⇒ numpy decoders suffice.
- Archetype fact: 57 % of umap SMComponents inherit their mesh from the BP template ⇒
  **ClassTemplateResolver** (TemplateIndex-first) — full census: 81.5 % direct + justified exclusions
  (772 spawner previews) + 193-comp curated supplement; ledger balances exactly.
- Gatherer census (corrected): **48** classes override `gatherDataFromKey`; the 45 `…KeyT` classes are the
  separate trigger lane (doors etc. — never destroyed on load, stay as-cooked).
- NPC: kerfurOmega rows live in `objectsData` with transforms; SK geometry via the proven
  `ue_skelmesh.py` port (self-run: 102 bones / 4,332 verts, round-trip OK).
- Blender assembly measured trivial (20k objects 1.49 s); cold import ~2–4 min grounded, warm < 1 min.

## 3. Decisions (with dates)

- 2026-08-29 user: name **VotvIO**; this doc is the living arc doc.
- 2026-08-29 /qf: self-contained pure-python (SourceIO bar) — external .NET/FModel lanes rejected,
  recorded in the design doc §3.
- 2026-08-29 /qf: acceptance = **machine diff vs the live game** (UE4SS Lua probe, same save, post-load
  snapshot, self-calibrated tolerances) — the probe is **P2 step 1**, built before any P2 verdict.
  Exclusion rows always carry a measured justification; a noisy diff is never silenced by widening.
- 2026-08-29: generated tables (`int_save` membership, gatherer 48, trigger 45, UCS supplement 193,
  exclusions) derive **from the pak at addon build time** — the CXX dump is not a dependency.

## 4. Build plan

- **P0** — extension skeleton + .sav parse + manifest + placeholders. **BUILT.**
- **P1** — save props with real meshes + `tex` BaseColor materials. **BUILT** (5,091 meshed).
- **P2** — umap/landscape/foliage/lights + interim reconcile. **BUILT** (see §0). The formal
  acceptance probe (UE4SS Lua dump + t0/t0+5s calibration + machine diff) is still OWED and remains
  the verification gate — visual renders are smoke, not acceptance.
- **P3** — NPC SK bind pose (473 level SK comps + 23 save SK rows currently placeholders), grime
  decals, splines, gatherer kismet table, UCS supplement growth, BSP iff the diff shows missing walls.

## 5. Residual ledger

> **Session handoff 2026-08-29 (end of build day): the user reviewed the full scene and said
> "still many things to fix" — the NEXT session opens with their field-fix list.** Working
> agreement from this session: NO renders — hand over the `.blend`, the user inspects manually
> (scratchpad `votvio_smoke.blend` is the current full-scene artifact).

| Open | What | Phase |
|---|---|---|
| — | the user's field-fix list (pending, next session) | next |
| — | water: the material family is built, but lake/river SURFACES were never verified present | next |
| — | **acceptance probe + calibration + machine diff** (the design's own gate) | next |
| — | gatherer table from the 48 kismet bodies (interim: all int_save level actors skipped) | next |
| O8 | non-main-map saves (`Level != Untitled_1`) — generic attempt + warning, unvalidated | P3 |
| O12 | MIC parameter vocabulary census — the grey "monolith" slab = material without a `tex` param | P3 |
| O13 | SplineMeshComponent (89) deform | P3 |
| O14 | grime decal fidelity (piles/grime are placeholders/boxes) | P3 |
| O15 | landscape layer blending (terrain renders untextured white; weightmaps unshaded) | P3 |
| — | trashBitsPile procedural visuals (264 placeholders) + prop_C stragglers (47) | P3 |
| — | SK geometry port of `ue_skelmesh.py` (NPC bind pose) | P3 |

## 6. Dev notes

- Repo home: `tools/blender/votvio/` + `tools/blender/deploy_addon.ps1` → `extensions/user_default/votvio`.
- Headless loop: `blender --background --factory-startup --python tools/blender/votvio/tests/smoke.py`.
- License: MIT (repo license; GPL-compatible per Blender extension rules); vendored pyUE4Parse is MIT with
  attribution; the empirical vendor-patch list is in the design doc §1 (Blender side).
