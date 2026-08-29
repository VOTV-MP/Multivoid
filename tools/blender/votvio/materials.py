"""The Blender analog of VotV's material system.

Cooked packages keep no node graphs, but they keep everything that defines the LOOK:
the MIC parameter vocabulary (census over all 3,243 materials: tex/ag/ao/normal/rough/
specular + triplanar tex_x/y/z, scalars emisive_strength/emiss/glow/opacity, vectors
color/emissioncolor/w_absorb/w_scattering), the parent chain (mat_object 1439,
mat_foliageLeaves, mat_decal…, mat_water*), and BasePropertyOverrides
(BLEND_Masked 547 / Translucent 307 / Additive 79, TwoSided 460).
'ag' is the EMISSIVE MASK (measured: *_emiss textures, sparse-bright):
Emission = tex * ag * emisive_strength * emissioncolor.
"""
import bpy

from . import bc_decode

_FALLBACK_RGBA = (0.42, 0.42, 0.46, 1.0)
_WATER_HINTS = ("mat_water", "inst_water", "water1", "waterriver")


def _ref_pkg(v):
    if not isinstance(v, dict):
        return ""
    outer = v.get("OuterIndex") or {}
    return str(outer.get("ObjectName") or v.get("Outer") or "")


def _analyze(game, mat_pkg, cache):
    """Walk the MIC parent chain; leaf wins. -> info dict."""
    if mat_pkg in cache:
        return cache[mat_pkg]
    info = {"tex": {}, "scal": {}, "vec": {}, "blend": "", "twosided": False,
            "clip": 0.3333, "root": "", "chain": []}
    cur = mat_pkg
    for _ in range(8):
        if not cur:
            break
        info["chain"].append(cur)
        info["root"] = cur.rsplit("/", 1)[-1].lower()
        nxt = ""
        for e in game.package_dict(cur):
            if not isinstance(e, dict):
                continue
            ty = e.get("Type")
            if ty not in ("MaterialInstanceConstant", "Material"):
                continue
            pr = e.get("Properties") or {}
            for tv in pr.get("TextureParameterValues") or []:
                nm = str((tv.get("ParameterInfo") or {}).get("Name", "")).lower()
                pkg = _ref_pkg(tv.get("ParameterValue"))
                if nm and pkg and nm not in info["tex"]:
                    info["tex"][nm] = pkg
            for sv in pr.get("ScalarParameterValues") or []:
                nm = str((sv.get("ParameterInfo") or {}).get("Name", "")).lower()
                if nm and nm not in info["scal"]:
                    try:
                        info["scal"][nm] = float(sv.get("ParameterValue", 0.0))
                    except (TypeError, ValueError):
                        pass
            for vv in pr.get("VectorParameterValues") or []:
                nm = str((vv.get("ParameterInfo") or {}).get("Name", "")).lower()
                val = vv.get("ParameterValue") or {}
                if nm and isinstance(val, dict) and nm not in info["vec"]:
                    info["vec"][nm] = (float(val.get("R", 1.0)), float(val.get("G", 1.0)),
                                       float(val.get("B", 1.0)), float(val.get("A", 1.0)))
            bpo = pr.get("BasePropertyOverrides") or {}
            if not info["blend"]:
                bm = bpo.get("BlendMode") or (pr.get("BlendMode") if ty == "Material" else "")
                if bm:
                    info["blend"] = str(bm)
            if bpo.get("TwoSided") or (ty == "Material" and pr.get("TwoSided")):
                info["twosided"] = True
            if bpo.get("OpacityMaskClipValue") is not None:
                try:
                    info["clip"] = float(bpo["OpacityMaskClipValue"])
                except (TypeError, ValueError):
                    pass
            par = _ref_pkg(pr.get("Parent")) if ty == "MaterialInstanceConstant" else ""
            if par.startswith("/Game/"):
                nxt = par
        cur = nxt
    cache[mat_pkg] = info
    return info


def _family(info, mesh_hint):
    names = " ".join(info["chain"]).lower() + " " + mesh_hint
    if "w_absorb" in info["vec"] or any(h in names for h in _WATER_HINTS):
        return "water"
    if "BLEND_Additive" in info["blend"]:
        return "additive"
    if "tex_x" in info["tex"] and "tex_y" in info["tex"]:
        return "triplanar"
    if "foliageleaves" in names or "foliage" in info["root"]:
        return "foliage"
    if "BLEND_Translucent" in info["blend"]:
        return "translucent"
    if "BLEND_Masked" in info["blend"]:
        return "masked"
    return "object"


def _image(game, tex_pkg, caches, warnings, non_color=False):
    key = tex_pkg
    if key in caches["img"]:
        img = caches["img"][key]
    else:
        img = bc_decode.texture_to_image(game, tex_pkg, warnings)
        caches["img"][key] = img
    if img is not None and non_color:
        try:
            img.colorspace_settings.name = "Non-Color"
        except Exception:  # noqa: BLE001
            pass
    return img


