#!/usr/bin/env python3
"""skin_to_rig -- auto-rig an extracted mesh onto a VOTV game skeleton (pure Python).

TARGET (corrected 2026-07-01): the ANTHRO rig `kerfurOmegaV1_Skeleton` (~101 bones,
hands/fingers) — the rig the real client skin `kerfurOmega_KelSkin` uses. (`kel_lmao`
6-bone is a stick placeholder — ignore.) GoldSrc/HL1 is 1-bone-per-vertex with Bip01
biped bones, so the auto-rig is a DETERMINISTIC HL-Bip01 → anthro-bone remap (rigid,
weight 1.0) — no geometric guessing. HL humanoid arms/legs map cleanly to the anthro
arms(upperarm/forearm/hand)/legs(thigh/lowerLeg/foot).

Clones the target skeleton (all nodes + inverseBindMatrices) verbatim from the target
glb, aligns the source to the target mesh bbox, assigns each vertex its anthro joint.
Output = skinned .glb (self-validated). The cook (ue_skelmesh splice) later resolves
bones by NAME into kerfurOmega_KelSkin's cooked refskeleton.

Usage: python skin_to_rig.py <extractDir> <target.glb> <out.glb> [--yaw180]
  e.g. target.glb = .../meshes/kerfurAnthro/sk/kerfurOmega_KelSkin.glb
Dev/RE tool (RULE 3); output is derived content -> keep gitignored.
"""
import json
import os
import struct
import sys

import numpy as np
import skin_transfer as st
import ue_cook  # THE one bone resolver (keyword + ancestor walk) lives there


def read_obj(path):
    V, VT, F = [], [], []
    for line in open(path):
        if line.startswith("v "):
            V.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("vt "):
            VT.append([float(x) for x in line.split()[1:3]])
        elif line.startswith("f "):
            c = []
            for tok in line.split()[1:]:
                a = tok.split("/")
                c.append((int(a[0]) - 1, int(a[1]) - 1 if len(a) > 1 and a[1] else -1))
            for i in range(1, len(c) - 1):
                F.append((c[0], c[i], c[i + 1]))
    return np.array(V, np.float64), (np.array(VT, np.float64) if VT else np.zeros((1, 2))), F


def pad4(b):
    b = bytearray(b)
    while len(b) % 4:
        b += b"\0"
    return b


