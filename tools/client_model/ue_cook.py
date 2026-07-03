#!/usr/bin/env python3
"""ue_cook -- splice a rigged scientist into the anthro cooked SkeletalMesh template.

Template = kerfurOmega_KelSkin (the real client skin, anthro rig kerfurOmegaV1_Skeleton).
Phase A (here): decompose the template render region into labeled KEEP/REPLACE segments
and prove lossless reassembly (identity round-trip). Phase B: encode the scientist's
buffers (from mdl_extract + the anthro bone map) into the REPLACE segments. Phase C:
rebuild .uasset/.uexp (repath scientist + fix SerialSize/offsets) -> UnrealPak -> pak.
Spec: SPEC.md. Dev/RE tool (RULE 3).
"""
import json
import os
import re
import struct
import sys

import numpy as np

import ue_pkg
import ue_skelmesh as sm

SCI_DIR = r"D:\Projects\Programming\VOTV_MP\research\pak_re\mesh_out\hl_einstein"
# Model keeps its ORIGINAL name (user 2026-07-02): package hl_einstein_v1sc, not "scientist".
OUT = r"D:\Projects\Programming\VOTV_MP\research\pak_re\mesh_out\hl_einstein\hl_einstein_v1sc"
REPOSED = r"D:\Projects\Programming\VOTV_MP\tools\hl_einstein_v1sc\hl_einstein_v1sc_tpose.obj"
ATLAS = os.path.join(SCI_DIR, "atlas.json")   # tools/client_model/atlas.py output

TEMPLATE = r"D:\Projects\Programming\VOTV_MP\research\pak_re\extracted\VotV\Content\meshes\kerfurAnthro\sk\kerfurOmega_KelSkin"


def load_full(base):
    b = open(base + ".uasset", "rb").read()
    c = ue_pkg.parse_summary(b); kv = {t[1]: t[3] for t in c.tok if t[1]}
    o = kv["nameOffset"]; names = []
    for _ in range(kv["nameCount"]):
        ln = struct.unpack_from("<i", b, o)[0]; o += 4
        s = b[o:o + ln - 1].decode("latin1", "replace") if ln > 0 else ""; o += ln if ln > 0 else 0
        o += 4; names.append(s)
    uexp = open(base + ".uexp", "rb").read()
    return b, uexp, names, kv


def parse_segments(payload, names):
    """Return (segs=[(label,start,end)], has_colors). segs cover payload contiguously."""
    segs = []
    rstart, has_colors = sm.property_block_end(payload, names)
    segs.append(("propblock", 0, rstart))
    o = rstart

    def i32():
        nonlocal o; v = struct.unpack_from("<i", payload, o)[0]; o += 4; return v
    def bulk():
        nonlocal o
        es = struct.unpack_from("<i", payload, o)[0]; n = struct.unpack_from("<i", payload, o + 4)[0]
        o += 8 + es * n
    def arr(el):
        nonlocal o; n = struct.unpack_from("<i", payload, o)[0]; o += 4 + n * el

    s = o; o += 2; segs.append(("skelstrip", s, o))
    s = o; o += 28; segs.append(("bounds", s, o))                 # REPLACE
    rs, nb = sm.find_refskel(payload, names, o)
    segs.append(("materials", o, rs)); o = rs                     # KEEP
    s = o; nbi = i32(); o += nbi * 12; nbp = i32(); o += nbp * 40; nnm = i32(); o += nnm * 12
    segs.append(("refskel", s, o))                               # KEEP
    s = o; o += 4; segs.append(("bCooked", s, o))
    s = o; nlod = i32(); segs.append(("numLODs", s, o))
    assert nlod == 1, f"expected 1 LOD, got {nlod}"
    s = o; o += 2 + 4 + 4; segs.append(("lodhdr", s, o))          # strip + bIsLODCookedOut + bInlined
    s = o; arr(2); segs.append(("requiredbones", s, o))          # REPLACE (all bones)
    s = o; nsec = i32()
    for _ in range(nsec):
        o += 2 + 2 + 4 + 4 + 4 + 1 + 4 + 4                        # strip,MatIdx,BaseIdx,NumTri,bRecomp,mask,bCast,BaseVtx
        arr(1); arr(2)                                           # ClothMappingDataLODs, BoneMap
        o += 4 + 4 + 2 + 20                                       # NumVerts,MaxInf,CorrespondCloth,ClothingData
        arr(4); arr(8)                                           # DupVert, DupVertIndex
        o += 4                                                    # bDisabled
    segs.append(("sections", s, o))                             # REPLACE (-> 1 section)
    s = o; arr(2); segs.append(("activebones", s, o))           # REPLACE (all)
    s = o; o += 4; segs.append(("buffersize", s, o))            # REPLACE
    s = o; o += 2; segs.append(("sd_strip", s, o))
    s = o; o += 1; bulk(); segs.append(("index", s, o))         # REPLACE
    s = o; o += 4 + 4; bulk(); segs.append(("position", s, o))  # REPLACE
    s = o; o += 2 + 4 + 4 + 4 + 4; bulk(); bulk(); segs.append(("smvb", s, o))  # REPLACE (tangents+UV)
    s = o; o += 2 + 4 * 5; bulk(); o += 2 + 4; bulk(); segs.append(("skinweight", s, o))  # REPLACE
    if has_colors:
        s = o; o += 2 + 4 + 4; bulk(); segs.append(("color", s, o))  # REPLACE (white)
    s = o; o += 1; bulk(); segs.append(("adjacency", s, o))     # REPLACE (empty)
    segs.append(("tail", o, len(payload)))                      # KEEP
    return segs, has_colors


