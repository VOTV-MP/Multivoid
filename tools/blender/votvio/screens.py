"""RT/logic screen still frames: the game draws these surfaces at runtime with
dynamic material params; the cooked CachedExpressionData keeps the param
DEFAULTS and the referenced textures (measured per material), so each root gets
a faithful still: the clock's 7-seg digits atlas at num=0 (mat_clockMat, red
unlit), the analog desk displays in their real colors (screen=orange,
graph=yellow, bulbs=green), orange polarity/frequency rings, the 8/2 UI grid,
and the game's own noise textures for the true static surfaces.

Extracted from materials.py (soft-cap rule); image_fn is materials._image.
"""

ROOT_PREFIXES = ("mat_tvscreen", "mat_analogds_", "mat_segmentdigits",
                 "mat_screennoise", "mat_uiscreennoise", "matui_screengrid",
                 "mat_polarity", "mat_frequency", "mat_clockmat")
_TV_STATIC_TEX = "/Game/textures/misc/tex_hugeNoise"
_TILING_NOISE_TEX = "/Game/textures/unsorted/TilingNoise_contrast"
_BULB_NOISE_TEX = "/Game/MSPresets/MSTextures/noise_mask"
_DIGITS_TEX = "/Game/textures/misc/digits"
_DIGIT_DOTS_TEX = "/Game/textures/misc/digit_dots"


