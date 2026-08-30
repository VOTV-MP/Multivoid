"""Cooked USkeletalMesh reader: bind-pose LOD0 geometry as static data.

pyUE4Parse's own USkeletalMesh stops after the materials array; the render
geometry (what the garage door, kerfur units and every other SkeletalMesh
actor LOOK like) was unread, so SK actors were invisible. This export class
replaces it in the registry (same trick as bsp_model's cooked-UModel reader)
and walks the 4.27 cooked layout to the end of the LOD0 vertex buffers:

  FStripDataFlags, FBoxSphereBounds, TArray<FSkeletalMaterial>
  FReferenceSkeleton (bone FName+parent / bind FTransforms / name->index map)
  bCooked, TArray<FSkeletalMeshLODRenderData>:
    strip, bIsLODCookedOut, bInlined, RequiredBones,
    sections (strip, mat idx16, base idx32, tris, bRecomputeTangent,
              mask channel u8, bCastShadow, base vertex u32,
              cloth mapping (76B each), BoneMap i16[], NumVertices,
              MaxBoneInfluences, cloth asset i16, FClothingSectionData (20B),
              FDuplicatedVerticesBuffer, bDisabled),
    ActiveBoneIndices, buffer size u32,
    strip, index container (u8 type size + bulk), FPositionVertexBuffer,
    FStaticMeshVertexBuffer  <- STOP (weights/colors/adjacency not needed)

Two facts pinned by hex-fit against VOTV's cook (sk_probe/sk_dump 2026-08-30,
then a 27/27 sweep over every SK the map references):
  - FDuplicatedVerticesBuffer serializes as TWO COUNT-FIRST TArrays (uint32
    dup indices, then 8-byte index/length pairs with count == NumVertices),
    NOT as esz-tagged bulk arrays;
  - the custom-version container reads empty (CustomVer == -1), so the
    RecomputeTangentVertexColorMask byte is present via the >=4.15 fallback.
Positions are bind-pose component space; render sections index one shared
vertex/index pool, so a static import needs no skinning at all.
"""
from UE4Parse.Assets.Exports.ExportRegistry import register_export
from UE4Parse.Assets.Exports.UObjects import UObject
from UE4Parse.Assets.Objects.FStripDataFlags import FStripDataFlags
from UE4Parse.Assets.Objects.Meshes.FBoxSphereBounds import FBoxSphereBounds
from UE4Parse.Assets.Exports.SkeletalMesh.FSkeletalMaterial import FSkeletalMaterial
from UE4Parse.Assets.Exports.StaticMesh.FPositionVertexBuffer import FPositionVertexBuffer
from UE4Parse.Assets.Exports.StaticMesh.FStaticMeshVertexBuffer import FStaticMeshVertexBuffer
from UE4Parse.Versions.FRecomputeTangentCustomVersion import FRecomputeTangentCustomVersion


