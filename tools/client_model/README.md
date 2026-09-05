# client_model: the custom skin pipeline

Everything for custom player skins in one place: take a source model, repose it to the game's
T-pose, cook it into a real UE4.27 `USkeletalMesh` and a `.pak` without the Unreal editor (pure
Python), and have the mod load it on a player's puppet. The runtime side, how a skin is chosen,
announced and applied, is on `docs/players.md`; the byte-level cook spec is `SPEC.md` here.

Decisions: a real cooked skeletal mesh (the engine skins it natively through the game's own
animation blueprint); the target rig is the game's anthro skeleton `kerfurOmegaV1_Skeleton`,
about a hundred bones, the rig of the real client skin; the cook engine is pure Python; the
source format is a GoldSrc (Half-Life) `.mdl`. No Blender in the path and no editor.

## Pipeline

```
source.mdl
  -> mdl_extract.py   model.obj (+usemtl per face) + model.bones.json (+bone world matrices) + tex/*.png
  -> repose.py apply  <model>_tpose.obj        (A-pose to the game's T-pose plus scale, driven by a
                                               profile from profiles/; `default` is the current standard)
  -> atlas.py         atlas.png + atlas.json   (every tex/*.png shelf-packed, 1 px clamp-extend gutter)
  -> ue_cook.py       <model>.uasset/.uexp     (Y-mirror PSK to cooked, HL-to-anthro rigid remap,
                                               per-face atlas UV remap and v-flip, winding matched to
                                               the template's measured signed-volume side, spliced into
                                               the cooked kerfurOmega_KelSkin template)
  -> ue_tex.py cook   tex_<model>.uasset/.uexp (atlas.png as a cooked UTexture2D, PF_B8G8R8A8 inline)
  -> repak pack       <model>.pak              (VotV/Content/Mods/VOTVCoop/*, V11, four files)
  -> the mod:         mounts the pak, loads the objects, sets both mesh slots and the slot-0
                      material's texture parameter by peer role
```

Use `python` with numpy and pillow. Adding a new model is the six steps above, or one run of the
portable converter.

## The portable converter

`python portable/make_portable.py [--exe]` bundles the live modules above (unmodified; the
originals stay the single source of truth), the embedded cook templates and the profile library
into `portable/dist/`: `convert_model.pyz` (needs python, numpy and pillow) or
`convert_model.exe` (needs nothing), plus `repak.exe`, `convert.bat` and a `README.txt`. Drop the
dist files into any folder with a `.mdl` and run `convert.bat`; the `.pak` appears there, plus
`<name>.png`, the skin browser's preview tile (converted from the model's own `.bmp` thumbnail
when present; the browser also reads a raw `<name>.bmp` beside the pak). Flags: `--name`,
`--learn`, `--profile`, `--keep-work`. The pak name is the in-game skin name: drop the pak and the
preview into `LogicMods/multivoid/` on every peer and pick it in F1 > Cosmetics > Skins.

Profile resolution: a manual-pose PSK next to the `.mdl` is auto-detected (exact point and bone
correspondence) and its exact profile is learned and saved as `<name>.profile.json`; otherwise
the best library profile is auto-selected by a printed scoring table (bone coverage, then
rest-pose similarity), with uncovered bones reported instead of silently left in A-pose. The pyz
and the exe reproduce the deployed pak content byte-identically. `dist/` and `build/` are
gitignored (game-derived template bytes); rebuild after any module change.

## The tools

- `mdl_extract.py`: GoldSrc `.mdl` to `model.obj` (A-pose), `model.bones.json` (hierarchy,
  per-vertex bone, bone world matrices) and `tex/*.png`. Pure Python.
- `repose.py`: `learn` extracts a T-pose profile from a manual example (format 3: per-bone pose
  deltas plus a rest-pose fit metric); `apply` reposes with an explicit profile or `auto` (library
  scoring: coverage, then rest-pose similarity), always printing the uncovered-bones report;
  `select` prints the table only. It reproduces each manual example to a residual near zero.
- `profiles/`: the profile library, one format-3 JSON per learned example; `profiles/README.md`
  is the provenance and status table (a rejected profile is skipped by auto-select).
- `portable/`: `driver.py` orchestrates the six steps in the model's own folder with the embedded
  templates and the profile library, auto-learning from a manual-pose PSK beside the `.mdl`;
  `make_portable.py` builds `dist/`.
- `atlas.py`: shelf-packs `tex/*.png` into one atlas plus `atlas.json`, a name-to-pixel-rect map
  (a 1 px clamp-extend gutter; no mips are cooked, so 1 px kills bilinear bleed).
- `ue_cook.py`: the cook. Reads the reposed OBJ, applies the PSK-to-cooked Y-mirror, matches the
  winding to the template's measured signed-volume side (never an assumed convention; the
  assumption rendered inside-out), remaps every corner UV into its atlas tile (cooked v = 1 minus
  obj v), rigid-remaps HL bones to the anthro rig, splices the buffers into the cooked template
  and fixes the serial sizes and bulk-data offsets.
- `ue_tex.py`: cooks a PNG into a cooked `UTexture2D` package (PF_B8G8R8A8, one inline mip, a full
  package rename with the real name-hash recipe). `cook <png> <out_base>`.
- `ue_skelmesh.py`: the cooked skeletal-mesh render-data parser; round-trips the game's own meshes
  byte for byte and validates cook output. `python ue_skelmesh.py <base-no-ext>`.
- `ue_pkg.py`: the UE4.27 cooked package (de)serializer; round-trips byte-identical.
- `skin_to_rig.py` and `skin_transfer.py`: an early rigid-remap viewing aid and its
  weight-transfer utility; not in the cook path.
- `mesh_extract/`: a C# CUE4Parse tool to study the game's own meshes (`export`, `scan`,
  `imports`).
- `cue4parse_ref/`: the CUE4Parse reader sources (MIT), the byte-order spec the cook mirrors.
- `SPEC.md`: the pure-Python cook spec: serialization order, buffer formats, offsets.

The source models, the extracted game templates and the packer live in a gitignored workspace
outside this folder; the cook templates are game-derived bytes and are never committed.

## Status

Verified in-game: geometry (shape, rig, animation), textures (a 19-tile atlas), winding, and the
runtime path (pak mount, object load, both-slot mesh apply, slot-0 texture bind), with two peers
loading each other's skins. The next asset-side item is block-compressed textures, which the
README under `assets/paks/` explains.

None of this ships at runtime; it is a build tool for assets.
