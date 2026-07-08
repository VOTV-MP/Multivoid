# mesh_extract

Dev/RE tool (does NOT ship — RULE 3). Reads VOTV's cooked, **unencrypted v11**
pak via CUE4Parse and exports skeletal meshes + their skeleton (rig) to
Blender-openable formats. We read the cooked pak; we never modify/repack it
(RULE 1). Output is copyrighted game content → write it under `research/pak_re/`
(gitignored).

Why not the usual tools: `repak` (`tools/bp_reflect.py`) only extracts the raw
`.uasset`; it can't deserialize a SkeletalMesh. gildor.org (umodel) is
unreachable and FModel is GUI-only. CUE4Parse (the library FModel is built on) is
on NuGet, so we drive it directly. Needs the .NET SDK (dotnet 9) + internet on
first run (NuGet restore).

## Usage

```
# export a mesh (+ rig + textures) to .psk (ActorX) and .glb (glTF)
dotnet run --project tools/mesh_extract -- export <PaksDir> <outDir> [assetSubstr ...]

# list SkeletalMesh/Skeleton assets + bone counts under a path substring
dotnet run --project tools/mesh_extract -- scan <PaksDir> <substr>

# dump a package's referenced assets (which mesh/skeleton a BP uses)
dotnet run --project tools/mesh_extract -- imports <PaksDir> <pkgSubstr> [classFilter]
```

`<PaksDir>` = `Game_0.9.0n_HOST\WindowsNoEditor\VotV\Content\Paks`.
`assetSubstr` matches against the `.uasset` path (default: the Kel body
`meshes/kel/kel_lmao.uasset`).

## Example (what produced the current extraction)

```
$P = 'D:\Projects\Programming\VOTV_MP\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Content\Paks'
$O = 'D:\Projects\Programming\VOTV_MP\research\pak_re\mesh_out'
dotnet run --project tools/mesh_extract -- export $P $O `
  'meshes/kel/kel_lmao.uasset' `
  'kerfurAnthro/sk/kerfurOmegaV1.uasset' `
  'kerfurAnthro/sk/kerfurOmega_KelSkin.uasset' `
  'kerfurAnthro/sk/kerfurOmega_mannequinSkin.uasset'
```

## Rig facts (see docs/COOP_CLIENT_MODEL.md)

- Dr. Kel `kel_lmao` = 6-bone, armless low-poly.
- Humanoid rig with hands = anthro `kerfurOmegaV1_Skeleton` (102 bones,
  `hand_R`/`hand_L`/`finger_*`), shared by all `meshes/kerfurAnthro/sk/*` skins.

## Blender import

- `.glb` → native glTF 2.0 import.
- `.psk` → the `io_scene_psk_psa` addon at `reference/psk-psa-v9.1.2`
  (Blender 4.2+ extension). Keep bone names/hierarchy when re-skinning.
