#!/usr/bin/env python3
"""atlas -- pack the mdl_extract per-texture PNGs into ONE atlas + UV-rect map.

Rung 4 of the texture ladder (docs/COOP_CLIENT_MODEL.md 7). A GoldSrc model
paints per-mesh textures (19 on the scientist); the cooked pipeline binds ONE
texture (the 'tex' param on the slot-0 MID). So: shelf-pack every extracted PNG
into a single atlas and emit the name->pixel-rect map that ue_cook uses to remap
each face corner's UV into its tile.

Bleed guard: every tile gets a 1px duplicated-edge gutter (clamp-extend). The
cooked texture has NO mips (ue_tex cooks 1 inline mip), so 1px is sufficient for
bilinear sampling at tile edges (UVs land exactly on tile boundaries: measured
u,v extents hit 0.0/1.0).

Usage:
  python tools/client_model/atlas.py <texDir> <outBase>
    writes <outBase>.png + <outBase>.json
    {"canvas":[W,H], "tiles":{name:[x,y,w,h]}}   name = usemtl = filename - .png
    x,y = tile top-left in IMAGE rows (y-down). ue_cook maps cooked v downward
    (v_cooked = 1 - v_obj) to match: UE samples V=0 at mip row 0 = PNG row 0.
Dev/RE tool (RULE 3).
"""
import json
import os
import sys

from PIL import Image

GUTTER = 1  # duplicated-edge px on each side of every tile


def shelf_pack(sizes, canvas_w):
    """sizes: [(name,w,h)] -> {name:(x,y)} tile top-lefts (gutter-inset) or None."""
    cell = sorted(sizes, key=lambda t: -t[2])  # height desc
    pos, x, y, row_h = {}, 0, 0, 0
    for name, w, h in cell:
        cw, ch = w + 2 * GUTTER, h + 2 * GUTTER
        if cw > canvas_w:
            return None, 0
        if x + cw > canvas_w:
            x, y, row_h = 0, y + row_h, 0
        pos[name] = (x + GUTTER, y + GUTTER)
        x += cw
        row_h = max(row_h, ch)
    return pos, y + row_h


def pack(tex_dir, out_base):
    tiles = []
    for fn in sorted(os.listdir(tex_dir)):
        if not fn.lower().endswith(".png"):
            continue
        im = Image.open(os.path.join(tex_dir, fn)).convert("RGBA")
        tiles.append((fn[:-4], im))  # name = usemtl (mdl_extract wrote <safe>.png)
    assert tiles, f"no PNGs in {tex_dir}"

    sizes = [(n, im.width, im.height) for n, im in tiles]
    for cw in (128, 256, 512, 1024):
        pos, used_h = shelf_pack(sizes, cw)
        if pos is not None and used_h <= cw:
            W = cw
            H = next(p for p in (128, 256, 512, 1024) if p >= used_h)
            break
    else:
        raise SystemExit("tiles do not fit 1024x1024 -- add a real packer")

    atlas = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    rects = {}
    for name, im in tiles:
        x, y = pos[name]
        w, h = im.width, im.height
        atlas.paste(im, (x, y))
        # clamp-extend 1px: L/R columns, then full-width top/bottom rows (covers corners)
        atlas.paste(im.crop((0, 0, 1, h)), (x - 1, y))
        atlas.paste(im.crop((w - 1, 0, w, h)), (x + w, y))
        row = atlas.crop((x - 1, y, x + w + 1, y + 1))
        atlas.paste(row, (x - 1, y - 1))
        row = atlas.crop((x - 1, y + h - 1, x + w + 1, y + h))
        atlas.paste(row, (x - 1, y + h))
        rects[name] = [x, y, w, h]

    # no-overlap self-check on the gutter-expanded cells
    cells = [(r[0] - GUTTER, r[1] - GUTTER, r[0] + r[2] + GUTTER, r[1] + r[3] + GUTTER)
             for r in rects.values()]
    for i, a in enumerate(cells):
        assert a[2] <= W and a[3] <= H, f"cell out of canvas: {a}"
        for b in cells[i + 1:]:
            assert a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1], \
                f"overlap {a} vs {b}"

    atlas.save(out_base + ".png")
    json.dump({"canvas": [W, H], "tiles": rects}, open(out_base + ".json", "w"), indent=1)
    area = sum(w * h for _, w, h in sizes)
    print(f"[atlas] {len(tiles)} tiles -> {W}x{H} ({100.0 * area / (W * H):.0f}% filled), "
          f"gutter={GUTTER}px  -> {out_base}.png/.json")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    pack(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