class USkeletalMeshExport(UObject):
    sk = None   # dict(positions, uv_items, full_uv, indices, sections, mats) or None

    def __init__(self, reader):
        super().__init__(reader)
        self.sk = None

    def deserialize(self, validpos):
        super().deserialize(validpos)
        try:
            self.sk = self._parse(self.reader)
        except Exception:  # noqa: BLE001 - a bad asset must not kill the package
            self.sk = None

    def _parse(self, r):
        FStripDataFlags(r)
        FBoxSphereBounds(r)
        mats = r.readTArray(lambda: FSkeletalMaterial(r))
        # FReferenceSkeleton: needed only to advance the cursor
        nbones = r.readInt32()
        if not 0 <= nbones < 65536:
            raise ValueError(f"bones {nbones}")
        for _ in range(nbones):
            r.readFName()
            r.readInt32()                       # parent index
            if not r.is_filter_editor_only:
                r.readFString()                 # editor-only export name
        npose = r.readInt32()
        if npose != nbones:
            raise ValueError(f"pose {npose} != bones {nbones}")
        r.base_stream.seek(40 * npose, 1)       # FTransform: quat + pos + scale
        nmap = r.readInt32()
        if nmap != nbones:
            raise ValueError(f"name map {nmap} != bones {nbones}")
        for _ in range(nmap):
            r.readFName()
            r.readInt32()
        if not r.readBool():                    # bCooked
            raise ValueError("not cooked")
        nlods = r.readInt32()
        if not 1 <= nlods <= 8:
            raise ValueError(f"lods {nlods}")

        # ---- LOD0 only ----
        FStripDataFlags(r)
        cooked_out = r.readBool()
        inlined = r.readBool()
        r.readTArray(r.readInt16)               # RequiredBones
        if cooked_out or not inlined:
            raise ValueError("LOD0 stripped or streamed")
        nsec = r.readInt32()
        if not 1 <= nsec <= 256:
            raise ValueError(f"sections {nsec}")
        mask_byte = FRecomputeTangentCustomVersion().get(r) >= \
            FRecomputeTangentCustomVersion.Type.RecomputeTangentVertexColorMask
        sections = []
        total_verts = 0
        total_tris = 0
        for _ in range(nsec):
            FStripDataFlags(r)
            mat_idx = r.readInt16()
            base_index = r.readInt32()
            num_tris = r.readInt32()
            r.readBool()                        # bRecomputeTangent
            if mask_byte:
                r.readUInt8()
            r.readBool()                        # bCastShadow
            r.readUInt32()                      # BaseVertexIndex (pool is shared)
            ncloth = r.readInt32()
            r.base_stream.seek(76 * ncloth, 1)  # FMeshToMeshVertData each
            r.readTArray(r.readInt16)           # BoneMap
            num_verts = r.readInt32()
            r.readInt32()                       # MaxBoneInfluences
            r.readInt16()                       # CorrespondClothAssetIndex
            r.base_stream.seek(20, 1)           # FClothingSectionData
            c1 = r.readInt32()                  # DupVertData (count-first)
            if not 0 <= c1 <= 20_000_000:
                raise ValueError(f"dup verts {c1}")
            r.base_stream.seek(4 * c1, 1)
            c2 = r.readInt32()                  # DupVertIndexData
            if c2 != num_verts:
                raise ValueError(f"dup index {c2} != verts {num_verts}")
            r.base_stream.seek(8 * c2, 1)
            r.readBool()                        # bDisabled
            sections.append((int(mat_idx), int(base_index) // 3, int(num_tris)))
            total_verts += num_verts
            total_tris += num_tris

        r.readTArray(r.readInt16)               # ActiveBoneIndices
        r.readUInt32()                          # serialized buffer size
        FStripDataFlags(r)
        dts = r.readUInt8()                     # FMultisizeIndexContainer
        esz = r.readInt32()
        cnt = r.readInt32()
        if dts not in (2, 4) or esz != dts or cnt % 3 or cnt // 3 != total_tris:
            raise ValueError(f"index buffer dts={dts} esz={esz} cnt={cnt}")
        read16 = dts == 2
        indices = [r.readUInt16() if read16 else r.readUInt32() for _ in range(cnt)]
        pvb = FPositionVertexBuffer(r)
        if pvb.NumVertices != total_verts:
            raise ValueError(f"positions {pvb.NumVertices} != {total_verts}")
        vb = FStaticMeshVertexBuffer(r)
        if vb.NumVertices != total_verts:
            raise ValueError(f"uv verts {vb.NumVertices} != {total_verts}")

        mat_paths = []
        for m in mats:
            pkg = ""
            try:
                v = m.Material.GetValue() if m.Material is not None else None
                pkg = str(((v or {}).get("OuterIndex") or {}).get("ObjectName") or "")
            except Exception:  # noqa: BLE001
                pkg = ""
            mat_paths.append(pkg)

        return {
            "positions": pvb.Verts,             # FVector list, bind-pose space
            "uv_items": vb.UV,                  # FStaticMeshUVItem tuple
            "full_uv": bool(vb.UseFullPrecisionUVs),
            "indices": indices,
            "sections": sections,               # (material slot, first tri, tris)
            "mats": mat_paths,
        }


register_export(USkeletalMeshExport, Type="SkeletalMesh")
