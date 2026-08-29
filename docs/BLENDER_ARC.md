# BLENDER_ARC — VotvIO: the Blender 5.1 addon that imports a VOTV .sav into a full scene

> Living doc for the VotvIO arc. Started 2026-08-29 (user ask, verbatim: «Нужен аддон для блендера 5.1,
> который позволит импортировать .sav сохранение Voices Of The Void — далее аддон прогрузит всю карту и
> все ассеты, которые валяются на карте, все модели и все entity, все npc»; precedent bar named by the
> user: SourceIO — self-contained, no manual export steps). Addon name **VotvIO** (user's pick).
> Design of record: `research/findings/tooling/votv-blender-sav-importer-DESIGN-2026-08-29.md` (local-only,
> like all research/ pointers) + its QF transcript sibling. Status: **DESIGN converged (/qf, 7 rounds,
> "that holds"); NOTHING BUILT yet.**

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

- **P0** — extension skeleton + .sav parse + manifest + placeholder boxes (no pak). ← NEXT
- **P1** — save props with real meshes + `tex` BaseColor materials; decode budget profiled.
- **P2** — acceptance probe+calibration FIRST, then umap/landscape/foliage/lights + reconcile; diff green.
- **P3** — NPC SK bind pose, grime decals, splines, UCS supplement, BSP iff the diff shows missing walls.

## 5. Residual ledger

| Open | What | Phase |
|---|---|---|
| O8 | non-main-map saves (`Level != Untitled_1`) — generic attempt + warning, unvalidated | P1 |
| O12 | MIC parameter vocabulary census (normal/roughness names) | P1 |
| O13 | SplineMeshComponent (89) deform | P3 |
| O14 | grime decal fidelity | P3 |
| O15 | landscape layer blending fidelity | P2/P3 |
| — | UCS-dynamic supplement (193 comps, ~11 classes) — counts published here when built | P3 |
| — | door-root-transform inferred row — verified at first acceptance run | P2 |

## 6. Dev notes

- Repo home: `tools/blender/votvio/` + `tools/blender/deploy_addon.ps1` → `extensions/user_default/votvio`.
- Headless loop: `blender --background --factory-startup --python tools/blender/votvio/tests/smoke.py`.
- License: MIT (repo license; GPL-compatible per Blender extension rules); vendored pyUE4Parse is MIT with
  attribution; the empirical vendor-patch list is in the design doc §1 (Blender side).