def seg_mode(base):
    ua, uexp, names, kv = load_full(base)
    payload = uexp[:-4]
    segs, has_colors = parse_segments(payload, names)
    print(f"template: {os.path.basename(base)}  payload={len(payload)}  hasColors={has_colors}  segs={len(segs)}")
    for lbl, s, e in segs:
        print(f"  {lbl:14s} [{s:8d}:{e:8d}]  {e-s}B")
    rebuilt = b"".join(payload[s:e] for _, s, e in segs)
    print(f"identity reassembly: {'OK' if rebuilt == payload else 'MISMATCH'}")


# ---------- Phase B/C: splice the scientist into the template ----------
def read_obj(path):
    V, VT, VN, F, FM = [], [], [], [], []
    mat = None
    for line in open(path):
        if line.startswith("v "):
            V.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("vt "):
            VT.append([float(x) for x in line.split()[1:3]])
        elif line.startswith("vn "):
            VN.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("usemtl"):
            mat = line.split()[1]
        elif line.startswith("f "):
            c = []
            for tok in line.split()[1:]:
                a = tok.split("/")
                c.append((int(a[0]) - 1,
                          int(a[1]) - 1 if len(a) > 1 and a[1] else -1,
                          int(a[2]) - 1 if len(a) > 2 and a[2] else -1))
            for i in range(1, len(c) - 1):
                F.append((c[0], c[i], c[i + 1])); FM.append(mat)
    return (np.array(V, np.float64), (np.array(VT, np.float64) if VT else np.zeros((1, 2))),
            (np.array(VN, np.float64) if VN else np.zeros((0, 3))), F, FM)


def bone_keyword(name):
    """HL/Source bone name -> anthro deform-bone name, or None when no keyword
    matches. Tokens split on space/underscore/dot/dash so 'Bip01 L Arm1',
    'Bip01_L_UpperArm' and 'ValveBiped.Bip01_R_Calf' all resolve; side comes from a
    standalone l/r/left/right token. Callers MUST route None through
    resolve_bone_targets (nearest mapped ancestor) -- the old constant-pelvis
    fallback rigid-glued every unknown bone to the hip (rvi_scientist 2026-07-02:
    toes/fingers/head-prop = 106/764 verts pinned to pelvis, boot toes stayed
    behind the walking foot)."""
    s = name.lower()
    toks = re.split(r"[ _.\-]+", s)
    side = "L" if ("l" in toks or "left" in toks) else \
           ("R" if ("r" in toks or "right" in toks) else None)
    sd = lambda base: f"{base}_{side}" if side else base
    c = re.sub(r"[ _.\-]+", "", s)
    if "head" in s: return "head"
    if "neck" in s: return "neck"
    if "finger" in s or "thumb" in s: return sd("hand")   # anthro fingers exist, but a
    if "hand" in s or "wrist" in s or "ulna" in s: return sd("hand")  # rigid HL mitt rides the hand
    if "forearm" in c or "arm2" in c: return sd("forearm")
    if "upperarm" in c or "arm1" in c or "bicep" in c: return sd("upperarm")
    if "clavicle" in s or "shoulder" in s: return "chest"
    if "arm" in s: return sd("upperarm")
    if "toe" in s: return sd("foot")                      # anthro rig has no toe bones
    if "foot" in s or "ankle" in s: return sd("foot")
    if "calf" in c or "shin" in c or "leg1" in c: return sd("lowerLeg")
    if "thigh" in s or "leg" in s: return sd("thigh")
    if "spine" in s: return "chest" if any(d in c for d in ("1", "2", "3")) else "belly"
    if "pelvis" in s or "hip" in toks or s.strip() == "bip01": return "pelvis"
    return None


