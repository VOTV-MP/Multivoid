"""Powered-on workstation screens: rasterize the game's own UMG screen widgets
into the CRT materials (the "screens_powered" import option, default ON).

The game draws every workstation monitor by rendering a UMG widget into a
render target and feeding it to mat_tvScreen's `tex` parameter (measured:
scalars brightness=0.5/static=0/active=1, res_X=352/res_Y=288 -- the CRT pixel
grid; textures tex=00000000texture dummy, tex_pixel=tex_rgbPixel). The cooked
pak carries the whole widget TREE (panels/images/texts with layout), the UI
textures, and the terminal font face (FSEX300.ufont) -- so a faithful
powered-on still is: rasterize the widget tree with the game's own art + font,
then emit it through the same CRT chain (content x brightness x RGB pixel
mask).

Routing (measured, and it is ONE texture for everything): ui_consolesAtlas is
ONE 2000x2000 canvas whose quadrants only GROUP the content; each screen
mesh's "UI" section is a SCATTER of rectangular cutouts, each windowing its
own atlas rect by raw UV (UV-island census: comp 2, coords 18 -- including
five PER-DIGIT windows per counter row -- download 8, playback 6, console 1,
radar 5). The widget leans on RenderTransform: the comp table and the desk
logs bake in ROTATED (+-90) and the cutouts read the rotated projection; the
unit face geometry turns them upright again. The SAT console face windows
exactly the embedded umg_console rect (byte-equal to its mesh UVs), the radar
face the embedded umg_radar region. The per-device cover layer
(CanvasPanel_76) and the 14 deathscreen overlays ship Collapsed and stay
skipped by their own cooked Visibility. So: ONE raster, ONE material, six
meshes, raw UV. The faces' NON-RT analog sections (dials, mini scopes, LED
row, compass) are screens_hw's job.

PIL does the rasterizing (bundled with Blender 5.1); everything degrades to
the existing dark-CRT face when PIL, the pak, or a widget is missing.
"""
import io
import re

import numpy as np

from . import bc_decode

# the screen meshes whose "UI" slot samples the one shared atlas raster
SCREEN_MESHES = frozenset((
    "ads_1_comp", "ads_1_coords", "ads_1_download",
    "ads_1_playback", "ads_1_console", "ads_1_radar",
))
_ATLAS_PKG = "VotV/Content/test/ui_consolesAtlas"

# Runtime-painted surfaces (signal image, spectrogram, ping wave, radar
# heightmap sweep...): BLACK at a powered-on idle desk -- never sample their
# design-time placeholder art.
_RUNTIME_IMG_WIDGETS = ("img_object", "img_signal", "img_visualizer")
_RUNTIME_MATS = ("mat_spectr", "instt_distort", "mat_pingwave", "inst_radarheighmap",
                 "mat_radarpath", "mat_radarcut", "mat_screenwarp", "panelstestatlas",
                 "mat_analogds_bar")
# Procedural UI materials the game draws in-shader: reproduce, not sample.
_BORDER_MATS = ("mat_squareborder", "inst_squareborder")
_LINE_MATS = ("inst_coordline", "mat_coordline")
_GRID_MATS = ("mat_coordspacegrid",)
_SPACE_MATS = ("mattt_space",)
# mattT_space's cooked texture is the space2 Milky Way panorama (4096x2048
# equirect), stretched over the whole pannable canvas_spaceSigns surface; the
# retainer clip then shows a window of it -- exactly the grainy field with
# dark dust lanes in the reference shots. The widget shader lifts it hard.
_SPACE_TEX = "/Game/textures/ui/space2"
_SPACE_GAIN = 3.5
# The screen-noise underlay (mat_uiScreenNoise): the game's shader animates a
# grain field; the coords space view tints it (1,0.5,0). A still frame keeps a
# faint deterministic grain instead of skipping it to pure black.
_NOISE_MATS = ("mat_uiscreennoise",)
# UMG Font assets -> the .ufont face payload backing them (measured via the
# Font packages' import maps: font_terminal -> FSEX300, font_analogue -> the
# 7-segment "taximeter" face used by the azimuth counters + filter readouts).
_FONT_FACES = {
    "terminal": "VotV/Content/main/FSEX300.ufont",
    "analogue": "VotV/Content/main/taximeter.ufont",
}
# The game's square/multi border shaders draw in a fixed orange; the color is
# baked into the material node graph (no vector params, no widget tint), so it
# is reproduced as a constant sampled from the in-game reference shots.
_BORDER_RGB = (205, 125, 45)
_PIXEL_TEX = "/Game/textures/misc/tex_rgbPixel"
# mat_tvScreen cooked defaults: brightness 0.5, CRT grid 352x288
_BRIGHTNESS = 0.5
_RES_X, _RES_Y = 352.0, 288.0
_MAX_CANVAS = 1600          # raster cap (the atlas canvas is 2000 design px)
_SLATE_PX = 4.0 / 3.0       # Slate font size (pt @96dpi) -> px

