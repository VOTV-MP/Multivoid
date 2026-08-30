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

from . import bc_decode, convert, screens
from .decals import _seed_int

_FALLBACK_RGBA = (0.42, 0.42, 0.46, 1.0)

# The game's actual water-surface BASE materials (measured census of materials/
# named *water*). Family membership = the MIC chain's ROOT is one of these (or
# the instance carries w_absorb). Never a substring test: "mat_watermelon"
# contains "mat_water" and the watermelon is not water (2026-08-29 field bug).
_WATER_ROOTS = {"mat_water", "mat_water2", "mat_waterriver", "mat_frozenwater",
                "mat_bucketwater", "mat_gldsrcquakewater"}

# Meshes cooked with UE's placeholder in a slot (the game assigns the real
# material at runtime, unreachable from cook). Curated by measurement:
# d_window's panes (the base main window) carry WorldGridMaterial in all slots;
# the BP's ImportMap names inst_newwindow as the runtime glass.
_PLACEHOLDER = "/Engine/EngineMaterials/WorldGridMaterial"
DIRTY_GLASS = "votvio://dirty_glass"
# inst_newwindow's own defaults (measured): the washable-dirt canvas (three
# pane slices in one 1645x512 sheet, mapped by the panes' own UVs) + the
# glass tint / opacity / roughness shaping constants.
_WINDOW_DIRT_TEX = "/Game/meshes/base2/tex_windowDirtDefault"
_WINDOW_GLASS_TINT = (0.611, 0.708, 0.667, 1.0)
PLACEHOLDER_SLOT_OVERRIDES = (
    ("newbaseWindow2_sig2", DIRTY_GLASS),
)

# RT/logic-driven screen surfaces live in screens.py (extracted at the 800-LOC
# soft cap); the root-prefix set + the per-root still-frame builders are there.


def resolve_slot(mesh_basename, mat_path):
    """Swap a cooked placeholder slot for the curated runtime material."""
    if mat_path == _PLACEHOLDER:
        low = mesh_basename.lower()
        for prefix, repl in PLACEHOLDER_SLOT_OVERRIDES:
            if low.startswith(prefix.lower()):
                return repl
    return mat_path


def _ref_pkg(v):
    if not isinstance(v, dict):
        return ""
    outer = v.get("OuterIndex") or {}
    return str(outer.get("ObjectName") or v.get("Outer") or "")


def _analyze(game, mat_pkg, cache):
    """Walk the MIC parent chain; leaf wins. -> info dict."""
    if mat_pkg in cache:
        return cache[mat_pkg]
    info = {"tex": {}, "scal": {}, "vec": {}, "switch": {}, "blend": "",
            "twosided": False, "clip": 0.3333, "root": "", "chain": []}
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
            for sw in (pr.get("StaticParameters") or {}).get("StaticSwitchParameters") or []:
                nm = str((sw.get("ParameterInfo") or {}).get("Name", "")).lower()
                if nm and sw.get("bOverride") and nm not in info["switch"]:
                    info["switch"][nm] = bool(sw.get("Value"))
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


def _family(info):
    names = " ".join(info["chain"]).lower()
    if "w_absorb" in info["vec"] or info["root"] in _WATER_ROOTS:
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


def get_digit_pair_material(game, num, caches, warnings, with_textures=True):
    """A clock digit-pair quad showing the two digits of num (the save's own
    HH / MM - the game's animated mat_clockMat as a stale frame)."""
    key = ("digitpair", int(num) % 100)
    if key in caches["mat"]:
        return caches["mat"][key]
    mat = bpy.data.materials.new(f"votv_digits_{key[1]:02d}")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    caches["mat"][key] = mat
    if bsdf is not None and with_textures:
        screens.build_digit_pair(game, mat, nt, bsdf, key[1], caches, warnings,
                                 _image)
    return mat


def get_material(game, mat_pkg_path, caches, warnings, with_textures=True):
    key = mat_pkg_path or "<none>"
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
    if mat_pkg_path == DIRTY_GLASS:
        _build_dirty_glass(game, mat, nt, bsdf, caches, warnings, with_textures)
        return mat

    info = _analyze(game, mat_pkg_path, caches["mat_info"]) if mat_pkg_path else \
        {"tex": {}, "scal": {}, "vec": {}, "blend": "", "twosided": False,
         "clip": 0.333, "root": "", "chain": []}
    fam = _family(info)

    if info["root"].startswith(screens.ROOT_PREFIXES):
        screens.build_screen(game, mat, nt, bsdf, info["root"], name.lower(),
                             caches, warnings, with_textures, _image)
        return mat
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
            # Object coords are world-scaled: divide by the import Scale so the
            # texture covers the same game-world area at any scene size
            s = (scal.get("scale_x") or 0.02) / convert.factor()
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

    # emissive: the mat_object family gates emission on the useEmissive
    # STATIC SWITCH (measured: inst_banana carries ag=tex_ceillampMask +
    # strength=100 with useEmissive=FALSE - a cargo-cult clone of a lamp MIC
    # that glows nothing in-game; inst_alamp2_on has useEmissive=TRUE; the
    # parent default is off - no override anywhere in a chain means dark).
    # The ag mask requirement stays: the parent's DEFAULT ag is the engine
    # Black texture, so strength without a mask is equally dead.
    strength = scal.get("emisive_strength", 0.0) or scal.get("emiss", 0.0) or scal.get("glow", 0.0)
    switch_ok = info["switch"].get("useemissive", False) \
        if "emisive_strength" in scal else True
    if strength > 0.01 and switch_ok and with_textures and "ag" in tex:
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
            nt.links.new(emis_socket, bsdf.inputs["Emission Color"])
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