def get_material(game, mat_pkg_path, caches, warnings, with_textures=True, mesh_hint=""):
    key = (mat_pkg_path or "<none>", "w" in mesh_hint and "water" in mesh_hint)
    if key in caches["mat"]:
        return caches["mat"][key]
    caches.setdefault("mat_info", {})

    name = (mat_pkg_path.rsplit("/", 1)[-1] if mat_pkg_path else "votv_fallback")
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    caches["mat"][key] = mat
    if bsdf is None:
        return mat
    bsdf.inputs["Roughness"].default_value = 0.62

    info = _analyze(game, mat_pkg_path, caches["mat_info"]) if mat_pkg_path else \
        {"tex": {}, "scal": {}, "vec": {}, "blend": "", "twosided": False,
         "clip": 0.333, "root": "", "chain": []}
    fam = _family(info, mesh_hint.lower())

    if fam == "water":
        _build_water(mat, nt, bsdf, info)
        return mat

    tex = info["tex"]
    scal = info["scal"]
    vec = info["vec"]
    base_pkg = tex.get("tex") or tex.get("diffuse") or tex.get("c1") or tex.get("tex_x") or ""
    img = _image(game, base_pkg, caches, warnings) if (with_textures and base_pkg) else None

    color_socket = None
    if img is not None:
        timg = nt.nodes.new("ShaderNodeTexImage")
        timg.image = img
        timg.location = (-560, 260)
        if fam == "triplanar":
            timg.projection = "BOX"
            timg.projection_blend = 0.35
            coord = nt.nodes.new("ShaderNodeTexCoord")
            coord.location = (-980, 200)
            mapping = nt.nodes.new("ShaderNodeMapping")
            mapping.location = (-780, 200)
            s = scal.get("scale_x") or 0.02
            mapping.inputs["Scale"].default_value = (s, s, s)
            nt.links.new(coord.outputs["Object"], mapping.inputs["Vector"])
            nt.links.new(mapping.outputs["Vector"], timg.inputs["Vector"])
        color_socket = timg.outputs["Color"]
        alpha_socket = timg.outputs["Alpha"]
    else:
        bsdf.inputs["Base Color"].default_value = _FALLBACK_RGBA
        alpha_socket = None

    if color_socket is not None and "color" in vec:
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        mix.blend_type = "MULTIPLY"
        mix.inputs["Factor"].default_value = 1.0
        mix.location = (-300, 260)
        nt.links.new(color_socket, mix.inputs["A"])
        mix.inputs["B"].default_value = vec["color"]
        color_socket = mix.outputs["Result"]
    if color_socket is not None and "ao" in tex and with_textures:
        ao_img = _image(game, tex["ao"], caches, warnings, non_color=True)
        if ao_img is not None:
            aon = nt.nodes.new("ShaderNodeTexImage")
            aon.image = ao_img
            aon.location = (-560, -40)
            mix = nt.nodes.new("ShaderNodeMix")
            mix.data_type = "RGBA"
            mix.blend_type = "MULTIPLY"
            mix.inputs["Factor"].default_value = 0.85
            mix.location = (-140, 200)
            nt.links.new(color_socket, mix.inputs["A"])
            nt.links.new(aon.outputs["Color"], mix.inputs["B"])
            color_socket = mix.outputs["Result"]
    if color_socket is not None:
        nt.links.new(color_socket, bsdf.inputs["Base Color"])

    if with_textures and "normal" in tex:
        nimg = _image(game, tex["normal"], caches, warnings, non_color=True)
        if nimg is not None:
            tn = nt.nodes.new("ShaderNodeTexImage")
            tn.image = nimg
            tn.location = (-560, -320)
            nm = nt.nodes.new("ShaderNodeNormalMap")
            nm.location = (-260, -320)
            nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
            nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    if with_textures and "rough" in tex:
        rimg = _image(game, tex["rough"], caches, warnings, non_color=True)
        if rimg is not None:
            tr = nt.nodes.new("ShaderNodeTexImage")
            tr.image = rimg
            tr.location = (-560, -580)
            nt.links.new(tr.outputs["Color"], bsdf.inputs["Roughness"])

    # emissive: ag mask, or plain emiss/glow scalars
    strength = scal.get("emisive_strength", 0.0) or scal.get("emiss", 0.0) or scal.get("glow", 0.0)
    if strength > 0.01:
        ecol = vec.get("emissioncolor", (1.0, 1.0, 1.0, 1.0))
        emis_socket = color_socket
        if with_textures and "ag" in tex:
            aimg = _image(game, tex["ag"], caches, warnings, non_color=True)
            if aimg is not None:
                ta = nt.nodes.new("ShaderNodeTexImage")
                ta.image = aimg
                ta.location = (-560, 560)
                if color_socket is not None:
                    mul = nt.nodes.new("ShaderNodeMix")
                    mul.data_type = "RGBA"
                    mul.blend_type = "MULTIPLY"
                    mul.inputs["Factor"].default_value = 1.0
                    mul.location = (-140, 560)
                    nt.links.new(color_socket, mul.inputs["A"])
                    nt.links.new(ta.outputs["Color"], mul.inputs["B"])
                    emis_socket = mul.outputs["Result"]
                else:
                    emis_socket = ta.outputs["Color"]
        if emis_socket is not None:
            nt.links.new(emis_socket, bsdf.inputs["Emission Color"])
        else:
            bsdf.inputs["Emission Color"].default_value = ecol
        bsdf.inputs["Emission Strength"].default_value = min(strength * 0.25, 20.0)

    if fam in ("masked", "foliage"):
        if alpha_socket is not None:
            clip = nt.nodes.new("ShaderNodeMath")
            clip.operation = "GREATER_THAN"
            clip.inputs[1].default_value = min(max(info["clip"], 0.05), 0.9)
            clip.location = (-140, -60)
            nt.links.new(alpha_socket, clip.inputs[0])
            nt.links.new(clip.outputs[0], bsdf.inputs["Alpha"])
        if fam == "foliage":
            bsdf.inputs["Roughness"].default_value = 0.75
    elif fam == "translucent":
        op = scal.get("opacity", scal.get("alpha", 0.35))
        bsdf.inputs["Alpha"].default_value = min(max(op, 0.03), 0.95)
        bsdf.inputs["Roughness"].default_value = 0.12
        _set_blended(mat)
    elif fam == "additive":
        if color_socket is not None:
            nt.links.new(color_socket, bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 3.0
        bsdf.inputs["Alpha"].default_value = 0.5
        _set_blended(mat)
    return mat


def _set_blended(mat):
    try:
        mat.surface_render_method = "BLENDED"
    except AttributeError:
        pass


def _build_water(mat, nt, bsdf, info):
    absorb = info["vec"].get("w_absorb", (0.06, 0.18, 0.22, 1.0))
    bsdf.inputs["Base Color"].default_value = (absorb[0], absorb[1], absorb[2], 1.0)
    bsdf.inputs["Roughness"].default_value = 0.05
    bsdf.inputs["Alpha"].default_value = 0.5
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 24.0
    noise.inputs["Detail"].default_value = 6.0
    noise.location = (-560, -200)
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.12
    bump.location = (-260, -200)
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    _set_blended(mat)


# ---------------------------------------------------------------- terrain
TERRAIN_STYLES = {
    "GREEN": ((0.216, 0.302, 0.129), (0.129, 0.192, 0.078), (0.35, 0.33, 0.30)),
    "SNOW": ((0.78, 0.80, 0.83), (0.62, 0.66, 0.72), (0.38, 0.36, 0.34)),
    "DIRT": ((0.33, 0.26, 0.18), (0.24, 0.18, 0.12), (0.36, 0.34, 0.32)),
}


def terrain_material(style):
    a, b, rock = TERRAIN_STYLES.get(style, TERRAIN_STYLES["GREEN"])
    mat = bpy.data.materials.new(f"votv_terrain_{style.lower()}")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    bsdf.inputs["Roughness"].default_value = 0.95
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 0.4
    noise.inputs["Detail"].default_value = 8.0
    noise.location = (-820, 200)
    mixg = nt.nodes.new("ShaderNodeMix")
    mixg.data_type = "RGBA"
    mixg.location = (-560, 200)
    mixg.inputs["A"].default_value = (*a, 1.0)
    mixg.inputs["B"].default_value = (*b, 1.0)
    nt.links.new(noise.outputs["Fac"], mixg.inputs["Factor"])
    # slope -> rock
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    geo.location = (-820, -80)
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    sep.location = (-660, -80)
    nt.links.new(geo.outputs["Normal"], sep.inputs["Vector"])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.location = (-500, -80)
    ramp.color_ramp.elements[0].position = 0.75
    ramp.color_ramp.elements[1].position = 0.92
    nt.links.new(sep.outputs["Z"], ramp.inputs["Fac"])
    mixr = nt.nodes.new("ShaderNodeMix")
    mixr.data_type = "RGBA"
    mixr.location = (-260, 100)
    mixr.inputs["A"].default_value = (*rock, 1.0)
    nt.links.new(mixg.outputs["Result"], mixr.inputs["B"])
    nt.links.new(ramp.outputs["Color"], mixr.inputs["Factor"])
    nt.links.new(mixr.outputs["Result"], bsdf.inputs["Base Color"])
    return mat