def _dark_face(bsdf):
    bsdf.inputs["Base Color"].default_value = (0.010, 0.014, 0.011, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.18


def _emit_tex(game, nt, bsdf, tex_pkg, color, strength, caches, warnings,
              image_fn, shape=None):
    """emission = texture (optionally MapRange-shaped) x color."""
    img = image_fn(game, tex_pkg, caches, warnings, non_color=True)
    if img is None:
        bsdf.inputs["Emission Color"].default_value = color
        bsdf.inputs["Emission Strength"].default_value = strength * 0.4
        return
    timg = nt.nodes.new("ShaderNodeTexImage")
    timg.image = img
    timg.location = (-620, 260)
    src = timg.outputs["Color"]
    if shape is not None:
        mr = nt.nodes.new("ShaderNodeMapRange")
        mr.inputs["From Min"].default_value = shape[0]
        mr.inputs["From Max"].default_value = shape[1]
        mr.clamp = True
        mr.location = (-420, 260)
        nt.links.new(src, mr.inputs["Value"])
        src = mr.outputs["Result"]
    mix = nt.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    mix.inputs["Factor"].default_value = 1.0
    mix.location = (-220, 260)
    nt.links.new(src, mix.inputs["A"])
    mix.inputs["B"].default_value = color
    nt.links.new(mix.outputs["Result"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = strength


def _uv_sep(nt):
    coord = nt.nodes.new("ShaderNodeTexCoord")
    coord.location = (-1120, 0)
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    sep.location = (-940, 0)
    nt.links.new(coord.outputs["UV"], sep.inputs["Vector"])
    return sep


def _grid_line(nt, sock, cells, width_frac):
    """|fract(x*cells)-0.5| > 0.5-width -> 1 on the lines."""
    mul = nt.nodes.new("ShaderNodeMath")
    mul.operation = "MULTIPLY"
    mul.inputs[1].default_value = cells
    nt.links.new(sock, mul.inputs[0])
    frac = nt.nodes.new("ShaderNodeMath")
    frac.operation = "FRACT"
    nt.links.new(mul.outputs[0], frac.inputs[0])
    sub = nt.nodes.new("ShaderNodeMath")
    sub.operation = "SUBTRACT"
    sub.inputs[1].default_value = 0.5
    nt.links.new(frac.outputs[0], sub.inputs[0])
    ab = nt.nodes.new("ShaderNodeMath")
    ab.operation = "ABSOLUTE"
    nt.links.new(sub.outputs[0], ab.inputs[0])
    gt = nt.nodes.new("ShaderNodeMath")
    gt.operation = "GREATER_THAN"
    gt.inputs[1].default_value = 0.5 - width_frac
    nt.links.new(ab.outputs[0], gt.inputs[0])
    return gt.outputs[0]


def _rings(nt, freq):
    """Concentric rings mask around UV center."""
    sep = _uv_sep(nt)
    dx = nt.nodes.new("ShaderNodeMath")
    dx.operation = "SUBTRACT"
    dx.inputs[1].default_value = 0.5
    nt.links.new(sep.outputs["X"], dx.inputs[0])
    dy = nt.nodes.new("ShaderNodeMath")
    dy.operation = "SUBTRACT"
    dy.inputs[1].default_value = 0.5
    nt.links.new(sep.outputs["Y"], dy.inputs[0])
    comb = nt.nodes.new("ShaderNodeCombineXYZ")
    nt.links.new(dx.outputs[0], comb.inputs["X"])
    nt.links.new(dy.outputs[0], comb.inputs["Y"])
    ln = nt.nodes.new("ShaderNodeVectorMath")
    ln.operation = "LENGTH"
    nt.links.new(comb.outputs[0], ln.inputs[0])
    mul = nt.nodes.new("ShaderNodeMath")
    mul.operation = "MULTIPLY"
    mul.inputs[1].default_value = 6.2832 * freq * 2.0
    nt.links.new(ln.outputs["Value"], mul.inputs[0])
    sin = nt.nodes.new("ShaderNodeMath")
    sin.operation = "SINE"
    nt.links.new(mul.outputs[0], sin.inputs[0])
    ab = nt.nodes.new("ShaderNodeMath")
    ab.operation = "ABSOLUTE"
    nt.links.new(sin.outputs[0], ab.inputs[0])
    gt = nt.nodes.new("ShaderNodeMath")
    gt.operation = "GREATER_THAN"
    gt.inputs[1].default_value = 0.92
    nt.links.new(ab.outputs[0], gt.inputs[0])
    return gt.outputs[0]


def _emit_mask(nt, bsdf, sock, color, strength):
    mix = nt.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    mix.inputs["Factor"].default_value = 1.0
    nt.links.new(sock, mix.inputs["A"])
    mix.inputs["B"].default_value = color
    nt.links.new(mix.outputs["Result"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = strength


def _build_digits(game, mat, nt, bsdf, tex_pkg, cell, caches, warnings, image_fn):
    """7-seg digit atlas (10 cells): unlit RED glyph, masked. num default = 0."""
    bsdf.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.4
    img = image_fn(game, tex_pkg, caches, warnings, non_color=True)
    if img is None:
        return
    coord = nt.nodes.new("ShaderNodeTexCoord")
    coord.location = (-1000, 220)
    mapping = nt.nodes.new("ShaderNodeMapping")
    mapping.location = (-800, 220)
    if cell is not None:
        mapping.inputs["Scale"].default_value = (0.1, 1.0, 1.0)
        mapping.inputs["Location"].default_value = (cell * 0.1, 0.0, 0.0)
    nt.links.new(coord.outputs["UV"], mapping.inputs["Vector"])
    timg = nt.nodes.new("ShaderNodeTexImage")
    timg.image = img
    timg.location = (-560, 220)
    nt.links.new(mapping.outputs["Vector"], timg.inputs["Vector"])
    _emit_mask(nt, bsdf, timg.outputs["Color"], (1.0, 0.0, 0.0, 1.0), 3.0)


def build_digit_pair(game, mat, nt, bsdf, num, caches, warnings, image_fn):
    """One clock digit-PAIR quad (measured: clock2 slot1 = the hour pair, slot2
    = the minute pair, each ONE quad with UV 0..1 across TWO digits; the game
    shader splits u at 0.5 and samples the atlas cell from 'num'). Static form:
    tens/ones are constants baked from the save's own time."""
    _dark_face(bsdf)
    bsdf.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    img = image_fn(game, _DIGITS_TEX, caches, warnings, non_color=True)
    if img is None:
        return
    num = int(num) % 100
    tens, ones = num // 10, num % 10
    sep = _uv_sep(nt)
    u2 = nt.nodes.new("ShaderNodeMath")
    u2.operation = "MULTIPLY"
    u2.inputs[1].default_value = 2.0
    nt.links.new(sep.outputs["X"], u2.inputs[0])
    frac = nt.nodes.new("ShaderNodeMath")
    frac.operation = "FRACT"
    nt.links.new(u2.outputs[0], frac.inputs[0])
    cell = nt.nodes.new("ShaderNodeMath")   # local 0..1 -> 0..0.1 within a cell
    cell.operation = "MULTIPLY"
    cell.inputs[1].default_value = 0.1
    nt.links.new(frac.outputs[0], cell.inputs[0])
    right = nt.nodes.new("ShaderNodeMath")  # 0 on the tens half, 1 on the ones
    right.operation = "GREATER_THAN"
    right.inputs[1].default_value = 0.5
    nt.links.new(sep.outputs["X"], right.inputs[0])
    dsel = nt.nodes.new("ShaderNodeMath")   # cell origin: tens/10 or ones/10
    dsel.operation = "MULTIPLY_ADD"
    dsel.inputs[1].default_value = (ones - tens) * 0.1
    dsel.inputs[2].default_value = tens * 0.1
    nt.links.new(right.outputs[0], dsel.inputs[0])
    u = nt.nodes.new("ShaderNodeMath")
    u.operation = "ADD"
    nt.links.new(dsel.outputs[0], u.inputs[0])
    nt.links.new(cell.outputs[0], u.inputs[1])
    comb = nt.nodes.new("ShaderNodeCombineXYZ")
    nt.links.new(u.outputs[0], comb.inputs["X"])
    nt.links.new(sep.outputs["Y"], comb.inputs["Y"])
    timg = nt.nodes.new("ShaderNodeTexImage")
    timg.image = img
    timg.location = (-560, 220)
    nt.links.new(comb.outputs[0], timg.inputs["Vector"])
    _emit_mask(nt, bsdf, timg.outputs["Color"], (1.0, 0.0, 0.0, 1.0), 3.0)


def build_screen(game, mat, nt, bsdf, root, leaf, caches, warnings,
                 with_textures, image_fn):
    """Per-root faithful still frame from the material's own cooked defaults."""
    _dark_face(bsdf)
    if not with_textures:
        return
    if root == "mat_clockmat" or root.startswith("mat_segmentdigits") \
            or leaf.startswith("inst_segmentdigits"):
        tex = _DIGIT_DOTS_TEX if "dots" in leaf else _DIGITS_TEX
        cell = None if "dots" in leaf else 0   # num default = 0 -> '0'
        _build_digits(game, mat, nt, bsdf, tex, cell, caches, warnings, image_fn)
        return
    if root.startswith("mat_tvscreen"):
        # cooked defaults: active=1, static=0 -> the screen shows its CONTENT
        # input (the 8x8 dummy 00000000texture), i.e. a powered-off dark face.
        # (tex_hugeNoise is RGB confetti and only shows when the game raises
        # 'static' at runtime - USER: colored noise here is wrong.)
        return
    if root.startswith(("mat_screennoise", "mat_uiscreennoise")):
        # the true static surfaces: the game's own grayscale tiling noise
        _emit_tex(game, nt, bsdf, _TILING_NOISE_TEX, (0.62, 0.66, 0.62, 1.0), 0.4,
                  caches, warnings, image_fn)
        return
    if root.startswith("mat_analogds_bulbs"):
        _emit_tex(game, nt, bsdf, _BULB_NOISE_TEX, (0.0, 1.0, 0.0, 1.0), 1.0,
                  caches, warnings, image_fn, shape=(0.35, 0.8))
        return
    if root.startswith("mat_analogds_bar"):
        sep = _uv_sep(nt)
        lt = nt.nodes.new("ShaderNodeMath")
        lt.operation = "LESS_THAN"
        lt.inputs[1].default_value = 0.12   # alpha default 0 -> empty bar stub
        nt.links.new(sep.outputs["X"], lt.inputs[0])
        _emit_mask(nt, bsdf, lt.outputs[0], (1.0, 0.25, 0.0, 1.0), 1.2)
        return
    if root.startswith("mat_analogds_graph"):
        # yellow trace line across the middle (color default (1,1,0))
        sep = _uv_sep(nt)
        sub = nt.nodes.new("ShaderNodeMath")
        sub.operation = "SUBTRACT"
        sub.inputs[1].default_value = 0.5
        nt.links.new(sep.outputs["Y"], sub.inputs[0])
        ab = nt.nodes.new("ShaderNodeMath")
        ab.operation = "ABSOLUTE"
        nt.links.new(sub.outputs[0], ab.inputs[0])
        lt = nt.nodes.new("ShaderNodeMath")
        lt.operation = "LESS_THAN"
        lt.inputs[1].default_value = 0.015
        nt.links.new(ab.outputs[0], lt.inputs[0])
        _emit_mask(nt, bsdf, lt.outputs[0], (1.0, 1.0, 0.0, 1.0), 1.6)
        return
    if root.startswith("mat_analogds_screen"):
        # the orange signal field: game's tiling noise shaped to speckle
        _emit_tex(game, nt, bsdf, _TILING_NOISE_TEX, (1.0, 0.25, 0.0, 1.0), 1.1,
                  caches, warnings, image_fn, shape=(0.42, 0.85))
        return
    if root.startswith(("mat_polarity", "mat_frequency")):
        freq = 2.0 if root.startswith("mat_frequency") else 3.0
        _emit_mask(nt, bsdf, _rings(nt, freq), (1.0, 0.25, 0.0, 1.0), 1.4)
        return
    if root.startswith("matui_screengrid"):
        # size=8 cells, width=2 (of 64) - soft green grid
        sep = _uv_sep(nt)
        lu = _grid_line(nt, sep.outputs["X"], 8.0, 2.0 / 64.0)
        lv = _grid_line(nt, sep.outputs["Y"], 8.0, 2.0 / 64.0)
        mx = nt.nodes.new("ShaderNodeMath")
        mx.operation = "MAXIMUM"
        nt.links.new(lu, mx.inputs[0])
        nt.links.new(lv, mx.inputs[1])
        _emit_mask(nt, bsdf, mx.outputs[0], (0.3, 1.0, 0.5, 1.0), 0.8)
        return
    # unknown screen surface: leave the dark CRT face