def get_decal_material(game, mat_pkg_path, caches, warnings, with_textures=True,
                       seed="", alpha_mult=1.0):
    """Alpha-blended quad material for a DecalComponent (grime/graffiti/stains).
    Base color from the first texture param; alpha = texture alpha x opacity,
    or the 'mask' texture when the decal carries one (blood family).
    mat_dynamicWallDirt samples a WINDOW of its big overlay sheet (measured:
    size=2048, decalScale=200 half-extents -> a 400/2048 window, anchored by a
    per-instance runtime offset) - windowed here with a seeded offset bucket."""
    caches.setdefault("mat_info", {})
    info = _analyze(game, mat_pkg_path, caches["mat_info"]) if mat_pkg_path else \
        {"tex": {}, "scal": {}, "vec": {}, "blend": "", "twosided": False,
         "clip": 0.333, "root": "", "chain": []}
    windowed = info["root"] == "mat_dynamicwalldirt"
    bucket = (_seed_int(seed) % 12) if windowed else 0
    alpha_mult = min(max(float(alpha_mult), 0.0), 1.0)
    key = ("decal", mat_pkg_path or "<none>", bucket, round(alpha_mult, 2))
    if key in caches["mat"]:
        return caches["mat"][key]
    name = "decal_" + (mat_pkg_path.rsplit("/", 1)[-1] if mat_pkg_path else "none")
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    caches["mat"][key] = mat
    if bsdf is None:
        return mat
    bsdf.inputs["Roughness"].default_value = 0.8
    # a deferred decal modulates the receiver's albedo and adds no specular
    # film of its own; Principled's default specular made every translucent
    # sheet visible as a glossy plane at glancing angles
    spec = bsdf.inputs.get("Specular IOR Level")
    if spec is not None:
        spec.default_value = 0.0
    _set_blended(mat)
    # two-sided sheet pairs wind toward their own viewer side; culling the
    # backface keeps the far-side sheet from ghosting through thin walls
    mat.use_backface_culling = True

    tex = info["tex"]
    base_pkg = tex.get("tex") or tex.get("diffuse") or \
        (next(iter(tex.values())) if tex else "")
    img = _image(game, base_pkg, caches, warnings) if (with_textures and base_pkg) else None
    if img is None:
        bsdf.inputs["Base Color"].default_value = (0.25, 0.22, 0.20, 1.0)
        bsdf.inputs["Alpha"].default_value = 0.35
        return mat
    timg = nt.nodes.new("ShaderNodeTexImage")
    timg.image = img
    timg.location = (-560, 260)
    # mat_decal_grunge draws UNLIT: the texture's own RGB, straight. The old
    # x0.5 darkening was calibrated while decals were invisible for OTHER
    # reasons (inverted receiver normals + coincident duplicate sheets, both
    # fixed) and turned the game's gray-green grime near-black - field
    # comparison showed charcoal drips and a mud-brown palette. RULE 2.
    if windowed:
        # decalScale*2/size of the sheet per axis, seeded window origin
        span = 2.0 * float(info["vec"].get("decalscale", (200.0,) * 4)[1]) / \
            max(float(info["scal"].get("size", 2048.0) or 2048.0), 1.0)
        span = min(max(span, 0.02), 1.0)
        ox = (_seed_int(str(bucket) + ":wx") % 1000) / 1000.0 * (1.0 - span)
        oy = (_seed_int(str(bucket) + ":wy") % 1000) / 1000.0 * (1.0 - span)
        coord = nt.nodes.new("ShaderNodeTexCoord")
        coord.location = (-980, 260)
        mapping = nt.nodes.new("ShaderNodeMapping")
        mapping.location = (-780, 260)
        mapping.inputs["Location"].default_value = (ox, oy, 0.0)
        mapping.inputs["Scale"].default_value = (span, span, 1.0)
        nt.links.new(coord.outputs["UV"], mapping.inputs["Vector"])
        nt.links.new(mapping.outputs["Vector"], timg.inputs["Vector"])
    color_socket = timg.outputs["Color"]
    if "color" in info["vec"]:
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        mix.blend_type = "MULTIPLY"
        mix.inputs["Factor"].default_value = 1.0
        mix.location = (-300, 260)
        nt.links.new(color_socket, mix.inputs["A"])
        mix.inputs["B"].default_value = info["vec"]["color"]
        color_socket = mix.outputs["Result"]
    nt.links.new(color_socket, bsdf.inputs["Base Color"])

    alpha_socket = timg.outputs["Alpha"]
    if "mask" in tex and with_textures:
        mimg = _image(game, tex["mask"], caches, warnings, non_color=True)
        if mimg is not None:
            tm = nt.nodes.new("ShaderNodeTexImage")
            tm.image = mimg
            tm.location = (-560, -60)
            alpha_socket = tm.outputs["Color"]
    # the game CLIPS decal opacity: every decal chain in the pak carries
    # BasePropertyOverrides.OpacityMaskClipValue=0.3333 (probe_v9), and the
    # in-game ground truth agrees - sub-clip texels draw NOTHING (74.7% of the
    # dyn dirt sheet sits at alpha 0.1..0.33 and the game shows no film there),
    # so only stain/drip CORES render. Without this, 257 grime_dyn decals were
    # 2.2m smoky squares plastered over walls, doors and props. v8d's
    # CheapContrast-over-texture-alpha is retired whole: numerically it
    # approximated this clip below the threshold, but the material's real
    # CheapContrasts sit over LinearGradients (FunctionInfos order), not the
    # texture alpha.
    clip = float(info.get("clip", 0.3333) or 0.0)
    if clip > 0.001:
        gate = nt.nodes.new("ShaderNodeMath")
        gate.operation = "GREATER_THAN"
        gate.inputs[1].default_value = clip
        gate.location = (-320, -180)
        nt.links.new(alpha_socket, gate.inputs[0])
        keep = nt.nodes.new("ShaderNodeMath")
        keep.operation = "MULTIPLY"
        keep.location = (-320, -60)
        nt.links.new(alpha_socket, keep.inputs[0])
        nt.links.new(gate.outputs[0], keep.inputs[1])
        alpha_socket = keep.outputs[0]
    op = info["scal"].get("opacity", info["scal"].get("alpha", 1.0))
    mul = nt.nodes.new("ShaderNodeMath")
    mul.operation = "MULTIPLY"
    mul.inputs[1].default_value = min(max(op, 0.0), 1.0) * alpha_mult
    mul.location = (-140, -60)
    nt.links.new(alpha_socket, mul.inputs[0])
    nt.links.new(mul.outputs[0], bsdf.inputs["Alpha"])
    return mat


