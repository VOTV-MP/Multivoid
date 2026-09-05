# VOTV SkeletalMesh pure-Python cooker — implementation spec (resumable)

> All tools live under `tools/client_model/` (this file too); a bare tool name below means
> the file of that name in this folder. Overview:
> the runtime side is `docs/players.md` (Skins).

> The real client skin is `kerfurOmega_KelSkin` (ANTHRO rig
> `kerfurOmegaV1_Skeleton`, ~101 bones w/ hands). `kel_lmao` (6-bone) is a stick
> placeholder. **Target/template = `kerfurOmega_KelSkin`, NOT kel_lmao.** The
> kel_lmao offsets/anchors below were a FORMAT PROOF (parse round-tripped) — the
> UE4.27 cooked format is identical, so re-run the parse GENERICALLY on
> `kerfurOmega_KelSkin.uexp` (different counts/offsets: 10978 verts, 101 bones,
> uexp ~1.7 MB; `ue_pkg` already round-trips its package). skin_to_rig → HL→anthro map.

**Goal:** cook a custom `USkeletalMesh` `.uasset` WITHOUT the
UE editor, by **mutating kel_lmao's cooked mesh** (keep skeleton/materials/package,
splice in the scientist geometry+weights from `skin_to_rig.py`), then pack
with repak. Feeds the skins runtime (`docs/players.md`).
Spec sources (MIT, downloaded): `cue4parse_ref/*.cs` (CUE4Parse readers —
the exact byte order; the same code that successfully read kel_lmao).