_HIDDEN = ("ESlateVisibility::Hidden", "ESlateVisibility::Collapsed")

# Design-time placeholder texts the game replaces at init (USER: "9999 это
# фейк числа до того как игра инициализирует всё как надо") -- show the
# initialized idle instead. Name overrides first, then the numeric rule.
_TEXT_OVERRIDES = {
    "txt_vol": "100",             # playback volume readout
    "text_playSignalText": "",    # playback text pane: empty until a signal plays
    "richtxt_consoleLog": "",     # '-1 console begin' x14: design-time filler
    "RichTextBlock_coordLog": ">coordinates log",  # the log's real idle line
    "tex_consoleName": "",        # 'NAME': the unit name arrives at runtime
}
_FAKE_NUM = re.compile(r"9{3,}|8{4,}|7{4,}")


def _init_text(name, txt):
    if _FAKE_NUM.search(txt):
        # zero every number KEEPING ITS LENGTH: the coords counters are shown
        # through per-DIGIT mesh cutouts (5 windows per row, measured), so
        # '99999' must stay 5 glyphs ('00000'), and '-999.99' -> '000.00'.
        txt = re.sub(r"-?\d+\.\d+",
                     lambda m: re.sub(r"\d", "0", m.group()).lstrip("-"), txt)
        txt = re.sub(r"-?\d{3,}",
                     lambda m: "0" * len(m.group().lstrip("-")), txt)
    return txt


def _ref(v):
    return v.get("ObjectName") if isinstance(v, dict) else None


def _ref_pkg(v):
    """A cooked object ref -> its package path ('/Game/...') or None."""
    if not isinstance(v, dict):
        return None
    outer = v.get("Outer")
    if isinstance(outer, str) and outer.startswith("/Game/"):
        return outer
    oi = v.get("OuterIndex")
    if isinstance(oi, dict):
        nm = oi.get("ObjectName")
        if isinstance(nm, str) and nm.startswith("/Game/"):
            return nm
    return None


def _color(v, default=(1.0, 1.0, 1.0, 1.0)):
    c = (v or {}).get("SpecifiedColor") if isinstance(v, dict) else None
    if not isinstance(c, dict):
        return default
    return (float(c.get("R", 1.0)), float(c.get("G", 1.0)),
            float(c.get("B", 1.0)), float(c.get("A", 1.0)))


def _srgb8(c):
    """Linear FLinearColor -> sRGB 0..255 tuple (UMG colors are linear)."""
    out = []
    for i, x in enumerate(c):
        x = max(0.0, min(1.0, float(x)))
        if i < 3:
            x = x * 12.92 if x <= 0.0031308 else 1.055 * (x ** (1 / 2.4)) - 0.055
        out.append(int(round(x * 255.0)))
    return tuple(out)


class _Tree:
    """One widget package parsed into paint-ready form."""

    def __init__(self, game, pkg, warnings):
        self.game = game
        self.warnings = warnings
        self.by_name = {}          # widget exports by name (unique per WidgetTree)
        self.slots = {}            # (slot name, panel name) -> slot export
        self.root = None
        for e in game.package_dict(pkg) or []:
            t = str(e.get("Type") or "")
            nm = e.get("Name")
            if t == "WidgetTree":
                self.root = _ref((e.get("Properties") or {}).get("RootWidget"))
            elif t.endswith("Slot"):
                self.slots[(nm, e.get("Outer"))] = e
            else:
                self.by_name.setdefault(nm, e)

    def widget(self, name):
        return self.by_name.get(name)

    def children(self, panel):
        """Content widgets of a panel, in Slots (= paint) order."""
        out = []
        for sref in (panel.get("Properties") or {}).get("Slots") or []:
            se = self.slots.get((_ref(sref), panel.get("Name")))
            if se is None:
                continue
            c = _ref((se.get("Properties") or {}).get("Content"))
            w = self.by_name.get(c)
            if w is not None:
                out.append((se, w))
        return out


