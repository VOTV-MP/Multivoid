# client_model — custom per-client model pipeline (VOTV coop)

Everything for the **custom client-model** feature in one place: take a source model,
auto-repose it to VOTV's T-pose, **cook it into a real UE4.27 `USkeletalMesh` + `.pak`
WITHOUT the UE editor** (pure Python), and have the mod load it on client puppets (host
stays Dr. Kel). Design + runtime + test recipe: `docs/COOP_CLIENT_MODEL.md`; byte-level
cook spec: `SPEC.md` here.

Decisions (RULE 1 no-crutch, RULE 3 no-editor): real cooked skeletal mesh (engine skins
it natively via the anthro AnimBP), target rig = **anthro `kerfurOmegaV1_Skeleton`**
(~101 bones — the real client skin's rig), cook engine = **pure Python**, source =
**GoldSrc/HL1 `.mdl`**.

## Pipeline (stages → tools)

```
source.mdl
  → mdl_extract.py   model.obj(+usemtl per face) + model.bones.json(+bone WORLD mats) + tex/*.png
  → repose.py apply  <model>_tpose.obj        (auto A-pose→VOTV T-pose + scale; PROFILE-driven --
                                               profiles/ library, `default` = the current standard)
  → atlas.py         atlas.png + atlas.json   (all tex/*.png shelf-packed, 1px clamp-extend gutter)
  → ue_cook.py       <model>.uasset/.uexp     (Y-mirror PSK→cooked, HL→anthro rigid remap,
                                               per-face atlas UV remap + v-flip, winding matched
                                               to the TEMPLATE's measured signed-volume side,
                                               splice into kerfurOmega_KelSkin cooked template)
  → ue_tex.py cook   tex_<model>.uasset/.uexp (atlas.png → cooked UTexture2D PF_B8G8R8A8 inline)
  → repak pack       <model>.pak              (VotV/Content/Mods/VOTVCoop/*, V11, 4 files)
  → mod runtime:     mount pak + LoadObject + both-slot SetSkeletalMesh + slot-0 MID
                     SetTextureParameterValue('tex') by peer role
```

Current model = `hl_einstein_v1sc` (its ORIGINAL name; the early "scientist" naming is
retired). Profiles: `profiles/README.md` — v1 "narrow" is the DEFAULT (in-game verdict
2026-07-02 evening: the v2 "wide" look was rejected; v2 stays in the library).

Adding a NEW HL model = the six steps above (no Blender). See `docs/COOP_CLIENT_MODEL.md §4`
for exact commands. Use `python` (has numpy), not `python3`.

**PORTABLE ONE-SHOT (`portable/`, 2026-07-02):** `python portable/make_portable.py [--exe]`
bundles the LIVE modules above (unmodified — single source of truth, the originals stay) +
the embedded cook templates + the default profile into `portable/dist/`:
`convert_model.pyz` (needs python+numpy+pillow) / `convert_model.exe` (needs nothing) +
`repak.exe` + `convert.bat` + `README.txt` (RU). Drop the dist files into any folder with a
`.mdl`, run `convert.bat` — the `.pak` appears right there. Flags: `--name` (cook under the
DLL's fixed `hl_einstein_v1sc` name to swap the in-game model), `--profile`, `--keep-work`.
Verified 2026-07-02: pyz and exe runs both reproduce the deployed pak content 4/4
byte-identical. `dist/`+`build/` are gitignored (game-derived template bytes); rebuild after
any module change.

## Tools here
- **`mdl_extract.py`** — GoldSrc/HL1 `.mdl` → `model.obj` (A-pose) + `model.bones.json`
  (hierarchy + per-vertex bone + **HL bone WORLD matrices**) + `tex/*.png`. Pure Python.
- **`repose.py`** — the repose automation. `learn` extracts a VOTV T-pose profile from a
  manual example; `apply` auto-reposes any HL Bip01 model to T-pose + scale (`default` =
  the library default). Validated: reproduces each manual example to residual ~0 (v2 =
  0.00005 max). See `docs/COOP_CLIENT_MODEL.md §5`.
- **`profiles/`** — the profile LIBRARY (one json per learned example; format 1 =
  rotation-only, format 2 = full R+t local deltas). `profiles/README.md` = provenance
  table; DEFAULT_PROFILE in repose.py names the default (v1 narrow).
- **`portable/`** — the drop-in-folder converter: `driver.py` (orchestrates the six steps
  in the model's own folder, embedded templates/profile) + `make_portable.py` (builds
  dist/: pyz + optional PyInstaller exe + repak.exe + convert.bat + README.txt).
- **`atlas.py`** — shelf-packs `tex/*.png` into ONE atlas + `atlas.json` name→pixel-rect map
  (1px clamp-extend gutter; no mips cooked so 1px kills bilinear bleed).
- **`ue_cook.py`** — THE COOK. Sources the reposed OBJ, applies the exact PSK→cooked Y-mirror,
  matches the winding to the TEMPLATE's measured signed-volume side (never an assumed
  convention — the assumption rendered inside-out, hands-on 2026-07-02), remaps every corner
  UV into its usemtl atlas tile (v-flip: cooked v = 1 - obj_v), HL→anthro rigid bone remap,
  splices scientist buffers into the cooked `kerfurOmega_KelSkin` template, fixes
  SerialSize/BulkDataStartOffset. Output valid.
- **`ue_tex.py`** — cooks a PNG into a cooked `UTexture2D` package (PF_B8G8R8A8, 1 inline
  mip, full package rename with the real FName hash recipe). `cook <png> <out_base>`.
- **`ue_skelmesh.py`** — cooked `FSkeletalMeshRenderData` parser; round-trips kerfurOmega +
  kel_lmao byte-for-byte. Validates cook output. `python ue_skelmesh.py <base-no-ext>`
- **`ue_pkg.py`** — UE4.27 cooked package (de)serializer; round-trips byte-identical.
- **`skin_to_rig.py`** — SUPERSEDED (early rigid-remap→glb Blender-view helper; not in the
  cook path). Kept as a viewing aid.
- **`skin_transfer.py`** — glb loader/weight-transfer utility used by `skin_to_rig`.
- **`mesh_extract/`** — C#/CUE4Parse (dotnet): study game meshes (`export`/`scan`/`imports`).
- **`cue4parse_ref/`** — CUE4Parse reader sources (MIT) = the byte-order spec we mirror.
- **`SPEC.md`** — the pure-Python cook spec: serialization order, buffer formats, offsets.

## Related (kept in place, referenced)
- `research/pak_re/` — workspace (gitignored): `mesh_out/` (mdl_extract + cook outputs),
  `modpak/` (pak staging), `extracted/` (cook template = repak-extracted game meshes),
  `tools/repak.exe` (packer), `hl_einstein_v1sc.pak` (the deliverable).
- `tools/hl_einstein_v1sc/` — example HL1 scientist `.mdl` + `dr_kel.psk` (reference skin)
  + `hl_einstein_v1sc_tpose.obj` (reposed, v1 profile) + the manual examples
  `hl_einstein_v1sc.psk` (v1 narrow) / `hl_einstein_v1sc_new_profile.psk` (v2 wide)
  (Valve asset / local only).
- `tools/SourceIO/` — Blender addon (GoldSrc `.mdl` import); `reference/psk-psa-v9.1.2/`
  — PSK import/export. Used for the manual repose example.

## Status (2026-07-02)
- **EVERYTHING VERIFIED IN-GAME (hands-on):** geometry (shape/rig/anim), textures (19-tile
  atlas, real scientist look), winding (template-matched signed volume — the old outward-normal
  assumption rendered inside-out and was root-fixed). Runtime DONE + PROVEN: pak auto-mount +
  LoadObject + role-gated both-slot mesh apply + slot-0 MID texture bind
  (`coop/player/client_model` + `RemotePlayer::Spawn`).
- Remaining: the coop two-peer visual (host+client facing each other) — the feature path is
  wired but exercised solo-host via the probe only. `docs/COOP_CLIENT_MODEL.md` STATUS is
  the canonical live state.

RULE 3: everything here is a dev/RE tool — none of it ships at runtime.