## Guiding trick (why this is tractable)
The scientist binds to the SAME `kel_lmao_Skeleton` (6 bones) in the SAME engine,
so **mirror kel_lmao's CONCRETE buffer format** — read its actual header params
(index dataSize, position stride, UV count/precision, tangent format, skin-weight
influences/flags) and re-emit the scientist's data in that identical format. This
sidesteps resolving UE4.27 version-flag branches abstractly. Validate by
**round-trip** (re-emit kel's own bytes → byte-identical) BEFORE splicing.

## What's DONE (tools/ue_pkg.py)
- Package summary round-trips byte-identical. VOTV quirks: **no LocalizationId**
  in summary; export has TemplateIndex; CustomVersionsCount=0 (⇒ CUE4Parse used
  UE4.27 DEFAULT versions when it read kel_lmao — so mirror UE4.27 defaults).
- kel_lmao.uasset: TotalHeaderSize=3241, nameCount=104@193, exportCount=1@3109,
  importCount=9@2857. Export `kel_lmao`: SerialSize=17391, SerialOffset=3241.
- Payload = `kel_lmao.uexp[0:17391]` (+4-byte pkg tag at end). BulkDataStartOffset
  = 20632 (end of uexp − 4) ⇒ NO separate `.ubulk`.
- Payload property block (tagged, NOT unversioned; PackageFlags=0x80000000
  =PKG_FilterEditorOnly, no PKG_UnversionedProperties): `Skeleton`(ObjectProperty),
  `LODInfo`(ArrayProperty), `SamplingInfo`(StructProperty) → **"None" ends @2003**.
  Tag layout has property-guid byte + struct-guid (both present).

## Render region = payload[2003:17391] (15388 bytes) — serialization order
From `USkeletalMesh.Deserialize` + `FStaticLODModel` (`cue4parse_ref/`):
1. `FStripDataFlags` — 2 bytes (GlobalStripFlags u8, ClassStripFlags u8).
2. `ImportedBounds` = FBoxSphereBounds — Origin(FVector 12) + BoxExtent(12) +
   SphereRadius(float 4) = 28 bytes. **[SPLICE: recompute from scientist verts]**
3. `SkeletalMaterials[]` — TArray: count i32 + each FSkeletalMaterial (read
   `FSkeletalMaterial.cs`; MaterialInterface FPackageIndex + MaterialSlotName FName
   + UVChannelData…). **[KEEP unchanged]**
4. `ReferenceSkeleton` (FReferenceSkeleton) — RawRefBoneInfo TArray (FMeshBoneInfo =
   Name FName(8) + ParentIndex i32; cooked strips ExportName) + RawRefBonePose
   TArray (FTransform = Quat 16 + Vec 12 + Vec 12 = 40) + NameToIndexMap.
   **[KEEP unchanged — it's the 6-bone kel rig we target]** (need FReferenceSkeleton.cs
   — download failed; find via GitHub API, likely `Objects/Engine/` or
   `Objects/Meshes/`, else read from UEViewer/CUE4Parse repo.)
5. `bCooked` bool. (size 1 vs 4 — don't care; cursor advance validated by round-trip)
6. `numLODs` i32, then per-LOD **SerializeRenderItem** (new cooked format;
   UE4.27 default `SkeletalMesh.UseNewCookedFormat`=true).

### SerializeRenderItem (per LOD)
- `FStripDataFlags`(2), `bIsLODCookedOut`(bool), `bInlined`(bool)
- `RequiredBones` TArray<short>  **[SPLICE: all 6 bone indices]**
- if !AVstripped && !bIsLODCookedOut:
  - `Sections`: count i32 + each `FSkelMeshSection.SerializeRenderItem`
    (read `FSkelMeshSection.cs`: MaterialIndex u16, BaseIndex u32, NumTriangles u32,
    BaseVertexIndex u32, BoneMap TArray<short>, NumVertices i32, MaxBoneInfluences,
    bDisabled…). **[SPLICE: NumTriangles, NumVertices, BaseIndex/BaseVertexIndex,
    BoneMap=all 6]**
  - `ActiveBoneIndices` TArray<short> **[SPLICE: all 6]**
  - `Ar.Position += 4` — buffersSize u32 **[SPLICE: recompute]**
  - `bInlined` (true for normal cooked) → **SerializeStreamedData**

### SerializeStreamedData (the vertex/index buffers — the SPLICE target)
1. `FStripDataFlags`(2)
2. `Indices` = FMultisizeIndexContainer: `byte dataSize`(2=u16 | 4=u32) + bulk array
   (`elemSize i32, count i32, count*elemSize bytes`). **[SPLICE: scientist tri indices]**
3. `PositionBuffer` FPositionVertexBuffer: `i32 Stride`(12) + `i32 NumVertices` +
   bulk `FVector`(elemSize 12). **[SPLICE: scientist positions]**
4. `StaticMeshVertexBuffer` (tangents+UV): read `FStaticMeshVertexBuffer.cs` —
   NumTexCoords, Strides, bUseFullPrecisionUVs, tangents = 2×FPackedNormal (8 bytes),
   UVs = FMeshUVHalf(4) or FMeshUVFloat(8) × NumTexCoords. **[SPLICE: scientist
   normals→packed tangents + UVs]**
5. `SkinWeightBuffer` FSkinWeightVertexBuffer: strip flags(2) + metadata
   (UE4.27 default path: bNewWeightFormat = FAnimObjectVersion>=UnlimitedBoneInfluences
   — CONFIRM by mirroring kel's bytes) + bulk FSkinWeightInfo (4 bone-idx bytes + 4
   weight bytes = 8/vert for 4-influence). **[SPLICE: scientist bone(1) + weight 255]**
6. if bHasVertexColors: color buffer (kel: likely false — confirm).
7. if (4.27 < RemovingTessellation) && !AdjacencyStripped: `AdjacencyIndexBuffer`
   FMultisizeIndexContainer. **[was `bBuildAdjacencyBuffer` name — likely present;
   SPLICE or drop by stripping the flag]**
8. if HasClothData: cloth (kel: none).
9. `SkinWeightProfilesData` (TArray<FName> meta; likely empty).
10. if `SkeletalMesh.HasRayTracingData` (4.27?): ray tracing block — CONFIRM by
    whether bytes remain after (9) up to SerialSize end.

Bulk array = `ElementSize i32, ElementCount i32, ElementCount*ElementSize bytes`
(no separate .ubulk here — all inline).

## SPLICE plan (single LOD)
Input: scientist from `skin_to_rig.py` (positions, normals, UVs, per-vertex
Kel bone, triangles). All bind to kel rig (6 bones), weight 1.0 → skin-weight =
`[boneIdx,0,0,0]` + `[255,0,0,0]`.
1. Parse render region, record every field's byte range + concrete format params.
2. Round-trip (re-emit unchanged) → assert byte-identical vs payload[2003:17391].
3. Replace: Indices, Position, StaticMeshVertex (tangents/UV), SkinWeight buffers
   with scientist data (same formats). Update Sections (NumVertices, NumTriangles,
   BaseIndex, BaseVertexIndex, BoneMap=0..5), NumVertices, RequiredBones/
   ActiveBoneIndices (0..5), buffersSize, ImportedBounds.
4. Rebuild payload; new SerialSize; rename export `kel_lmao`→e.g. `scientist` +
   package path (name-map edit) so it doesn't override the game asset; fix all
   package offsets (ue_pkg writer) + the trailing 4-byte pkg tag.
5. UnrealPak `-Create` → `scientist.pak`, mount root `/Game/Mods/VOTVCoop/`.

## Version flags to confirm (mirror kel's actual bytes, don't assume)
UseNewCookedFormat (true), bNewWeightFormat/UnlimitedBoneInfluences, bInlined
(true), HasRayTracingData, AdjacencyData present, UV precision, NumTexCoords,
FArchive bool size. All resolved by the round-trip (if re-emit == original, the
cursor math + formats are right).

## EMPIRICAL ANCHORS (kel_lmao.uexp, verified) — hard checkpoints for the parser
All offsets are into the payload (= uexp[0:17391]); render region = [2003:17391].
- `FStripDataFlags`(2) @2003-2004.
- `ImportedBounds`(28) @2005-2032: Origin(0, 1.63, 0), BoxExtent(84.42, 30.47, 28.84),
  SphereRadius 84.42 (cm; ~168 cm tall). **[SPLICE from scientist verts]**
- `SkeletalMaterials` @2033: count + 2 materials (`body`, `face`; ~44 B each) → ends @2120.
  (Per-material fields are version-conditional — parse must LAND boneinfo-count exactly @2121.)
- `ReferenceSkeleton` @2121:
  - FinalRefBoneInfo: count=6 @2121; 6× FMeshBoneInfo (12 B: FName 8 + ParentIndex i32)
    @2125,2137,2149,2161,2173,2185 = lowlegs, thighs, pelvis, chest, head, head_end. **[KEEP]**
  - FinalRefBonePose: count=6 @2197; 6× FTransform (40 B) @2201-2440. **[KEEP]**
  - FinalNameToIndexMap: count + N×(FName 8 + i32 4). **[KEEP]**
  - Then `bCooked` + `numLODs`(=1) + LOD `SerializeRenderItem` → `SerializeStreamedData` buffers.

## Concrete field sizes (from the CUE4Parse readers, UE4.27 cooked / FilterEditorOnly)
- bool via `Ar.ReadBoolean()` = int32 (4 B) [validate by round-trip]; FStripDataFlags = 2×u8.
- FSkeletalMaterial: FPackageIndex(4) + MaterialSlotName FName(8) + [bSerializeImported bool +
  ImportedName FName if set] + UVChannelData(FMeshUVChannelInfo) + [OverlayMaterial FPackageIndex?].
  (nail exact size against the @2121 anchor; get FMeshUVChannelInfo.cs if needed.)
- FSkelMeshSection.SerializeRenderItem: strip(2), MaterialIndex(short), BaseIndex(i32),
  NumTriangles(i32), bRecomputeTangent(bool), [RecomputeMaskChannel byte], bCastShadow(bool),
  BaseVertexIndex(u32), ClothMappingDataLODs(1 array: count+FMeshToMeshVertData), BoneMap
  (count+u16), NumVertices(i32), MaxBoneInfluences(i32), CorrespondClothAssetIndex(short),
  ClothingData(FClothingSectionData ~20 B), DupVertData(SkipFixedArray 4)+DupVertIndex
  (SkipFixedArray 8) [if !classStrip(1)], bDisabled(bool).
- FMultisizeIndexContainer: byte dataSize(2|4) + bulk(elemSize i32, count i32, data).
- FPositionVertexBuffer: Stride i32(12) + NumVerts i32 + bulk FVector.
- FStaticMeshVertexBuffer (>=UE4.19): strip(2) + NumTexCoords(i32) + NumVertices(i32) +
  UseFullPrecisionUVs(bool) + UseHighPrecisionTangentBasis(bool) + tangents bulk(itemSize,count)
  + texcoords bulk(itemSize,count). tangents=2×FPackedNormal (4 B each unless high-prec 8);
  UV=FMeshUVHalf(4)|FMeshUVFloat(8) × NumTexCoords.
- FSkinWeightVertexBuffer: strip(2) + metadata (UE4.27 path per FSkinWeightVertexBuffer.cs) +
  bulk FSkinWeightInfo (4 idx bytes + 4 weight bytes for 4-influence). numTexCoords/influences
  read from kel's actual bytes (mirror).
- Still-unknown struct sizes (get if round-trip drifts): FMeshUVChannelInfo, FClothingSectionData,
  FSkinWeightProfilesData, FMeshBoneInfo (confirm 12 B cooked), HasRayTracingData tail.

## PARSE COMPLETE + GENERALIZED (tools/client_model/ue_skelmesh.py)
**Validated on the real target:** `ue_skelmesh` parses ANY UE4.27 cooked
SkeletalMesh (auto property-block end + RefSkeleton auto-locate + count-driven
sections/buffers + `bHasVertexColors` → FColorVertexBuffer) and **ROUND-TRIPS
`kerfurOmega_KelSkin`** (anthro, the real client skin): render@2041, RefSkeleton@2199
(101 bones), **3 sections, 17132 verts, 86055 indices, maxBoneInf=6 (BLENDED),
bHasVertexColors=True**. Still round-trips kel_lmao too. For the SPLICE, run
`python ue_skelmesh.py <kerfurOmega_KelSkin> --verbose` to get the anthro buffer
offsets. NOTE: the scientist is RIGID (1 influence, from skin_to_rig) → the splice
writes rigid skin weights + MaxBoneInfluences=1 into the anthro template (fine).

Confirmed facts for kel_lmao (the original 6-bone parse proof):
- 2 sections: body (BaseIndex 0, 214 tris, 173 verts, BoneMap[5]), face (BaseIndex 642,
  4 tris, 6 verts, BoneMap[1]). MaxBoneInfluences=1 (RIGID) — matches the scientist plan.
- 179 total verts, 654 uint16 indices (218 tris). 1 UV channel, HALF-precision UVs (4 B),
  LOW-precision tangents (8 B = 2×FPackedNormal). SkinWeight new-format, maxBoneInf=4,
  numBones=716, rigid (bone idx + 255). AdjacencyIndexBuffer = 2616 uint16.
- Bools via Ar<<bool = 4 bytes. Strip flags = 2×u8. UseNewCookedFormat + bNewWeightFormat both TRUE.

### SPLICE-TARGET OFFSETS (into payload; keep everything else unchanged)
| field | offset | format to re-encode with scientist data |
|---|---|---|
| ImportedBounds | 2005 (28 B) | Origin+BoxExtent+SphereRadius from scientist verts (cm) |
| Section 0 (body) | 2555 | BaseIndex, NumTriangles, BaseVertexIndex, BoneMap(u16[]), NumVertices |
| Section 1 (face) | 4848 | same (or collapse to 1 section covering the whole scientist) |
| IndexBuffer | 4998 | byte dataSize=2 + bulk(elem2, count) uint16 tri indices |
| PositionBuffer | 6322 | Stride12 + NumVerts + bulk(elem12) FVector (cm; align to kel space) |
| Tangents | 8496 | bulk(elem8) 2×FPackedNormal (packed from scientist normals) |
| TexCoords | 9936 | bulk(elem4) FMeshUVHalf (half-float UVs) |
| SkinWeight newData | 10682 | bulk(elem1) 179*8→N*8: per vert [boneIdx,0,0,0][255,0,0,0] |
| SkinWeight meta | 10666/10670/10674 | maxBoneInf=4, numBones=N*4, numVertices=N |
| AdjacencyIndexBuffer | 12137 | regenerate OR strip the CDSF_AdjacencyData flag to drop it |
Formats: FPackedNormal (`cue4parse_ref/FPackedNormal.cs`), FMeshUVHalf
(`FMeshUVHalf.cs`), FVector=3 float. Simplest for a static-ish scientist: 1 section,
all verts→their Kel bone (from skin_to_rig), weight 255, recompute bounds, drop adjacency.

## Next steps
1. Convert `ue_skelmesh.py` to record byte-ranges → `splice()` that rebuilds the render
   region with scientist buffers (encode index/pos/tangent/UV/skinweight; update sections,
   bounds, skinweight meta; drop or regen adjacency).
2. `ue_pkg` writer: new payload → new export SerialSize; rename kel_lmao→scientist (name map)
   so it doesn't override the game asset; fix all package offsets + trailing pkg tag.
3. UnrealPak `-Create` (`tools/unrealpak/`) → `scientist.pak`, mount root `/Game/Mods/VOTVCoop/`.
4. Mod runtime: mount pak + LoadObject + SetSkeletalMesh/SetAnimClass by role.
