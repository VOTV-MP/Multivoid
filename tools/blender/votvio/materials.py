"""VotV material -> Blender Principled.

Measured shape: a prop's material is a MaterialInstanceConstant child of mat_object with
texture parameter 'tex' (base color) + 'ag' (mask, ignored in P1). Unknown formats or
missing textures degrade to a flat placeholder color; geometry is never gated (design rule).
"""
import bpy

from . import bc_decode

_PLACEHOLDER_RGBA = (0.55, 0.55, 0.6, 1.0)


def _mic_texture_param(game, mat_pkg_path, wanted=("tex",)):
    """Walk the package's material exports; return a texture package path or ''."""
    first_any = ""
    for e in game.package_dict(mat_pkg_path):
        if not isinstance(e, dict):
            continue
        if e.get("Type") not in ("MaterialInstanceConstant", "Material"):
            continue
        props = e.get("Properties") or {}
        for tv in props.get("TextureParameterValues") or []:
            pinfo = tv.get("ParameterInfo") or {}
            pname = str(pinfo.get("Name", "")).lower()
            val = tv.get("ParameterValue") or {}
            outer = val.get("OuterIndex") or {}
            pkg = str(outer.get("ObjectName") or val.get("Outer") or "")
            if not pkg:
                continue
            if pname in wanted:
                return pkg
            if not first_any:
                first_any = pkg
        parent = props.get("Parent") or {}
        pouter = parent.get("OuterIndex") or {}
        ppkg = str(pouter.get("ObjectName") or parent.get("Outer") or "")
        if not first_any and ppkg.startswith("/Game/"):
            deeper = _mic_texture_param(game, ppkg, wanted)
            if deeper:
                return deeper
    return first_any


def get_material(game, mat_pkg_path, caches, warnings, with_textures=True):
    """caches = {'mat': {}, 'img': {}}; returns a bpy Material (always)."""
    key = mat_pkg_path or "<none>"
    if key in caches["mat"]:
        return caches["mat"][key]

    name = (mat_pkg_path.rsplit("/", 1)[-1] if mat_pkg_path else "votv_placeholder")
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")

    image = None
    if with_textures and mat_pkg_path:
        tex_pkg = _mic_texture_param(game, mat_pkg_path)
        if tex_pkg:
            if tex_pkg in caches["img"]:
                image = caches["img"][tex_pkg]
            else:
                image = bc_decode.texture_to_image(game, tex_pkg, warnings)
                caches["img"][tex_pkg] = image

    if image is not None and bsdf is not None:
        tex_node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        tex_node.image = image
        tex_node.location = (-320, 220)
        mat.node_tree.links.new(tex_node.outputs["Color"], bsdf.inputs["Base Color"])
        mat.node_tree.links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
    elif bsdf is not None:
        bsdf.inputs["Base Color"].default_value = _PLACEHOLDER_RGBA

    caches["mat"][key] = mat
    return mat