def resolve_bone_targets(names, parent, ri):
    """Per-bone anthro target: keyword match, else the NEAREST KEYWORD-MAPPED
    ANCESTOR (an attachment/helper bone follows the limb it hangs off -- e.g.
    rvi_scientist's Bone02 under Dummy08 under Head is head geometry), else pelvis
    counted as a real MISS. Returns [(anthro_name, how)] aligned with names; how is
    'direct', 'ancestor <name>', or 'MISS'. ri = anthro bone name -> index."""
    fallback = "pelvis" if "pelvis" in ri else next(iter(ri))
    direct = []
    for n in names:
        t = bone_keyword(n)
        direct.append(t if t is not None and t in ri else None)
    out = []
    for i in range(len(names)):
        if direct[i]:
            out.append((direct[i], "direct")); continue
        j = parent[i]
        while j >= 0 and direct[j] is None:
            j = parent[j]
        if j >= 0:
            out.append((direct[j], f"ancestor {names[j]}"))
        else:
            out.append((fallback, "MISS"))
    return out


def refskel_bone_names(payload, rs, names):
    n = struct.unpack_from("<i", payload, rs)[0]
    return [names[struct.unpack_from("<i", payload, rs + 4 + i * 12)[0]] for i in range(n)]


def bulk(es, cnt, data):
    return struct.pack("<ii", es, cnt) + data


def pack_pn(x, y, z, w):
    b = lambda c: max(0, min(255, int(round((c + 1) * 127.5))))
    data = b(x) | (b(y) << 8) | (b(z) << 16) | (b(w) << 24)
    return struct.pack("<I", (data ^ 0x80808080) & 0xFFFFFFFF)