def _slot_rect(slot, parent):
    """UMG CanvasPanelSlot layout -> (x, y, w, h) inside parent (x,y,w,h)."""
    px, py, pw, ph = parent
    ld = ((slot.get("Properties") or {}).get("LayoutData")) or {}
    off = ld.get("Offsets") or {}
    an = ld.get("Anchors") or {}
    al = ld.get("Alignment") or {}
    L = float(off.get("Left", 0.0))
    T = float(off.get("Top", 0.0))
    R = float(off.get("Right", 0.0))
    B = float(off.get("Bottom", 0.0))
    amin = an.get("Minimum") or {}
    amax = an.get("Maximum") or {}
    ax0, ay0 = float(amin.get("X", 0.0)), float(amin.get("Y", 0.0))
    ax1, ay1 = float(amax.get("X", 0.0)), float(amax.get("Y", 0.0))
    alx, aly = float(al.get("X", 0.0)), float(al.get("Y", 0.0))
    if ax0 == ax1:                      # point anchor: L=pos, R=SIZE
        w = R
        x = px + ax0 * pw + L - alx * w
    else:                               # stretch: L/R are margins
        x = px + ax0 * pw + L
        w = (px + ax1 * pw - R) - x
    if ay0 == ay1:
        h = B
        y = py + ay0 * ph + T - aly * h
    else:
        y = py + ay0 * ph + T
        h = (py + ay1 * ph - B) - y
    return (x, y, max(w, 0.0), max(h, 0.0))


