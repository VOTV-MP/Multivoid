# assets/paks — the starter-skin pak staging dir (UNTRACKED contents)

The release zip's `pak\` route is filled from here by `tools/release/package.ps1`
(auto-included when the dir is non-empty). Expected contents — ONE bundle pak
carrying the four starter scientists the mod assigns randomly to a new identity
(user decision 2026-08-29: "нужен общий пак scientists.pak"), plus the four
F1-browser preview tiles by MEMBER name:

```
scientists.pak     (walter_v1sc + sci_v1sc + rvi_scientist_v1sc + luther_v1sc)
walter_v1sc.png  sci_v1sc.png  rvi_scientist_v1sc.png  luther_v1sc.png
```

The bundle-to-members mapping lives in `skin_registry.cpp` `kSkinBundles`; the
loader itself never keys on the pak filename (assets resolve by their internal
`/Game/Mods/VOTVCoop/<name>` paths). Rebuild the bundle from the four
single-skin paks with repak (UE4 pak V11, mount `../../../`, no compression):
`repak unpack` each + merge trees + `repak pack -m "../../../" --version V11`.

The binaries are deliberately **not tracked** (public-repo caution for
game-derived meshes; the ship decision covers the RELEASE artifact —
`docs/UE4SS_ARC.md` §7.6). Restock this dir from any dev install:
`Game_0.9.0n_HOST/WindowsNoEditor/VotV/Content/Paks/LogicMods/multivoid/`,
or rebuild from the model sources (the "votv convert" workspace). CI therefore
assembles a pak-less zip for drills; a RELEASE zip is assembled on a box where
this dir is stocked (`UE4SS_ARC` §7.9's manual-assembly lane).
