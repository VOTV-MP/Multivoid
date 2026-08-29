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
single-skin paks with repak (UE4 pak V11, mount `../../../`, **Zlib**):

```
repak unpack -o un_<name> <name>.pak      # each source pak
# merge the four trees into one directory
repak pack -m "../../../" --version V11 -p 0 --compression Zlib <dir> scientists.pak
```

**Compress it — the uncompressed pak is 5.2x larger for nothing.** Measured
2026-08-29: 14 343 848 B uncompressed vs **2 776 794 B with Zlib**, unpack-compare
byte-identical, and a two-peer smoke loaded both skins from the compressed pak
(`client_model: -> skin 'luther_v1sc' (mesh 2/2 slots, atlas tex bound)` on both
peers). UE mounts a Zlib pak natively; nothing in our code sees the difference.

**Why it was that large, and the deeper fix (NOT done):** the meshes are 90-200 KB
each -- the whole cost is the TEXTURES, stored **uncompressed BGRA8**. Three are
exactly 4 MiB (`1024*1024*4`) and one is 1 MiB (`512*512*4`), for HL1-era art. The
converter in `tools/client_model/` should emit a block-compressed format (BC1/DXT1
is 8:1 on an opaque diffuse, BC7 4:1); that would cut the payload again AND cut GPU
memory at runtime, which Zlib does not -- a compressed pak still decompresses to
4 MiB per texture in VRAM. Zlib is the packaging fix; the texture format is the
asset fix.

The binaries are deliberately **not tracked** (public-repo caution for
game-derived meshes; the ship decision covers the RELEASE artifact —
`docs/UE4SS_ARC.md` §7.6). Restock this dir from any dev install:
`Game_0.9.0n_HOST/WindowsNoEditor/VotV/Content/Paks/LogicMods/multivoid/`,
or rebuild from the model sources (the "votv convert" workspace). CI therefore
assembles a pak-less zip for drills; a RELEASE zip is assembled on a box where
this dir is stocked (`UE4SS_ARC` §7.9's manual-assembly lane).