def _build_dirty_glass(game, mat, nt, bsdf, caches, warnings, with_textures):
    """The base main window = inst_newwindow (measured): ThinTranslucent glass
    tinted color_glass, with the washable-dirt canvas tex_windowDirtDefault
    (three pane slices, mapped by the panes' own UVs) shaping color, opacity
    (opacity_a=0 -> opacity_b=1) and roughness (0.1 -> 1.0)."""
    bsdf.inputs["Base Color"].default_value = _WINDOW_GLASS_TINT
    bsdf.inputs["Roughness"].default_value = 0.1
    bsdf.inputs["Alpha"].default_value = 0.06
    _set_blended(mat)
    img = _image(game, _WINDOW_DIRT_TEX, caches, warnings) if with_textures else None
    if img is None:
        return
    timg = nt.nodes.new("ShaderNodeTexImage")
    timg.image = img
    timg.location = (-820, 220)
    # dirt amount = canvas luminance (white = dirt), shaped by power_opacity=0.6
    dirt = nt.nodes.new("ShaderNodeMath")
    dirt.operation = "POWER"
    dirt.inputs[1].default_value = 0.6
    dirt.location = (-560, 40)
    nt.links.new(timg.outputs["Color"], dirt.inputs[0])
    # color: glass tint -> dusty gray-brown where dirty
    mix = nt.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MIX"
    mix.location = (-300, 260)
    mix.inputs["A"].default_value = _WINDOW_GLASS_TINT
    mix.inputs["B"].default_value = (0.42, 0.40, 0.36, 1.0)
    nt.links.new(dirt.outputs[0], mix.inputs["Factor"])
    nt.links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    # opacity: lerp(0.06 clear glass, 0.95 dirty) by shaped dirt
    op = nt.nodes.new("ShaderNodeMapRange")
    op.inputs["To Min"].default_value = 0.06
    op.inputs["To Max"].default_value = 0.95
    op.location = (-300, -40)
    nt.links.new(dirt.outputs[0], op.inputs["Value"])
    nt.links.new(op.outputs["Result"], bsdf.inputs["Alpha"])
    rough = nt.nodes.new("ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = 0.1
    rough.inputs["To Max"].default_value = 1.0
    rough.location = (-300, -280)
    nt.links.new(dirt.outputs[0], rough.inputs["Value"])
    nt.links.new(rough.outputs["Result"], bsdf.inputs["Roughness"])



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
_LANDSCAPE_MIC = "/Game/inst_mainLandscape"


def landscape_layer_specs(game, caches):
    """layer name -> (detail texture pkg, repeat in Blender units), read from the
    game's own landscape MIC (inst_mainLandscape: tex_<layer> + size_<layer>
    params, measured: grass=tex_pineGrass2 @ 4096uu, gravel=tex_gravel2 @
    2048uu...)."""
    caches.setdefault("mat_info", {})
    info = _analyze(game, _LANDSCAPE_MIC, caches["mat_info"])
    specs = {}
    for name in ("grass", "gravel", "dirt", "rock", "sand", "forestfloor"):
        tex = info["tex"].get("tex_" + name)
        if tex:
            size_uu = info["scal"].get("size_" + name) or 4096.0
            specs[name] = (tex, max(float(size_uu), 64.0) * convert.SCALE)
    return specs


def landscape_material(game, layers, weight_img, specs, caches, warnings,
                       cache_key):
    """One landscape component set's material: the game's layer detail textures
    tiled in world XY, blended by the component weightmap channels (the mesh
    carries a 0..1 component UV). layers = [(layer_name, channel)]."""
    if cache_key in caches["mat"]:
        return caches["mat"][cache_key]
    mat = bpy.data.materials.new("votv_landscape_" + "_".join(n for n, _c in layers))
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    caches["mat"][cache_key] = mat
    if bsdf is None:
        return mat
    bsdf.inputs["Roughness"].default_value = 0.93

    coord = nt.nodes.new("ShaderNodeTexCoord")
    coord.location = (-1400, 200)

    def layer_color(name, idx):
        tex, repeat = specs[name]
        img = _image(game, tex, caches, warnings)
        if img is None:
            return None
        mapping = nt.nodes.new("ShaderNodeMapping")
        mapping.location = (-1160, 300 - idx * 320)
        s = 1.0 / max(repeat, 0.01)
        mapping.inputs["Scale"].default_value = (s, s, s)
        nt.links.new(coord.outputs["Object"], mapping.inputs["Vector"])
        timg = nt.nodes.new("ShaderNodeTexImage")
        timg.image = img
        timg.extension = "REPEAT"
        timg.location = (-920, 300 - idx * 320)
        nt.links.new(mapping.outputs["Vector"], timg.inputs["Vector"])
        return timg.outputs["Color"]

    known = [(n, c) for n, c in layers if n in specs]
    if not known:
        bsdf.inputs["Base Color"].default_value = (0.216, 0.302, 0.129, 1.0)
        return mat
    base = layer_color(known[0][0], 0)
    if base is None:
        bsdf.inputs["Base Color"].default_value = (0.216, 0.302, 0.129, 1.0)
        return mat

    wsep = None
    if weight_img is not None and len(known) > 1:
        wt = nt.nodes.new("ShaderNodeTexImage")
        wt.image = weight_img
        wt.interpolation = "Linear"
        wt.extension = "EXTEND"
        wt.location = (-1160, 640)
        try:
            weight_img.colorspace_settings.name = "Non-Color"
        except Exception:  # noqa: BLE001
            pass
        nt.links.new(coord.outputs["UV"], wt.inputs["Vector"])
        wsep = nt.nodes.new("ShaderNodeSeparateColor")
        wsep.location = (-920, 640)
        nt.links.new(wt.outputs["Color"], wsep.inputs["Color"])

    chan_socket = {0: "Red", 1: "Green", 2: "Blue"}
    for idx, (name, chan) in enumerate(known[1:], start=1):
        over = layer_color(name, idx)
        if over is None:
            continue
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        mix.blend_type = "MIX"
        mix.location = (-560 + idx * 40, 200)
        nt.links.new(base, mix.inputs["A"])
        nt.links.new(over, mix.inputs["B"])
        if wsep is not None and chan in chan_socket:
            nt.links.new(wsep.outputs[chan_socket[chan]], mix.inputs["Factor"])
        elif wsep is not None:
            # alpha channel rides the image's own Alpha output
            wt_node = wsep.inputs["Color"].links[0].from_node
            nt.links.new(wt_node.outputs["Alpha"], mix.inputs["Factor"])
        else:
            mix.inputs["Factor"].default_value = 0.0
        base = mix.outputs["Result"]
    nt.links.new(base, bsdf.inputs["Base Color"])
    return mat


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