def cook(template_base, geom_obj, bones_json, out_base, atlas_json=ATLAS):
    ua, uexp, names, kv = load_full(template_base)
    payload = uexp[:-4]; tag4 = uexp[-4:]
    segs, has_colors = parse_segments(payload, names)
    seg = {l: (s, e) for l, s, e in segs}
    rs = seg["refskel"][0]
    bone_names = refskel_bone_names(payload, rs, names)
    ri = {n: i for i, n in enumerate(bone_names)}; nbones = len(bone_names)
    ps, pe = seg["position"]
    pnv = struct.unpack_from("<i", payload, ps + 4)[0]
    TP = np.frombuffer(payload, "<f4", pnv * 3, ps + 16).reshape(pnv, 3)

    V, VT, VN, F, FM = read_obj(geom_obj)            # reposed T-pose, PSK space
    assert len(VN) and all(t[2] >= 0 for tri in F for t in tri), \
        "geom obj carries no authored normals (vn / f p/t/n) -- re-run mdl_extract+repose"
    meta = json.load(open(bones_json))
    vbone = meta["vert_bone"]; bnames = [b["name"] for b in meta["bones"]]

    # Texture ATLAS (rung 4): the model paints 19 per-mesh textures; the runtime
    # binds ONE ('tex' on the slot-0 MID). atlas.py packed them into one canvas;
    # remap every corner's per-texture UV into its tile's pixel rect.
    # V-orientation: GoldSrc t=0 = texture TOP; the OBJ carries v = 1 - t/th
    # (v-up), while cooked sampling is v-DOWN with mip row 0 = PNG row 0 (ue_tex
    # keeps PIL row order). So undo the OBJ flip (vd = 1-v) and place y-down.
    atl = json.load(open(atlas_json))
    AW, AH = atl["canvas"]; TILES = atl["tiles"]
    missing = sorted({m for m in FM if m not in TILES} | ({None} if None in FM else set()))
    assert not missing, f"faces with no atlas tile: {missing}"

    # PSK-space -> cooked-UE-space is EXACTLY a Y negation (measured from the dr_kel pair:
    # dr_kel.psk points -> cooked kerfurOmega positions, affine residual 0). repose already
    # matched dr_kel's size + placement, so NO rescale/recenter -- just mirror Y.
    G = V.astype(np.float64).copy(); G[:, 1] = -G[:, 1]
    print(f"  scientist bbox {(G.max(0)-G.min(0)).round(1)}  vs template {(TP.max(0)-TP.min(0)).round(1)}")

    # WINDING: match the TEMPLATE's stored index orientation, MEASURED -- never an
    # assumed "geometric-outward = front" convention. Hands-on 2026-07-02: the old
    # outward-normal heuristic left the mesh CCW-outward while the game's own
    # meshes store CW-outward (kerfurOmega_KelSkin signed volume = -161625) ->
    # every front face got culled and the model rendered INSIDE-OUT (look at its
    # front, see the interior of its back). The rung-3 garble had masked it: a
    # closed mesh's silhouette is winding-invariant. Metric = signed volume
    # (divergence theorem): exact for the dominant closed shell, immune to the
    # concavity noise that made the centroid-ray test read 0.469-vs-0.716.
    def signed_vol(P, tris):
        A = P[[t[0] for t in tris]]; B = P[[t[1] for t in tris]]; C = P[[t[2] for t in tris]]
        return float(np.sum(np.einsum("ij,ij->i", A, np.cross(B, C)))) / 6.0
    i0 = seg["index"][0]
    ies, icnt = struct.unpack_from("<ii", payload, i0 + 1)
    TI = np.frombuffer(payload, "<u2" if ies == 2 else "<u4", icnt, i0 + 9).astype(np.int64).reshape(-1, 3)
    sv_t = signed_vol(TP.astype(np.float64), TI)
    fp = [(t[0][0], t[1][0], t[2][0]) for t in F]
    sv_o = signed_vol(G, fp)
    if (sv_o > 0) != (sv_t > 0):
        F = [(t[0], t[2], t[1]) for t in F]; fp = [(a, c, b) for a, b, c in fp]
        sv_o = signed_vol(G, fp)
        print(f"  winding REVERSED to match template (tplVol={sv_t:+.0f}, ours now {sv_o:+.0f})")
    else:
        print(f"  winding matches template (tplVol={sv_t:+.0f}, ours {sv_o:+.0f})")
    # SHADING normals are the MDL's AUTHORED ones (studiomdl smoothing groups),
    # carried through repose (rotated per-bone) -- NEVER recomputed from faces:
    # the scientist suit mesh has ~18% inconsistent-winding edges, so a winding-
    # based accumulation flips ~27% of its vertex normals (dark-from-some-angles
    # suit, user 2026-07-03). Positions were Y-mirrored into cooked space above;
    # mirror the normals the same way (unit length survives a reflection).
    NA = VN.astype(np.float64).copy(); NA[:, 1] = -NA[:, 1]
    # Bone targets via keyword + nearest-mapped-ancestor (resolve_bone_targets).
    # The accounting is per RESOLUTION KIND -- the old counter tested "target not in
    # refskel", which the constant-pelvis fallback made 0 by construction while 106
    # rvi verts sat mis-skinned (2026-07-02): count how each bone RESOLVED instead.
    bparent = [bn["parent"] for bn in meta["bones"]]
    targets = resolve_bone_targets(bnames, bparent, ri)
    pbone = np.array([ri[targets[vbone[p]][0]] for p in range(len(V))])
    vcnt = np.bincount(np.asarray(vbone), minlength=len(bnames))
    n_anc = n_miss = 0
    for bi in range(len(bnames)):
        if not vcnt[bi]:
            continue
        tgt, how = targets[bi]
        if how != "direct":
            print(f"  bone-map: '{bnames[bi]}' -> {tgt} ({how}, {int(vcnt[bi])} verts)")
        if how == "MISS":
            n_miss += int(vcnt[bi])
        elif how != "direct":
            n_anc += int(vcnt[bi])
    print(f"  HL->anthro: {len(set(pbone.tolist()))} anthro bones used; "
          f"{len(V) - n_anc - n_miss}/{len(V)} verts direct, {n_anc} via-ancestor, "
          f"{n_miss} MISS")
    if n_miss:
        print(f"  WARNING: {n_miss} verts have no mapped bone anywhere up their chain -- "
              f"pinned to the pelvis, they will NOT follow any limb in-game.")

    vmap = {}; POS = []; UVs = []; NR = []; BN = []; IDX = []
    for tri, mat in zip(F, FM):
        tx, ty, tw2, th2 = TILES[mat]
        for (p, uv, ni) in tri:
            k = (p, uv, mat, ni); vid = vmap.get(k)
            if vid is None:
                vid = len(POS); vmap[k] = vid
                POS.append(G[p]); NR.append(NA[ni]); BN.append(int(pbone[p]))
                u, v = VT[uv] if 0 <= uv < len(VT) else (0.0, 0.0)
                uc = min(max(u, 0.0), 1.0); vd = 1.0 - min(max(v, 0.0), 1.0)
                UVs.append(((tx + uc * tw2) / AW, (ty + vd * th2) / AH))
            IDX.append(vid)
    print(f"  atlas remap: {len(set(FM))} tiles used over {len(F)} faces (canvas {AW}x{AH})")
    nv = len(POS); ntri = len(IDX) // 3
    POS = np.array(POS, np.float32); NR = np.array(NR, np.float32); UVs = np.array(UVs, np.float32)
    IDX = np.array(IDX, np.uint32)

    S = b"\x01\x00"                                  # strip: global=1(editor) class=0
    e_index = b"\x02" + bulk(2, len(IDX), IDX.astype("<u2").tobytes())
    e_position = struct.pack("<ii", 12, nv) + bulk(12, nv, POS.astype("<f4").tobytes())
    tang = bytearray()
    for i in range(nv):
        n = NR[i]; tx = np.cross(n, [0, 0, 1])
        if np.linalg.norm(tx) < 1e-4: tx = np.cross(n, [0, 1, 0])
        tx = tx / (np.linalg.norm(tx) + 1e-8)
        tang += pack_pn(tx[0], tx[1], tx[2], 0) + pack_pn(n[0], n[1], n[2], 1)
    uvb = bytearray()
    for i in range(nv):
        uvb += struct.pack("<ee", float(UVs[i][0]), float(UVs[i][1]))
    e_smvb = S + struct.pack("<ii", 1, nv) + struct.pack("<ii", 0, 0) + bulk(8, nv, bytes(tang)) + bulk(4, nv, bytes(uvb))
    sw = bytearray()
    for i in range(nv):
        sw += bytes([BN[i] & 0xFF, 0, 0, 0, 255, 0, 0, 0])
    e_skin = (S + struct.pack("<IIIII", 0, 4, nv * 4, nv, 0) + bulk(1, nv * 8, bytes(sw))
              + S + struct.pack("<i", 0) + bulk(4, 0, b""))
    e_color = (S + struct.pack("<ii", 4, nv) + bulk(4, nv, b"\xff\xff\xff\xff" * nv)) if has_colors else b""
    e_adj = b"\x02" + bulk(2, 0, b"")
    bonemap = struct.pack("<i", nbones) + b"".join(struct.pack("<H", i) for i in range(nbones))
    clothing = b"\x00" * 16 + struct.pack("<i", -1)
    e_sections = struct.pack("<i", 1) + (
        S + struct.pack("<h", 0) + struct.pack("<i", 0) + struct.pack("<i", ntri)
        + struct.pack("<i", 0) + b"\x00" + struct.pack("<i", 1) + struct.pack("<I", 0)
        + struct.pack("<i", 0) + bonemap + struct.pack("<i", nv) + struct.pack("<i", 1)
        + struct.pack("<h", -1) + clothing + struct.pack("<i", 0) + struct.pack("<i", 0) + struct.pack("<i", 0))
    e_bones = struct.pack("<i", nbones) + b"".join(struct.pack("<h", i) for i in range(nbones))
    streamed = payload[seg["sd_strip"][0]:seg["sd_strip"][1]] + e_index + e_position + e_smvb + e_skin + e_color + e_adj
    e_buffersize = struct.pack("<I", len(streamed))

    keep = lambda l: payload[seg[l][0]:seg[l][1]]
    new_payload = (keep("propblock") + keep("skelstrip") + keep("bounds") + keep("materials") + keep("refskel")
                   + keep("bCooked") + keep("numLODs") + keep("lodhdr")
                   + e_bones + e_sections + e_bones + e_buffersize + streamed + keep("tail"))
    new_uexp = new_payload + tag4
    ths = kv["totalHeaderSize"]
    new_ua = bytearray(ua)
    struct.pack_into("<q", new_ua, kv["exportOffset"] + 28, len(new_payload))   # SerialSize
    old_bulk = ths + len(payload); new_bulk = ths + len(new_payload)
    idx = bytes(new_ua).find(struct.pack("<q", old_bulk))
    if idx >= 0:
        struct.pack_into("<q", new_ua, idx, new_bulk)
    else:
        print("WARN: BulkDataStartOffset not found")
    open(out_base + ".uasset", "wb").write(bytes(new_ua))
    open(out_base + ".uexp", "wb").write(new_uexp)
    print(f"cook: nv={nv} tris={ntri} bones={nbones}; payload {len(payload)}->{len(new_payload)}")
    print(f"  wrote {out_base}.uasset ({len(new_ua)}B) + .uexp ({len(new_uexp)}B)")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "seg":
        seg_mode(sys.argv[2] if len(sys.argv) > 2 else TEMPLATE)
    else:
        cook(TEMPLATE, REPOSED, os.path.join(SCI_DIR, "model.bones.json"), OUT)


if __name__ == "__main__":
    main()