def main():
    if len(sys.argv) < 4:
        print("usage: skin_to_rig <extractDir> <target.glb> <out.glb> [--yaw180]"); return 2
    edir, tgtglb, out = sys.argv[1], sys.argv[2], sys.argv[3]
    yaw180 = "--yaw180" in sys.argv

    V, VT, F = read_obj(os.path.join(edir, "model.obj"))
    meta = json.load(open(os.path.join(edir, "model.bones.json")))
    vert_bone = meta["vert_bone"]; bnames = [b["name"] for b in meta["bones"]]

    # --- target skeleton (clone verbatim) + mesh bbox (align target) ---
    tg, tb = st.load_glb(tgtglb)
    skin = tg["skins"][0]
    joint_names = [tg["nodes"][j].get("name", f"j{j}") for j in skin["joints"]]
    ji = {n: k for k, n in enumerate(joint_names)}   # anthro bone name -> JOINTS_0 index
    ibm = st.accessor(tg, tb, skin["inverseBindMatrices"]).reshape(-1, 16)
    tpos, _, _, _, _ = st.mesh_skin(tg, tb)
    tmin, tmax = tpos.min(0), tpos.max(0)

    bparent = [bn["parent"] for bn in meta["bones"]]
    targets = ue_cook.resolve_bone_targets(bnames, bparent, ji)

    def anthro_joint(hlbone_idx):
        return ji.get(targets[hlbone_idx][0], ji.get("pelvis", 0))

    # --- align source (HL Z-up) -> target mesh space (glTF Y-up), scale to height ---
    hx, hy, hz = V[:, 0], V[:, 1], V[:, 2]
    s = (tmax[1] - tmin[1]) / (hz.max() - hz.min())
    sgn = -1.0 if yaw180 else 1.0
    cx, cz = (tmin[0] + tmax[0]) / 2, (tmin[2] + tmax[2]) / 2
    G = np.empty_like(V)
    G[:, 0] = sgn * (hx - (hx.max() + hx.min()) / 2) * s + cx
    G[:, 1] = (hz - hz.min()) * s + tmin[1]
    G[:, 2] = sgn * (hy - (hy.max() + hy.min()) / 2) * s + cz

    # --- de-index OBJ -> unique glTF verts; rigid weights ---
    vmap = {}; POS = []; UV = []; JNT = []; IDX = []
    for tri in F:
        for (p, uv) in tri:
            key = (p, uv); vid = vmap.get(key)
            if vid is None:
                vid = len(POS); vmap[key] = vid
                POS.append(G[p]); UV.append(VT[uv] if 0 <= uv < len(VT) else (0.0, 0.0))
                JNT.append(anthro_joint(vert_bone[p]))
            IDX.append(vid)
    POS = np.array(POS, np.float32); UV = np.array(UV, np.float32)
    JNT = np.array(JNT, np.uint8); IDX = np.array(IDX, np.uint32)
    J4 = np.zeros((len(POS), 4), np.uint8); J4[:, 0] = JNT
    W4 = np.zeros((len(POS), 4), np.float32); W4[:, 0] = 1.0

    # --- assemble glb ---
    buf = bytearray(); views = []
    def add(ab, target=None):
        b = pad4(ab); off = len(buf); buf.extend(b)
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(ab), **({"target": target} if target else {})})
        return len(views) - 1
    a_pos = add(POS.tobytes(), 34962); a_uv = add(UV.tobytes(), 34962)
    a_jnt = add(J4.tobytes(), 34962); a_w = add(W4.tobytes(), 34962)
    a_idx = add(IDX.tobytes(), 34963); a_ibm = add(ibm.astype(np.float32).tobytes())
    accessors = [
        {"bufferView": a_pos, "componentType": 5126, "count": len(POS), "type": "VEC3", "min": POS.min(0).tolist(), "max": POS.max(0).tolist()},
        {"bufferView": a_uv, "componentType": 5126, "count": len(POS), "type": "VEC2"},
        {"bufferView": a_jnt, "componentType": 5121, "count": len(POS), "type": "VEC4"},
        {"bufferView": a_w, "componentType": 5126, "count": len(POS), "type": "VEC4"},
        {"bufferView": a_idx, "componentType": 5125, "count": len(IDX), "type": "SCALAR"},
        {"bufferView": a_ibm, "componentType": 5126, "count": len(ibm), "type": "MAT4"},
    ]
    # clone ALL target nodes verbatim (strip mesh/skin), append scientist mesh node
    nodes = []
    for n in tg["nodes"]:
        nodes.append({k: n[k] for k in ("name", "children", "translation", "rotation", "scale") if k in n})
    sci = len(nodes); nodes.append({"name": "scientist", "mesh": 0, "skin": 0})
    scene_nodes = list(tg["scenes"][tg.get("scene", 0)]["nodes"]) + [sci]
    gltf = {
        "asset": {"version": "2.0", "generator": "skin_to_rig"},
        "scene": 0, "scenes": [{"nodes": scene_nodes}], "nodes": nodes,
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4}]}],
        "skins": [{"joints": skin["joints"], "inverseBindMatrices": 5}],
        "bufferViews": views, "accessors": accessors, "buffers": [{"byteLength": len(buf)}],
    }
    jb = json.dumps(gltf).encode("utf-8"); jchunk = jb + b" " * ((4 - len(jb) % 4) % 4)
    bchunk = bytes(pad4(buf))
    glb = (struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(jchunk) + 8 + len(bchunk))
           + struct.pack("<II", len(jchunk), 0x4E4F534A) + jchunk
           + struct.pack("<II", len(bchunk), 0x004E4942) + bchunk)
    open(out, "wb").write(glb)

    # --- self-validate ---
    og, ob = st.load_glb(out); p2, _, w2, names2, _ = st.mesh_skin(og, ob)
    bone = w2.argmax(1); counts = np.bincount(bone, minlength=len(names2))
    used = [(names2[i], int(counts[i]), round(float(p2[bone == i, 1].mean()), 2)) for i in range(len(names2)) if counts[i] > 0]
    print(f"[skin_to_rig] wrote {out}  verts={len(POS)} tris={len(IDX)//3} joints={len(names2)}")
    print(f"  weight-sum min/max: {w2.sum(1).min():.3f}/{w2.sum(1).max():.3f}")
    print(f"  bbox: {(p2.max(0)-p2.min(0)).round(3)}  (target ~ {(tmax-tmin).round(3)})")
    print("  verts per bone (name, count, meanY) -- expect head high, foot low, arms present:")
    for nm, ct, my in sorted(used, key=lambda x: -x[2]):
        print(f"    {nm:14s} {ct:5d}  meanY={my}")


if __name__ == "__main__":
    main()
