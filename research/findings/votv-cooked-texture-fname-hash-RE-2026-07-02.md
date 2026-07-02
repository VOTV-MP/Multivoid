# VOTV cooked UTexture2D layout + FName name-map hashes + kel material chain + the puppet two-body facts

**Date:** 2026-07-02. **Type:** durable RE (byte layouts empirically validated against game assets;
engine behavior validated by in-game probes). Sources: `tex_kel3_skin` / `inst_kel4_body` (repak-extracted),
probe runs on live game. Implemented in `tools/client_model/ue_tex.py` (self-testing) + consumed by
`docs/COOP_CLIENT_MODEL.md` 7/8.

## 1. Cooked UTexture2D (.uexp) layout — UE4.27, VOTV (validated field-by-field on tex_kel3_skin)

```
[tagged properties]                  ImportedSize (IntPoint), LightingGuid, LODGroup, ... 'None'
[bHasGuid u32 = 0]                   the standard UObject::Serialize tail after the property list
[UTexture   strip flags u8+u8]       = 01 00 in cooked (global bit0 = editor-data-stripped)
[UTexture2D strip flags u8+u8]       = 01 00   (TWO FStripDataFlags -- one per Serialize level)
[bCooked u32 = 1]
[PixelFormatName FName (8B)]         e.g. 'PF_DXT1' -- name-map indexed
[SkipOffset i64]                     ABSOLUTE offset (totalHeaderSize + uexp pos) of the 'None' terminator
[FTexturePlatformData::SerializeCooked:
   SizeX i32, SizeY i32,
   PackedData u32                    = NumSlices (1) | flags
   PixelFormat FString               e.g. "PF_DXT1" (string AGAIN, len-prefixed)
   FirstMipToSerialize i32 = 0
   NumMips i32
   per mip:
     bCooked u32 = 1
     BulkDataFlags u32               streamed mip: 0x10501 = PayloadAtEndOfFile|PayloadInSeperateFile|
                                       Force_NOT_InlinePayload|NoOffsetFixUp  (payload in .ubulk,
                                       offset is UBULK-RELATIVE cumulative)
                                     inline mip:   0x48 = ForceInlinePayload|SingleUse
     ElementCount i32 (byte size), SizeOnDisk i32 (same)
     OffsetInFile i64                inline: ABSOLUTE (totalHeaderSize + uexp payload pos)
     [payload bytes -- inline only]
     SizeX i32, SizeY i32, SizeZ i32
   bIsVirtual u32 = 0]
['None' FName (8B)]                  the pixel-format loop terminator (SkipOffset points here)
[PACKAGE_FILE_TAG u32]
```

tex_kel3_skin concrete: PF_DXT1 512x512, 10 mips; mips 0-2 streamed (.ubulk = 131072+32768+8192 =
172032 B exactly), mips 3-9 inline. Our cook (`ue_tex.py`): PF_B8G8R8A8, 1 mip, fully inline — no
.ubulk file needed; non-power-of-two OK with NumMips=1.

## 2. FName name-map entry hashes — the REAL recipe (validated 19/19 BOTH slots vs tex_kel3_skin)

Each name-map entry: `FString` + `u16 NonCasePreservingHash` + `u16 CasePreservingHash`, where:
- **NonCase** = `FCrc::Strihash_DEPRECATED(name) & 0xFFFF` — per char: UPPERCASE it, process ONLY the
  low byte, `h = ((h>>8)&0xFFFFFF) ^ TBL[(h^byte)&0xFF]` with the **MSB-first** CRC table
  (poly 0x04C11DB7, `tbl[i]` from `i<<24`), init 0, **no final inversion**.
- **Case** = `FCrc::StrCrc32(name) & 0xFFFF` — standard reflected CRC32 (poly 0xEDB88320,
  init/final-invert standard) with **each char fed as 4 LE bytes** (char, 0, 0, 0 for ASCII).

The earlier claim in COOP_CLIENT_MODEL.md 8 ("NonCase = StrCrc32(name.lower())", "verified 6/6") was
WRONG on the NonCase slot — that "verification" never compared against stored bytes correctly.
Implementation + 19/19 self-test: `ue_tex.py name_entry()/selftest`. With this, a full package RENAME
is safe (name map rebuild + summary offset shifting + export SerialSize/SerialOffset patch — ue_tex.py
does all three; the texture package ships cleanly renamed as `/Game/Mods/VOTVCoop/tex_scientist`).

## 3. kel body material chain (slot 0 of kerfurOmega_KelSkin) — MIC property parse

`inst_kel4_body` = **MaterialInstanceConstant**, Parent = `/Game/materials/basic/mat_object_sk`
(a Material). ONE texture parameter: **ParameterInfo.Name = 'tex'** (Association=GlobalParameter),
value = `tex_kel3_skin`. One scalar param: RefractionDepthBias=0. => A runtime
`CreateDynamicMaterialInstance(component, 0)` + `SetTextureParameterValue('tex', ourTexture)`
re-skins the slot with NO cooked material. (A cooked UMaterial is NOT python-cookable — it embeds
platform shader maps; MIC/MID parameter override is the only mod-side path.)

## 4. mainPlayer_C puppet TWO-BODY facts (probe-verified in-game 2026-07-02)

- The actor renders **two overlapping bodies**: `mesh_playerVisible` (@0x04F8) AND its AttachParent,
  the inherited `ACharacter::Mesh` native slot (@0x0280), which carries its own kel skin. With
  identical skins the overlap is invisible (the game's own shape); a CUSTOM mesh applied to
  mesh_playerVisible alone stays **masked** by the native slot's kel (probe take 1: a 1062-vert
  scientist "rendered as" a pixel-perfect kel — impossible from its buffers; the kel was the OTHER body).
- **Hiding the AttachParent hides the child too, regardless of the propagate flag** (probe take 2,
  measured: `SetVisibility(false,false)` + `SetHiddenInGame(true,false)` on the native slot made BOTH
  bodies vanish). UE's visibility resolution walks the attach chain (`IsVisible`'s parent-walk — the
  codebase had already recorded this at `engine.h ClearSkeletalMesh`'s comment). Never hide the native
  body slot.
- **The fix is the game's own invariant:** write the SAME mesh into BOTH slots
  (`SpawnPuppetMainPlayer` now does; stock kel = engine early-out no-op). Probe take 3 with the
  scientist in both slots rendered the scientist shape, animated, hands-on-verified.

## 5. Probe-first record (why the healthy cook was never "fixed")

Take-1 "renders kel" had 4 candidate roots. Byte-parse (ue_skelmesh.py: our cooked render region =
1 section/1062 verts/52,016 B vs template 3 sections/17,132 verts/1,784,520 B; sha256 chain cook
output == staging == deployed pak) REFUTED the cook-splice hypothesis before any cook edits; the
impossibility argument (1062 verts cannot draw a clean kel) forced the two-body discovery. The cook
was correct all along.
