"""Numpy decoders for every pixel format VotV ships (full-population census, zero BC7):
DXT1(BC1) / DXT5(BC3) / BC4 / BC5 / B8G8R8A8 / G8 / G16 / FloatRGBA -> Blender image."""
import numpy as np

import bpy


def _expand565(c):
    r = ((c >> 11) & 31).astype(np.float32) * (255.0 / 31.0)
    g = ((c >> 5) & 63).astype(np.float32) * (255.0 / 63.0)
    b = (c & 31).astype(np.float32) * (255.0 / 31.0)
    return r, g, b


def _bc1_blocks(raw, four_color_only=False):
    """(N*8 bytes) -> (N, 16, 4) float32 RGBA 0..255."""
    n = len(raw) // 8
    b = np.frombuffer(raw, dtype=np.uint8)[:n * 8].reshape(n, 8)
    c0 = b[:, 0].astype(np.uint16) | (b[:, 1].astype(np.uint16) << 8)
    c1 = b[:, 2].astype(np.uint16) | (b[:, 3].astype(np.uint16) << 8)
    bits = (b[:, 4].astype(np.uint32) | (b[:, 5].astype(np.uint32) << 8)
            | (b[:, 6].astype(np.uint32) << 16) | (b[:, 7].astype(np.uint32) << 24))
    r0, g0, b0 = _expand565(c0)
    r1, g1, b1 = _expand565(c1)
    pal = np.zeros((n, 4, 4), dtype=np.float32)
    pal[:, 0] = np.stack([r0, g0, b0, np.full(n, 255.0, np.float32)], axis=1)
    pal[:, 1] = np.stack([r1, g1, b1, np.full(n, 255.0, np.float32)], axis=1)
    four = (c0 > c1) | four_color_only
    # 4-color mode
    pal[four, 2, :3] = (2.0 * pal[four, 0, :3] + pal[four, 1, :3]) / 3.0
    pal[four, 3, :3] = (pal[four, 0, :3] + 2.0 * pal[four, 1, :3]) / 3.0
    pal[four, 2, 3] = 255.0
    pal[four, 3, 3] = 255.0
    # 3-color + transparent mode
    tri = ~four
    pal[tri, 2, :3] = (pal[tri, 0, :3] + pal[tri, 1, :3]) * 0.5
    pal[tri, 2, 3] = 255.0
    pal[tri, 3] = 0.0
    sel = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))[None, :]) & 3
    return np.take_along_axis(pal, sel[:, :, None].astype(np.intp), axis=1)


def _bc4_blocks(raw):
    """(N*8 bytes) -> (N, 16) float32 0..255 single channel."""
    n = len(raw) // 8
    b = np.frombuffer(raw, dtype=np.uint8)[:n * 8].reshape(n, 8)
    a0 = b[:, 0].astype(np.float32)
    a1 = b[:, 1].astype(np.float32)
    bits = np.zeros(n, dtype=np.uint64)
    for i in range(6):
        bits |= b[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    pal = np.zeros((n, 8), dtype=np.float32)
    pal[:, 0] = a0
    pal[:, 1] = a1
    big = a0 > a1
    for k in range(2, 8):
        pal[big, k] = ((8 - k) * a0[big] + (k - 1) * a1[big]) / 7.0
    small = ~big
    for k in range(2, 6):
        pal[small, k] = ((6 - k) * a0[small] + (k - 1) * a1[small]) / 5.0
    pal[small, 6] = 0.0
    pal[small, 7] = 255.0
    sel = (bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(7)
    return np.take_along_axis(pal, sel.astype(np.intp), axis=1)


def _tile(blocks, w, h, channels=4):
    """(N, 16, C) block texels -> (h, w, C) image."""
    bx = (w + 3) // 4
    by = (h + 3) // 4
    a = blocks.reshape(by, bx, 4, 4, channels)
    a = a.transpose(0, 2, 1, 3, 4).reshape(by * 4, bx * 4, channels)
    return a[:h, :w]


def decode_pixels(fmt, raw, w, h):
    """-> (h, w, 4) float32 RGBA in 0..1, top-down; None if unknown format."""
    if fmt == "PF_DXT1":
        img = _tile(_bc1_blocks(raw), w, h) / 255.0
    elif fmt == "PF_DXT5":
        n = len(raw) // 16
        b = np.frombuffer(raw, dtype=np.uint8)[:n * 16].reshape(n, 16)
        alpha = _bc4_blocks(b[:, :8].tobytes())
        color = _bc1_blocks(b[:, 8:].tobytes(), four_color_only=True)
        color[:, :, 3] = alpha
        img = _tile(color, w, h) / 255.0
    elif fmt == "PF_BC4":
        g = _tile(_bc4_blocks(raw)[:, :, None], w, h, 1)[:, :, 0] / 255.0
        img = np.dstack([g, g, g, np.ones_like(g)])
    elif fmt == "PF_BC5":
        n = len(raw) // 16
        b = np.frombuffer(raw, dtype=np.uint8)[:n * 16].reshape(n, 16)
        r = _tile(_bc4_blocks(b[:, :8].tobytes())[:, :, None], w, h, 1)[:, :, 0] / 255.0
        g = _tile(_bc4_blocks(b[:, 8:].tobytes())[:, :, None], w, h, 1)[:, :, 0] / 255.0
        img = np.dstack([r, g, np.ones_like(r), np.ones_like(r)])
    elif fmt == "PF_B8G8R8A8":
        a = np.frombuffer(raw, dtype=np.uint8)[:w * h * 4].reshape(h, w, 4)
        img = a[:, :, [2, 1, 0, 3]].astype(np.float32) / 255.0
    elif fmt == "PF_G8":
        g = np.frombuffer(raw, dtype=np.uint8)[:w * h].reshape(h, w).astype(np.float32) / 255.0
        img = np.dstack([g, g, g, np.ones_like(g)])
    elif fmt == "PF_G16":
        g = np.frombuffer(raw, dtype="<u2")[:w * h].reshape(h, w).astype(np.float32) / 65535.0
        img = np.dstack([g, g, g, np.ones_like(g)])
    elif fmt == "PF_FloatRGBA":
        img = np.frombuffer(raw, dtype="<f2")[:w * h * 4].reshape(h, w, 4).astype(np.float32)
    else:
        return None
    return img


def texture_to_image(game, tex_pkg_path, warnings):
    """Decode a cooked Texture2D package into a packed Blender image (or None)."""
    ex = game.find_export(tex_pkg_path, "Texture2D")
    data = getattr(ex, "data", None) if ex is not None else None
    if not data:
        return None
    pd = data[0]
    mips = getattr(pd, "Mips", []) or []
    mip = None
    for m in mips:
        if getattr(getattr(m, "BulkData", None), "Data", None):
            mip = m
            break
    if mip is None:
        warnings.append(f"texture has no mip data: {tex_pkg_path}")
        return None
    w, h = int(mip.SizeX), int(mip.SizeY)
    fmt = pd.PixelFormat.name
    img = decode_pixels(fmt, bytes(mip.BulkData.Data), w, h)
    if img is None:
        warnings.append(f"unknown pixel format {fmt}: {tex_pkg_path} - placeholder used")
        return None
    name = tex_pkg_path.rsplit("/", 1)[-1]
    bimg = bpy.data.images.new(name, width=w, height=h, alpha=True)
    bimg.pixels.foreach_set(np.flipud(img).reshape(-1).astype(np.float32))
    bimg.pack()
    return bimg
