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
ONE 2000x2000 canvas and EVERY screen mesh's "UI" material slot windows its
region of it by raw UV -- the four desk monitors take the four 1000x1000
quadrants (TL=download+detector, TR=playback, BL=coords with the embedded
ui_coordinates, BR=comp), the SAT console face windows exactly the embedded
umg_console rect (U0-.5 V.194-.5, byte-equal to its mesh UVs), the radar face
the embedded umg_radar region. The per-device cover layer (CanvasPanel_76)
and the 14 deathscreen overlays ship Collapsed and stay skipped by their own
cooked Visibility. So: ONE raster, ONE material, six meshes, raw UV.

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
                 "mat_analogds_bar", "mat_uiscreennoise")
# Procedural UI materials the game draws in-shader: reproduce, not sample.
_BORDER_MATS = ("mat_squareborder", "inst_squareborder")
_LINE_MATS = ("inst_coordline", "mat_coordline")
_GRID_MATS = ("mat_coordspacegrid",)
# The coords space view: in game a SceneCaptureComponent2D (FOV 45) films an
# EditorSkySphere painted with mat_space and the widget shows that RT through
# mattT_space. The still frame samples the sphere's own star texture
# (mat_space -> spacehdr2) -- the game's art, a plausible capture window.
_SPACE_MATS = ("mattt_space",)
_SPACE_TEX = "/Game/textures/ui/spacehdr2"
_SPACE_WINDOW = 0.5  # central crop fraction imitating the capture's FOV window
_FONT_FILE = "VotV/Content/main/FSEX300.ufont"
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
}
_FAKE_NUM = re.compile(r"9{3,}|8{4,}|7{4,}")


def _init_text(name, txt):
    if name in _TEXT_OVERRIDES:
        return _TEXT_OVERRIDES[name]
    if _FAKE_NUM.search(txt):
        # zero every number, keeping its printed shape (-999.99 -> 0.00)
        txt = re.sub(r"-?\d+\.\d+", "0.00", txt)
        txt = re.sub(r"-?\d{3,}", "0", txt)
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
        self._font_bytes = self._load_font_bytes()

    # -- assets ------------------------------------------------------------
    def _load_font_bytes(self):
        try:
            rd = self.game.provider.get_reader(_FONT_FILE)
            if rd is None:
                raise RuntimeError("no reader")
            data = rd.read() if hasattr(rd, "read") else rd.base_stream.read()
            if data:
                return bytes(data)
        except Exception as e:  # noqa: BLE001
            self.warnings.append(f"screen font extract failed: {type(e).__name__} {e}")
        return None

    def font(self, px):
        px = max(6, int(round(px)))
        f = self._fonts.get(px)
        if f is None:
            try:
                if self._font_bytes:
                    f = self.PFont.truetype(io.BytesIO(self._font_bytes), px)
                else:
                    f = self.PFont.load_default()
            except Exception:  # noqa: BLE001
                f = self.PFont.load_default()
            self._fonts[px] = f
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
        nm = str(w.get("Name") or "")
        p = w.get("Properties") or {}
        if str(p.get("Visibility") or "") in _HIDDEN:
            return
        t = str(w.get("Type") or "")
        if t == "Image":
            if not nm.startswith(_RUNTIME_IMG_WIDGETS):
                self._paint_image(img, p, rect)
        elif t == "TextBlock":
            self._paint_text(img, p, rect, nm)
        elif t in ("RichTextBlock", "Spacer", "ProgressBar", "ScrollBoxSlot"):
            pass  # empty at idle (logs / lists / runtime bars)
        elif t in ("VerticalBox", "HorizontalBox"):
            kids = tree.children(w)
            if not kids:
                return
            x, y, wd, ht = rect
            if t == "VerticalBox":
                # natural row heights, clipped at the box bottom (a scrollbox
                # shows its top rows; compressing rows overlaps them instead)
                cy = y
                for _se, cw_ in kids:
                    h = self._desired_h(tree, cw_)
                    if ht > 1 and cy + h > y + ht + 1:
                        break
                    self._paint(img, tree, cw_, (x, cy, wd, h), depth + 1)
                    cy += h
            else:
                step = wd / len(kids)
                for i, (_se, cw_) in enumerate(kids):
                    self._paint(img, tree, cw_, (x + i * step, y, step, ht), depth + 1)
        elif t in ("ScrollBox", "RetainerBox"):
            # Both CLIP to their own box (a retainer renders its subtree into
            # its own RT -- the coords space view pans an 11840x5920 sky
            # surface inside one; unclipped it floods the whole canvas)
            x, y, wd, ht = self._px(rect)
            if wd <= 2 or ht <= 2:
                return
            tmp = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
            for _se, cw_ in tree.children(w):
                self._paint(tmp, tree, cw_, rect, depth + 1)
            clipped = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
            box = (max(0, x), max(0, y),
                   min(img.size[0], x + wd), min(img.size[1], y + ht))
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
            # ui_radar_C, ...): rasterize ITS package tree into this rect
            sub_pkg = self.game.class_package(t)
            if sub_pkg:
                sub = _Tree(self.game, sub_pkg, self.warnings)
                if sub.root and sub.widget(sub.root) is not None:
                    self._paint(img, sub, sub.widget(sub.root), rect, depth + 1)

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
        if leaf.startswith(_RUNTIME_MATS):
            return  # runtime-painted effect surface: black at idle
        if leaf.startswith(_SPACE_MATS):
            # the sky sphere's own star texture, tiled at ~native density (the
            # game's capture films the sphere; the pannable surface is 11840
            # design px wide, so one stretched copy would smear the stars)
            tex = self.texture(_SPACE_TEX)
            if tex is not None:
                ts = max(64, int(round(tex.size[0] * self.scale)))
                tile = tex.resize((ts, ts), self.PImage.BILINEAR)
                for ty0 in range(y, y + h, ts):
                    for tx0 in range(x, x + w, ts):
                        layer.paste(tile, (tx0, ty0), tile)  # paste allows negative dest
                img.alpha_composite(layer)
            return
        if leaf.startswith(_BORDER_MATS):
            # the game's shader border box -> a thin outline in the tint
            bw = max(1, int(round(3.0 * self.scale)))
            d.rectangle([x, y, x + w - 1, y + h - 1], outline=tint, width=bw)
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

    def _paint_text(self, img, p, rect, name=""):
        txt = ((p.get("Text") or {}).get("SourceString")
               if isinstance(p.get("Text"), dict) else None)
        if txt:
            txt = _init_text(name, str(txt))
        if not txt:
            return
        f = p.get("Font") or {}
        size = float(f.get("Size", 24.0)) * _SLATE_PX * self.scale
        font = self.font(size)
        col = _srgb8(_color(p.get("ColorAndOpacity")))
        x, y, w, h = self._px(rect)
        layer = self.PImage.new("RGBA", img.size, (0, 0, 0, 0))
        d = self.PDraw.Draw(layer)
        just = str(p.get("Justification") or "")
        try:
            tl = d.textlength(txt, font=font)
        except Exception:  # noqa: BLE001
            tl = len(txt) * size * 0.55
        tx = x
        if just.endswith("Center"):
            tx = x + (w - tl) / 2.0
        elif just.endswith("Right"):
            tx = x + w - tl
        d.text((tx, y), txt, font=font, fill=col)
        rt = p.get("RenderTransform") or {}
        ang = float(rt.get("Angle", 0.0)) if isinstance(rt, dict) else 0.0
        if ang:
            layer = layer.rotate(-ang, center=(x, y), resample=self.PImage.BILINEAR)
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