class Raster:
    """Rasterize one widget package to a PIL RGBA image."""

    def __init__(self, game, caches, warnings):
        from PIL import Image, ImageDraw, ImageFont  # bundled with Blender 5.1
        self.PImage, self.PDraw, self.PFont = Image, ImageDraw, ImageFont
        self.game = game
        self.caches = caches
        self.warnings = warnings
        self._tex = {}
        self._fonts = {}
        self._font_bytes = {k: self._load_font_bytes(v)
                            for k, v in _FONT_FACES.items()}
        self._in_scroll = 0

    # -- assets ------------------------------------------------------------
    def _load_font_bytes(self, path):
        try:
            rd = self.game.provider.get_reader(path)
            if rd is None:
                raise RuntimeError("no reader")
            data = rd.read() if hasattr(rd, "read") else rd.base_stream.read()
            if data:
                return bytes(data)
        except Exception as e:  # noqa: BLE001
            self.warnings.append(f"screen font extract failed ({path}): "
                                 f"{type(e).__name__} {e}")
        return None

    def font(self, px, face="terminal"):
        px = max(6, int(round(px)))
        blob = self._font_bytes.get(face) or self._font_bytes.get("terminal")
        f = self._fonts.get((face, px))
        if f is None:
            try:
                if blob:
                    f = self.PFont.truetype(io.BytesIO(blob), px)
                else:
                    f = self.PFont.load_default()
            except Exception:  # noqa: BLE001
                f = self.PFont.load_default()
            self._fonts[(face, px)] = f
        return f

    def texture(self, pkg):
        """Texture2D package -> PIL RGBA (cached), following one material hop."""
        if pkg in self._tex:
            return self._tex[pkg]
        img = None
        arr = self._tex_array(pkg)
        if arr is None:
            # a MATERIAL brush: use its first referenced texture
            for e in self.game.package_dict(pkg) or []:
                t = str(e.get("Type") or "")
                if t not in ("Material", "MaterialInstanceConstant"):
                    continue
                p = e.get("Properties") or {}
                refs = []
                for tv in p.get("TextureParameterValues") or []:
                    refs.append(_ref_pkg((tv or {}).get("ParameterValue")))
                ced = p.get("CachedExpressionData") or {}
                for r in ced.get("ReferencedTextures") or []:
                    refs.append(_ref_pkg(r))
                par = _ref_pkg(p.get("Parent"))
                for r in refs:
                    if r and "rgbPixel" not in r:
                        arr = self._tex_array(r)
                        if arr is not None:
                            break
                if arr is None and par:
                    return self.texture(par)
                break
        if arr is not None:
            img = self.PImage.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8), "RGBA")
        self._tex[pkg] = img
        return img

    def _tex_array(self, pkg):
        try:
            cube = False
            ex = self.game.find_export(pkg, "Texture2D")
            data = getattr(ex, "data", None) if ex is not None else None
            if not data:
                ex = self.game.find_export(pkg, "TextureCube")
                data = getattr(ex, "data", None) if ex is not None else None
                cube = bool(data)
            if not data:
                return None
            pd = data[0]
            mip = None
            for m in getattr(pd, "Mips", []) or []:
                if getattr(getattr(m, "BulkData", None), "Data", None):
                    mip = m
                    break
            if mip is None:
                return None
            raw = bytes(mip.BulkData.Data)
            if cube and len(raw) % 6 == 0:
                raw = raw[:len(raw) // 6]  # face 0 of the 6-face cube payload
            return bc_decode.decode_pixels(pd.PixelFormat.name, raw,
                                           int(mip.SizeX), int(mip.SizeY))
        except Exception:  # noqa: BLE001
            return None

    # -- painting ----------------------------------------------------------
    def render(self):
        """The one shared atlas raster (all six screen meshes window it by UV)."""
        tree = _Tree(self.game, _ATLAS_PKG, self.warnings)
        if tree.root is None or tree.widget(tree.root) is None:
            self.warnings.append("screen widget has no tree: " + _ATLAS_PKG)
            return None
        cw = ch = 2000.0  # the atlas canvas (measured: root child 2000x2000)
        self.scale = min(1.0, _MAX_CANVAS / max(cw, ch))
        W, H = int(round(cw * self.scale)), int(round(ch * self.scale))
        img = self.PImage.new("RGBA", (W, H), (0, 0, 0, 255))
        self._paint(img, tree, tree.widget(tree.root), (0.0, 0.0, float(cw), float(ch)), 0)
        return img

    def _desired_h(self, tree, w):
        """Row height for box stacking (UMG 'Auto' size, approximated)."""
        t = str(w.get("Type") or "")
        p = w.get("Properties") or {}
        if t == "TextBlock":
            f = p.get("Font") or {}
            return float(f.get("Size", 24.0)) * _SLATE_PX * 1.15
        if t == "Image":
            isz = (p.get("Brush") or {}).get("ImageSize") or {}
            return float(isz.get("Y", 24.0))
        if t in ("CanvasPanel", "SizeBox", "Border", "Overlay"):
            hh = 0.0
            for se, cw_ in tree.children(w):
                _x, y, _w, h = _slot_rect(se, (0.0, 0.0, 0.0, 0.0))
                hh = max(hh, y + h)
            # stretch-anchored children measure ~0 against a zero parent; a
            # sub-8px "row" is that artifact, not a real height
            return hh if hh > 8 else 24.0
        return 24.0

    def _paint(self, img, tree, w, rect, depth):
        if w is None or depth > 12:
            return
        p = w.get("Properties") or {}
        if str(p.get("Visibility") or "") in _HIDDEN:
            return
        rt = p.get("RenderTransform")
        if isinstance(rt, dict) and rt:
            # UMG RenderTransform (scale -> shear -> rotate -> translate around
            # the pivot): the atlas leans on it hard -- the comp table bakes in
            # ROTATED -90 and the mesh cutout reads the rotated projection.
            layer = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
            self._paint_inner(layer, tree, w, p, rect, depth)
            layer = self._apply_render_transform(layer, p, rt, rect)
            img.alpha_composite(layer)
        else:
            self._paint_inner(img, tree, w, p, rect, depth)

    def _apply_render_transform(self, layer, p, rt, rect):
        import math
        x, y, wd, ht = rect
        s = self.scale
        piv = rt.get("Pivot") or p.get("RenderTransformPivot") or {}
        pvx = (x + float(piv.get("X", 0.5)) * wd) * s
        pvy = (y + float(piv.get("Y", 0.5)) * ht) * s
        tr = rt.get("Translation") or {}
        tx, ty = float(tr.get("X", 0.0)) * s, float(tr.get("Y", 0.0)) * s
        sc = rt.get("Scale") or {}
        sx, sy = float(sc.get("X", 1.0)), float(sc.get("Y", 1.0))
        sh = rt.get("Shear") or {}
        shx = math.tan(math.radians(float(sh.get("X", 0.0))))
        shy = math.tan(math.radians(float(sh.get("Y", 0.0))))
        th = math.radians(float(rt.get("Angle", 0.0)))
        c, sn = math.cos(th), math.sin(th)
        # forward M = R @ Sh @ Sc (scale, then shear, then rotate; Y-down
        # screen space, positive Angle = clockwise -- validated against the
        # comp table's -90 cutout). Shear leans the glyph TOPS rightward.
        m00, m01 = sx, -shx * sy
        m10, m11 = shy * sx, sy
        r00, r01 = c * m00 - sn * m10, c * m01 - sn * m11
        r10, r11 = sn * m00 + c * m10, sn * m01 + c * m11
        det = r00 * r11 - r01 * r10
        if abs(det) < 1e-9:
            return layer
        i00, i01 = r11 / det, -r01 / det
        i10, i11 = -r10 / det, r00 / det
        ox, oy = pvx + tx, pvy + ty
        coeffs = (i00, i01, pvx - i00 * ox - i01 * oy,
                  i10, i11, pvy - i10 * ox - i11 * oy)
        return layer.transform(layer.size, self.PImage.AFFINE, coeffs,
                               resample=self.PImage.BILINEAR)

    def _paint_inner(self, img, tree, w, p, rect, depth):
        nm = str(w.get("Name") or "")
        t = str(w.get("Type") or "")
        if t == "Image":
            if not nm.startswith(_RUNTIME_IMG_WIDGETS):
                self._paint_image(img, p, rect)
        elif t == "TextBlock":
            self._paint_text(img, p, rect, nm)
        elif t == "RichTextBlock":
            # rich logs render their idle line (color comes from a style
            # DataTable; the desk logs read terminal green in the reference
            # shots) -- design-time filler is blanked via _TEXT_OVERRIDES
            self._paint_text(img, p, rect, nm, default_col=(64, 255, 64, 255))
        elif t in ("Spacer", "ProgressBar", "ScrollBoxSlot"):
            pass  # empty at idle (runtime bars / spacing)
        elif t in ("VerticalBox", "HorizontalBox"):
            kids = tree.children(w)
            if not kids:
                return
            x, y, wd, ht = rect
            if t == "VerticalBox":
                if self._in_scroll or ht <= 1:
                    # scroll content is unbounded (Fill has nothing to split):
                    # natural row heights, clipped at the box bottom
                    cy = y
                    for _se, cw_ in kids:
                        h = self._desired_h(tree, cw_)
                        if ht > 1 and cy + h > y + ht + 1:
                            break
                        self._paint(img, tree, cw_, (x, cy, wd, h), depth + 1)
                        cy += h
                else:
                    # bounded box: this cook's slots are Fill shares (measured
                    # by the coords counters' per-digit cutouts: rows at
                    # 1519/1603/1688 = 250/3 spacing, and the comp table shows
                    # ALL rows in its cutout, which natural heights would clip)
                    shares = [float((((se.get("Properties") or {}).get("Size"))
                                     or {}).get("Value", 1.0)) for se, _c in kids]
                    total = sum(shares) or 1.0
                    cy = y
                    for (se, cw_), shr in zip(kids, shares):
                        h = ht * shr / total
                        self._paint(img, tree, cw_, (x, cy, wd, h), depth + 1)
                        cy += h
            else:
                step = wd / len(kids)
                for i, (_se, cw_) in enumerate(kids):
                    self._paint(img, tree, cw_, (x + i * step, y, step, ht), depth + 1)
        elif t in ("ScrollBox", "RetainerBox"):
            # Both CLIP to their own box (a retainer renders its subtree into
            # its own RT -- the coords space view pans an 11840x5920 sky
            # surface inside one; unclipped it floods the whole canvas).
            # Layout rects can spill past the quadrant that owns the content
            # (ui_coordinates builds oversized), while the unit faces' cutouts
            # never show a neighbor quadrant's pixels (reference shots), so
            # the clip is also held inside the quadrant of the box center.
            x, y, wd, ht = self._px(rect)
            if wd <= 2 or ht <= 2:
                return
            tmp = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
            self._in_scroll += (t == "ScrollBox")
            for _se, cw_ in tree.children(w):
                self._paint(tmp, tree, cw_, rect, depth + 1)
            self._in_scroll -= (t == "ScrollBox")
            clipped = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
            box = (max(0, x), max(0, y),
                   min(img.size[0], x + wd), min(img.size[1], y + ht))
            half = int(round(1000.0 * self.scale))
            qx = 0 if (x + wd / 2.0) < half else half
            qy = 0 if (y + ht / 2.0) < half else half
            box = (max(box[0], qx), max(box[1], qy),
                   min(box[2], qx + half), min(box[3], qy + half))
            if box[2] > box[0] and box[3] > box[1]:
                clipped.paste(tmp.crop(box), (box[0], box[1]))
                img.alpha_composite(clipped)
        elif t in ("SizeBox", "Border", "Overlay", "ScaleBox", "WidgetSwitcher"):
            for _se, cw_ in tree.children(w):
                self._paint(img, tree, cw_, rect, depth + 1)
        elif t == "CanvasPanel":
            for se, cw_ in tree.children(w):
                self._paint(img, tree, cw_, _slot_rect(se, rect), depth + 1)
        elif t.endswith("_C"):
            # embedded user widget (ui_coordinates_C, ui_atlasDishesStatus_C,
            # ui_radar_C, ...): rasterize ITS package tree into this rect.
            # An embed's design spills past its quadrant (ui_coordinates
            # builds oversized) while the unit cutouts never show a neighbor
            # quadrant's pixels, so the whole embed is clipped to the
            # quadrant of its slot center.
            sub_pkg = self.game.class_package(t)
            if sub_pkg:
                sub = _Tree(self.game, sub_pkg, self.warnings)
                if sub.root and sub.widget(sub.root) is not None:
                    lay = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
                    self._paint(lay, sub, sub.widget(sub.root), rect, depth + 1)
                    x, y, wd, ht = self._px(rect)
                    half = int(round(1000.0 * self.scale))
                    qx = 0 if (x + wd / 2.0) < half else half
                    qy = 0 if (y + ht / 2.0) < half else half
                    keep = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
                    keep.paste(lay.crop((qx, qy, qx + half, qy + half)), (qx, qy))
                    img.alpha_composite(keep)

    def _px(self, rect):
        s = self.scale
        x, y, w, h = rect
        return (int(round(x * s)), int(round(y * s)),
                max(1, int(round(w * s))), max(1, int(round(h * s))))

    def _paint_image(self, img, p, rect):
        brush = p.get("Brush") or {}
        tint = _srgb8(_color(brush.get("TintColor"),
                             _color(p.get("ColorAndOpacity"))))
        if tint[3] == 0:
            return
        x, y, w, h = self._px(rect)
        if w <= 1 or h <= 1:
            isz = brush.get("ImageSize") or {}
            w = max(w, int(round(float(isz.get("X", 32.0)) * self.scale)))
            h = max(h, int(round(float(isz.get("Y", 32.0)) * self.scale)))
        ro = brush.get("ResourceObject")
        src_pkg = _ref_pkg(ro)
        leaf = (str(_ref(ro) or "")).lower()
        layer = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
        d = self.PDraw.Draw(layer)
        if leaf.startswith(_NOISE_MATS):
            # the analog grain underlay: a faint deterministic noise field in
            # the widget's tint (the coords space view tints it (1,0.5,0))
            rng = np.random.default_rng(1973)
            g = rng.integers(0, 256, (max(1, h // 2), max(1, w // 2)),
                             dtype=np.uint8)
            fld = np.stack([g * (tint[0] / 255.0), g * (tint[1] / 255.0),
                            g * (tint[2] / 255.0),
                            np.full_like(g, int(0.35 * tint[3]))], axis=-1)
            noise = self.PImage.fromarray(fld.astype(np.uint8), "RGBA")
            noise = noise.resize((w, h), self.PImage.NEAREST)
            layer.paste(noise, (x, y))
            img.alpha_composite(layer)
            return
        if leaf.startswith(_RUNTIME_MATS):
            return  # runtime-painted effect surface: black at idle
        if leaf.startswith(_SPACE_MATS):
            # the Milky Way panorama stretched over the whole pannable sky
            # surface (the retainer clip shows a window of it), lifted toward
            # the reference shots' exposure
            tex = self.texture(_SPACE_TEX)
            if tex is not None:
                tt = np.asarray(tex, dtype=np.float32)
                tt[..., :3] = np.clip(tt[..., :3] * _SPACE_GAIN, 0, 255)
                lifted = self.PImage.fromarray(tt.astype(np.uint8), "RGBA")
                piece = lifted.resize((max(1, w), max(1, h)), self.PImage.BILINEAR)
                layer.paste(piece, (x, y))  # paste allows a negative dest
                img.alpha_composite(layer)
            return
        if leaf.startswith(_BORDER_MATS) or leaf.startswith("inst_multiborder"):
            # the game's border shaders draw a fixed orange (baked in the node
            # graph); the widget tint modulates it. inst_multiborder_1xN adds
            # N-1 row separators inside the box.
            col = (_BORDER_RGB[0] * tint[0] // 255, _BORDER_RGB[1] * tint[1] // 255,
                   _BORDER_RGB[2] * tint[2] // 255, tint[3])
            bw = max(1, int(round(3.0 * self.scale)))
            d.rectangle([x, y, x + w - 1, y + h - 1], outline=col, width=bw)
            m = re.search(r"multiborder_(\d+)x(\d+)", leaf)
            if m:
                nx, ny = max(1, int(m.group(1))), max(1, int(m.group(2)))
                lw = max(1, int(round(2.0 * self.scale)))
                for i in range(1, ny):
                    gy = y + h * i // ny
                    d.rectangle([x, gy - lw // 2, x + w - 1, gy + (lw - 1) // 2], fill=col)
                for i in range(1, nx):
                    gx = x + w * i // nx
                    d.rectangle([gx - lw // 2, y, gx + (lw - 1) // 2, y + h - 1], fill=col)
            img.alpha_composite(layer)
            return
        if leaf.startswith(_LINE_MATS):
            # the game's shader draws a thin axis line inside the slot box
            lw = max(1, int(round(2.0 * self.scale)))
            if h >= w:  # vertical
                cx = x + w // 2
                d.rectangle([cx - lw // 2, y, cx + (lw - 1) // 2, y + h - 1], fill=tint)
            else:
                cyl = y + h // 2
                d.rectangle([x, cyl - lw // 2, x + w - 1, cyl + (lw - 1) // 2], fill=tint)
            img.alpha_composite(layer)
            return
        if leaf.startswith(_GRID_MATS):
            cell = max(4, int(round(50.0 * self.scale)))
            for gx in range(x, x + w, cell):
                d.line([gx, y, gx, y + h - 1], fill=tint, width=1)
            for gy in range(y, y + h, cell):
                d.line([x, gy, x + w - 1, gy], fill=tint, width=1)
            img.alpha_composite(layer)
            return
        tex = self.texture(src_pkg) if src_pkg else None
        if tex is None:
            return  # no cooked art (runtime content): leave black
        margin = brush.get("Margin") or {}
        drawas = str(brush.get("DrawAs") or "")
        if drawas.endswith("Box") and any(margin.get(k) for k in ("Left", "Top", "Right", "Bottom")):
            piece = self._nine_slice(tex, (w, h), margin)
        else:
            piece = tex.resize((w, h), self.PImage.BILINEAR)
        if tint != (255, 255, 255, 255):
            tt = np.asarray(piece, dtype=np.float32)
            tt *= np.array(tint, dtype=np.float32) / 255.0
            piece = self.PImage.fromarray(tt.astype(np.uint8), "RGBA")
        layer = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
        layer.paste(piece, (x, y), piece)  # paste allows a negative dest
        img.alpha_composite(layer)

    def _nine_slice(self, tex, wh, margin):
        tw, th = tex.size
        w, h = wh
        ml = int(float(margin.get("Left", 0.0)) * tw)
        mt = int(float(margin.get("Top", 0.0)) * th)
        mr = int(float(margin.get("Right", 0.0)) * tw)
        mb = int(float(margin.get("Bottom", 0.0)) * th)
        dl = min(ml, w // 2)
        dt = min(mt, h // 2)
        dr = min(mr, w - dl)
        db = min(mb, h - dt)
        out = self.PImage.new("RGBA", (w, h), (0, 0, 0, 0))

        def put(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1):
            if sx1 <= sx0 or sy1 <= sy0 or dx1 <= dx0 or dy1 <= dy0:
                return
            piece = tex.crop((sx0, sy0, sx1, sy1)).resize((dx1 - dx0, dy1 - dy0),
                                                          self.PImage.NEAREST)
            out.alpha_composite(piece, (dx0, dy0))

        xs = [0, ml, tw - mr, tw]
        ys = [0, mt, th - mb, th]
        dx = [0, dl, w - dr, w]
        dy = [0, dt, h - db, h]
        for i in range(3):
            for j in range(3):
                put(xs[i], ys[j], xs[i + 1], ys[j + 1],
                    dx[i], dy[j], dx[i + 1], dy[j + 1])
        return out

    def _paint_text(self, img, p, rect, name="", default_col=None):
        t_ = p.get("Text")
        # localized prose cooks as a dict (SourceString); culture-invariant
        # numbers cook as a PLAIN STRING
        if isinstance(t_, dict):
            txt = t_.get("SourceString") or t_.get("CultureInvariantString")
        else:
            txt = t_ if isinstance(t_, str) else None
        if name in _TEXT_OVERRIDES:
            txt = _TEXT_OVERRIDES[name]
        elif txt:
            txt = _init_text(name, str(txt))
        if not txt:
            return
        f = p.get("Font") or {}
        size = float(f.get("Size", 24.0)) * _SLATE_PX * self.scale
        face = "terminal"
        if "analogue" in str(_ref(f.get("FontObject")) or "").lower():
            face = "analogue"  # the 7-segment taximeter face
        font = self.font(size, face)
        if default_col is not None and not p.get("ColorAndOpacity"):
            col = default_col
        else:
            col = _srgb8(_color(p.get("ColorAndOpacity")))
        x, y, w, h = self._px(rect)
        layer = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
        d = self.PDraw.Draw(layer)
        just = str(p.get("Justification") or "")
        try:
            asc, dsc = font.getmetrics()
        except Exception:  # noqa: BLE001
            asc, dsc = int(size), 0
        lines = str(txt).replace("\r\n", "\n").split("\n")
        line_h = asc + dsc
        ty = y
        if h > line_h * len(lines) * 1.35:
            # a Fill row much taller than the text: the glyphs sit centered
            # (measured by the coords counters' per-digit cutouts)
            ty = y + (h - line_h * len(lines)) / 2.0
        # Slate LetterSpacing is 1/1000 em of the rendered size
        spacing = float(f.get("LetterSpacing", 0.0)) / 1000.0 * size
        for ln in lines:
            if ln:
                try:
                    tl = d.textlength(ln, font=font)
                except Exception:  # noqa: BLE001
                    tl = len(ln) * size * 0.55
                tl += spacing * max(0, len(ln) - 1)
                tx = x
                if just.endswith("Center"):
                    tx = x + (w - tl) / 2.0
                elif just.endswith("Right"):
                    tx = x + w - tl
                if spacing > 0.5:
                    cx2 = tx
                    for ch in ln:
                        d.text((cx2, ty), ch, font=font, fill=col)
                        try:
                            cx2 += d.textlength(ch, font=font) + spacing
                        except Exception:  # noqa: BLE001
                            cx2 += size * 0.55 + spacing
                else:
                    d.text((tx, ty), ln, font=font, fill=col)
            ty += line_h
        img.alpha_composite(layer)


# ---- Blender side ----------------------------------------------------------

def _content_image(game, caches, warnings):
    key = ("rtscreen_img", "atlas")
    if key in caches["img"]:
        return caches["img"][key]
    import bpy
    bimg = None
    try:
        r = Raster(game, caches, warnings)
        pil = r.render()
        if pil is not None:
            arr = np.asarray(pil, dtype=np.float32) / 255.0
            # sRGB bytes -> linear floats for a color image buffer
            rgb = arr[..., :3]
            arr[..., :3] = np.where(rgb <= 0.04045, rgb / 12.92,
                                    ((rgb + 0.055) / 1.055) ** 2.4)
            bimg = bpy.data.images.new("votv_rt_atlas", width=pil.width,
                                       height=pil.height, alpha=True)
            bimg.pixels.foreach_set(np.flipud(arr).reshape(-1).astype(np.float32))
            bimg.pack()
    except Exception as e:  # noqa: BLE001
        warnings.append(f"powered screen raster failed: {type(e).__name__} {e}")
    caches["img"][key] = bimg
    return bimg


def get_powered_material(game, caches, warnings, image_fn):
    """The powered-on CRT material shared by all six screen meshes:
    emission = atlas raster (raw UV) x brightness x the game's RGB pixel mask."""
    key = ("rtscreen", "atlas")
    if key in caches["mat"]:
        return caches["mat"][key]
    import bpy
    mat = bpy.data.materials.new("votv_rt_screen")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    caches["mat"][key] = mat
    if bsdf is None:
        return mat
    bsdf.inputs["Base Color"].default_value = (0.010, 0.014, 0.011, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.18
    content = _content_image(game, caches, warnings)
    if content is None:
        return mat  # dark face fallback
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = content
    tex.location = (-700, 300)
    src = tex.outputs["Color"]
    # the CRT pixel mask, tiled at the material's own res_X x res_Y defaults
    mask = image_fn(game, _PIXEL_TEX, caches, warnings, non_color=False)
    if mask is not None:
        coord = nt.nodes.new("ShaderNodeTexCoord")
        coord.location = (-1120, 60)
        mapping = nt.nodes.new("ShaderNodeMapping")
        mapping.location = (-940, 60)
        mapping.inputs["Scale"].default_value = (_RES_X, _RES_Y, 1.0)
        nt.links.new(coord.outputs["UV"], mapping.inputs["Vector"])
        mtex = nt.nodes.new("ShaderNodeTexImage")
        mtex.image = mask
        mtex.extension = "REPEAT"
        mtex.location = (-700, 40)
        nt.links.new(mapping.outputs["Vector"], mtex.inputs["Vector"])
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        mix.blend_type = "MULTIPLY"
        mix.inputs["Factor"].default_value = 1.0
        mix.location = (-420, 200)
        nt.links.new(src, mix.inputs["A"])
        nt.links.new(mtex.outputs["Color"], mix.inputs["B"])
        src = mix.outputs["Result"]
        gain = 2.6  # the triad mask averages ~1/2.6; keep perceived brightness
    else:
        gain = 1.0
    nt.links.new(src, bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = 2.0 * _BRIGHTNESS * gain
    return mat
