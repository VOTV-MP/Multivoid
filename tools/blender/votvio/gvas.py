"""GVAS (.sav) reader for VotV (UE4.27 SaveGame).

Byte-verified against the real save this session-of-record: header layout matches the
repo's C++ walker (ue_wrap/engine/gvas_meta.cpp) and the whole 20 MB s_1234.sav parses
to the None terminator. Delta-vs-CDO semantics: a property absent from the file means
"class default" -- callers treat missing arrays as empty.

Standalone (no bpy): usable from headless tests and build tooling.
"""
import struct

NATIVE_STRUCTS = {
    # struct name -> (unpack format or None, byte size)
    "Vector": ("<fff", 12),
    "Rotator": ("<fff", 12),
    "Quat": ("<ffff", 16),
    "Vector2D": ("<ff", 8),
    "Vector4": ("<ffff", 16),
    "IntPoint": ("<ii", 8),
    "IntVector": ("<iii", 12),
    "Guid": (None, 16),
    "DateTime": ("<q", 8),
    "Timespan": ("<q", 8),
    "LinearColor": ("<ffff", 16),
    "Color": (None, 4),
}


class GvasError(Exception):
    pass


class Reader:
    __slots__ = ("b", "o")

    def __init__(self, buf, offset=0):
        self.b = buf
        self.o = offset

    def u8(self):
        v = self.b[self.o]
        self.o += 1
        return v

    def i32(self):
        v, = struct.unpack_from("<i", self.b, self.o)
        self.o += 4
        return v

    def u32(self):
        v, = struct.unpack_from("<I", self.b, self.o)
        self.o += 4
        return v

    def i64(self):
        v, = struct.unpack_from("<q", self.b, self.o)
        self.o += 8
        return v

    def u16(self):
        v, = struct.unpack_from("<H", self.b, self.o)
        self.o += 2
        return v

    def f32(self):
        v, = struct.unpack_from("<f", self.b, self.o)
        self.o += 4
        return v

    def f64(self):
        v, = struct.unpack_from("<d", self.b, self.o)
        self.o += 8
        return v

    def raw(self, n):
        v = self.b[self.o:self.o + n]
        self.o += n
        return v

    def fstr(self):
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:
            return self.raw(-n * 2).decode("utf-16-le", "replace").rstrip("\0")
        return self.raw(n).decode("utf-8", "replace").rstrip("\0")


def read_header(r):
    if r.raw(4) != b"GVAS":
        raise GvasError("not a GVAS file")
    hdr = {
        "save_game_version": r.i32(),
        "package_version": r.i32(),
        "engine": (r.u16(), r.u16(), r.u16()),
        "changelist": r.u32(),
        "branch": r.fstr(),
    }
    r.i32()  # custom version format (3)
    n = r.i32()
    r.raw(n * 20)  # custom versions: guid + int32 each
    hdr["save_class"] = r.fstr()
    return hdr


def read_tag(r):
    name = r.fstr()
    if name == "None":
        return None
    t = {"name": name, "type": r.fstr()}
    t["size"] = r.i32()
    r.i32()  # ArrayIndex
    ty = t["type"]
    if ty == "StructProperty":
        t["struct"] = r.fstr()
        r.raw(16)  # struct guid
    elif ty in ("ArrayProperty", "SetProperty"):
        t["inner"] = r.fstr()
    elif ty == "MapProperty":
        t["key_type"] = r.fstr()
        t["value_type"] = r.fstr()
    elif ty in ("ByteProperty", "EnumProperty"):
        t["enum"] = r.fstr()
    elif ty == "BoolProperty":
        t["bool"] = r.u8()
    if r.u8():
        r.raw(16)  # property guid
    t["at"] = r.o
    return t


def _read_struct_body(r, struct_name, end):
    native = NATIVE_STRUCTS.get(struct_name)
    if native:
        fmt, size = native
        if fmt:
            vals = struct.unpack_from(fmt, r.b, r.o)
            r.o += size
            return vals if len(vals) > 1 else vals[0]
        r.o += size
        return None
    out = {}
    while r.o < end:
        tag = read_tag(r)
        if tag is None:
            break
        out[tag["name"]] = read_value(r, tag)
    return out


def read_value(r, tag):
    """Read one property's payload; always leaves r.o at the payload end."""
    ty = tag["type"]
    end = tag["at"] + (tag["size"] if ty != "BoolProperty" else 0)
    try:
        if ty == "BoolProperty":
            return bool(tag.get("bool", 0))
        if ty == "IntProperty":
            return r.i32()
        if ty == "UInt32Property":
            return r.u32()
        if ty == "Int64Property":
            return r.i64()
        if ty == "FloatProperty":
            return r.f32()
        if ty == "DoubleProperty":
            return r.f64()
        if ty in ("StrProperty", "NameProperty", "ObjectProperty",
                  "SoftObjectProperty", "EnumProperty"):
            return r.fstr()
        if ty == "ByteProperty":
            if tag.get("enum") and tag["enum"] != "None":
                return r.fstr()
            return r.u8()
        if ty == "StructProperty":
            return _read_struct_body(r, tag.get("struct", ""), end)
        if ty == "ArrayProperty":
            inner = tag.get("inner", "")
            n = r.i32()
            if inner == "StructProperty":
                itag = read_tag(r)  # one inner tag describes all elements
                sname = itag.get("struct", "") if itag else ""
                iend = itag["at"] + itag["size"] if itag else end
                return [_read_struct_body(r, sname, iend) for _ in range(n)]
            if inner == "BoolProperty":
                return [bool(r.u8()) for _ in range(n)]
            if inner == "IntProperty":
                return [r.i32() for _ in range(n)]
            if inner == "FloatProperty":
                return [r.f32() for _ in range(n)]
            if inner in ("StrProperty", "NameProperty", "ObjectProperty"):
                return [r.fstr() for _ in range(n)]
            if inner == "ByteProperty":
                # raw byte blob (thumbnails etc.) -- keep as bytes, cheap
                return bytes(r.raw(max(0, end - r.o)))
            return ("<unread array %s x%d>" % (inner, n))
        # TextProperty / MapProperty / anything else: skip whole payload
        return "<%s>" % ty
    finally:
        r.o = end


def parse_sav(path):
    """Parse a whole .sav -> (header, {top-level property name -> value})."""
    with open(path, "rb") as f:
        buf = f.read()
    r = Reader(buf)
    hdr = read_header(r)
    props = {}
    while True:
        tag = read_tag(r)
        if tag is None:
            break
        props[tag["name"]] = read_value(r, tag)
    hdr["file_size"] = len(buf)
    hdr["trailing_bytes"] = len(buf) - r.o
    return hdr, props
