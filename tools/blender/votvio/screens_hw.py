"""Powered-on ANALOG instrument sections of the workstation unit faces.

The unit faces are not one flat RT surface: besides the "UI" cutouts that
window the shared ui_consolesAtlas raster (screens_rt), each face carries
dedicated mesh sections with their own shader-driven materials (measured on
the ADS_1_* meshes):

  download  bulbs/bulbs_001 -> mat_polarity / mat_frequency   (the two dials)
  comp      scr1/scr12      -> inst_analogDS_screen_0/1       (mini scopes)
  comp      graph           -> mat_analogDS_graph             (scope strip)
  comp      bulbs_002       -> mat_analogDS_bulbs             (LED row)
  radar     compass         -> inst_scanLines_compass         (tex_compassScr)

Their cooked art is procedural (node graphs over 1x1 grids / noise), so a
powered-on still draws each face the way the reference shots show it: PIL
paints the instrument, and the section gets an unlit-ish emissive material.
The section UVs span 0..1 (measured), so one square texture per section.
"""
import numpy as np

# leaf material name (lowercase, startswith) -> painter kind
SECTION_KINDS = {
    "mat_polarity": "dial_polarity",
    "mat_frequency": "dial_frequency",
    "inst_analogds_screen": "mini_scope",
    "mat_analogds_graph": "graph_strip",
    "mat_analogds_bulbs": "led_row",
    "inst_scanlines_compass": "compass",
}

_DIAL_BG = (8, 6, 4, 255)
_DIAL_WEB = (150, 95, 40, 255)
_TRACE = (255, 205, 90, 255)


def section_kind(leaf):
    leaf = str(leaf).lower()
    for k, v in SECTION_KINDS.items():
        if leaf.startswith(k):
            return v
    return None


def _paint_dial(kind, size=512):
    import math
    from PIL import Image, ImageDraw
    img = Image.new("RGBA", (size, size), _DIAL_BG)
    d = ImageDraw.Draw(img)
    cx = cy = size / 2.0
    rmax = size * 0.48
    for i in range(1, 7):  # concentric rings
        r = rmax * i / 6.0
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=_DIAL_WEB, width=2)
    for k in range(24):    # radial spokes
        a = math.tau * k / 24.0
        d.line([cx, cy, cx + rmax * math.cos(a), cy + rmax * math.sin(a)],
               fill=_DIAL_WEB, width=2)
    if kind == "dial_polarity":
        # the raw-polarity fan: a bright filled sector (reference: upper-left)
        d.pieslice([cx - rmax, cy - rmax, cx + rmax, cy + rmax],
                   200, 250, fill=(255, 205, 90, 160), outline=_TRACE, width=4)
    else:
        # the raw-frequency trace: a sine ribbon across the dial
        pts = []
        for i in range(96 + 1):
            t = i / 96.0
            xx = cx - rmax + 2 * rmax * t
            yy = cy + math.sin(t * math.tau * 1.5) * rmax * 0.55
            dx, dy = xx - cx, yy - cy
            rr = math.hypot(dx, dy)
            if rr > rmax * 0.97:  # keep the ribbon inside the dial
                sc = rmax * 0.97 / rr
                xx, yy = cx + dx * sc, cy + dy * sc
            pts.append((xx, yy))
        d.line(pts, fill=_TRACE, width=6, joint="curve")
    d.ellipse([cx - rmax, cy - rmax, cx + rmax, cy + rmax],
              outline=(210, 170, 110, 255), width=5)
    return img


def _paint_mini_scope(size=256):
    from PIL import Image, ImageDraw
    rng = np.random.default_rng(1973)
    img = Image.new("RGBA", (size, size), (6, 5, 3, 255))
    d = ImageDraw.Draw(img)
    for gx in range(0, size, size // 8):
        d.line([gx, 0, gx, size], fill=(40, 28, 12, 255), width=1)
    for gy in range(0, size, size // 8):
        d.line([0, gy, size, gy], fill=(40, 28, 12, 255), width=1)
    pts, yy = [], size * 0.5
    for xx in range(0, size + 8, 8):
        yy = min(size * 0.8, max(size * 0.2, yy + rng.normal(0, size * 0.06)))
        pts.append((xx, yy))
    d.line(pts, fill=_TRACE, width=3)
    return img


def _paint_graph_strip(w=512, h=128):
    from PIL import Image, ImageDraw
    rng = np.random.default_rng(407)
    img = Image.new("RGBA", (w, h), (6, 5, 3, 255))
    d = ImageDraw.Draw(img)
    pts, yy = [], h * 0.5
    for xx in range(0, w + 6, 6):
        yy = min(h * 0.85, max(h * 0.15, yy + rng.normal(0, h * 0.10)))
        pts.append((xx, yy))
    d.line(pts, fill=_TRACE, width=2)
    return img


def _paint_led_row(w=512, h=32):
    from PIL import Image, ImageDraw
    img = Image.new("RGBA", (w, h), (10, 10, 8, 255))
    d = ImageDraw.Draw(img)
    n = 10
    for i in range(n):
        cx = w * (i + 0.5) / n
        r = h * 0.28
        on = i in (1, 4)  # a couple lit, the rest dark (reference shots)
        col = (110, 255, 110, 255) if on else (18, 60, 18, 255)
        d.ellipse([cx - r, h / 2 - r, cx + r, h / 2 + r], fill=col)
    return img


def _bpy_image(name, pil):
    import bpy
    arr = np.asarray(pil, dtype=np.float32) / 255.0
    rgb = arr[..., :3]
    arr[..., :3] = np.where(rgb <= 0.04045, rgb / 12.92,
                            ((rgb + 0.055) / 1.055) ** 2.4)
    bimg = bpy.data.images.new(name, width=pil.width, height=pil.height, alpha=True)
    bimg.pixels.foreach_set(np.flipud(arr).reshape(-1).astype(np.float32))
    bimg.pack()
    return bimg


def get_section_material(game, caches, warnings, leaf, image_fn):
    """Powered-on material for one analog instrument section, or None."""
    kind = section_kind(leaf)
    if kind is None:
        return None
    key = ("hw_section", kind)
    if key in caches["mat"]:
        return caches["mat"][key]
    import bpy
    mat = bpy.data.materials.new("votv_hw_" + kind)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    caches["mat"][key] = mat
    if bsdf is None:
        return mat
    bsdf.inputs["Base Color"].default_value = (0.010, 0.012, 0.010, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.25
    bimg = None
    try:
        if kind == "compass":
            # the one section with real cooked art
            bimg = image_fn(game, "/Game/textures/ui/tex_compassScr",
                            caches, warnings, non_color=False)
        else:
            ck = ("hw_img", kind)
            bimg = caches["img"].get(ck)
            if bimg is None:
                painter = {"dial_polarity": lambda: _paint_dial("dial_polarity"),
                           "dial_frequency": lambda: _paint_dial("dial_frequency"),
                           "mini_scope": _paint_mini_scope,
                           "graph_strip": _paint_graph_strip,
                           "led_row": _paint_led_row}[kind]
                bimg = _bpy_image("votv_hw_" + kind, painter())
                caches["img"][ck] = bimg
    except Exception as e:  # noqa: BLE001
        warnings.append(f"hw section {kind} failed: {type(e).__name__} {e}")
    if bimg is not None:
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = bimg
        tex.location = (-500, 200)
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 1.6
    return mat
