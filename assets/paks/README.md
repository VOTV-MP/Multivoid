# assets/paks — the starter-skin pak staging dir (UNTRACKED contents)

The release zip's `pak\` route is filled from here by `tools/release/package.ps1`
(auto-included when the dir is non-empty). Expected contents — the four starter
scientists the mod assigns randomly to a new identity, plus their F1-browser
preview tiles:

```
walter_v1sc.pak         walter_v1sc.png
sci_v1sc.pak            sci_v1sc.png
rvi_scientist_v1sc.pak  rvi_scientist_v1sc.png
luther_v1sc.pak         luther_v1sc.png
```

The binaries are deliberately **not tracked** (public-repo caution for
game-derived meshes; the ship decision covers the RELEASE artifact —
`docs/UE4SS_ARC.md` §7.6). Restock this dir from any dev install:
`Game_0.9.0n_HOST/WindowsNoEditor/VotV/Content/Paks/LogicMods/multivoid/`,
or rebuild from the model sources (the "votv convert" workspace). CI therefore
assembles a pak-less zip for drills; a RELEASE zip is assembled on a box where
this dir is stocked (`UE4SS_ARC` §7.9's manual-assembly lane).
