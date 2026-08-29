"""SceneManifest: turn the parsed .sav property tree into typed rows the scene builder consumes.

Field names in VotV's BP structs carry GUID suffixes (class_3_5267A5ED... etc.);
_field() strips them. Schema measured from the real save + CXXHeaderDump
(struct_save.hpp / struct_primitiveSave.hpp / saveSlot.hpp).
"""
import re

_FIELD_RE = re.compile(r"^(.*?)_\d+_[0-9A-Fa-f]{32}$")

# class-path substrings -> scene category
_NPC_HINTS = ("kerfurOmega.", "murderKerfur", "npc_", "kerfur.")
_VEHICLE_HINTS = ("/ATV.", "explodVehicle")
_PILE_HINTS = ("trashBitsPile", "garbageClump", "ChipPile")


def _field(struct_dict, base):
    """Fetch a GUID-suffixed field by its base name."""
    if not isinstance(struct_dict, dict):
        return None
    for k, v in struct_dict.items():
        m = _FIELD_RE.match(k)
        if (m.group(1) if m else k) == base:
            return v
    return None


def _flatten_slot_array(val):
    """TArray<struct_mX{ TArray<T> }> -> list of per-slot lists."""
    out = []
    for slot in (val or []):
        if isinstance(slot, dict) and slot:
            inner = next(iter(slot.values()))
            out.append(inner if isinstance(inner, list) else [])
        else:
            out.append([])
    return out


def _transform(val):
    """Nested tagged Transform -> (quat_xyzw, translation, scale) in UE space."""
    if not isinstance(val, dict):
        return ((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0), (1.0, 1.0, 1.0))
    rot = val.get("Rotation", (0.0, 0.0, 0.0, 1.0))
    tra = val.get("Translation", (0.0, 0.0, 0.0))
    scl = val.get("Scale3D", (1.0, 1.0, 1.0))
    return (tuple(rot), tuple(tra), tuple(scl))


class SaveRow:
    __slots__ = ("class_path", "transform", "key", "names", "ints", "floats",
                 "bools", "strings", "category", "source", "json")

    def __init__(self, class_path, transform, key, source):
        self.class_path = class_path or ""
        self.transform = transform
        self.key = key or "None"
        self.names = []
        self.ints = []
        self.floats = []
        self.bools = []
        self.strings = []
        self.json = ""
        self.source = source  # 'objects' | 'primitives' | 'contained' | 'equipment' | 'inventory'
        self.category = self._classify()

    def _classify(self):
        cp = self.class_path
        if self.source == "primitives" or any(h in cp for h in _PILE_HINTS):
            return "Piles"
        if any(h in cp for h in _NPC_HINTS):
            return "NPC"
        if any(h in cp for h in _VEHICLE_HINTS):
            return "Vehicles"
        return "Props"

    @property
    def class_name(self):
        # '/Game/objects/prop.prop_C' -> 'prop_C'
        return self.class_path.rsplit(".", 1)[-1] if self.class_path else ""

    @property
    def package_path(self):
        # '/Game/objects/prop.prop_C' -> '/Game/objects/prop'
        return self.class_path.rsplit(".", 1)[0] if "." in self.class_path else self.class_path

    @property
    def prop_name(self):
        """names[0][0] -- the list_props discriminator for generic prop_C rows."""
        if self.names and self.names[0]:
            n = self.names[0][0]
            return "" if n in ("", "None") else n
        return ""


def _row_from_struct_save(s, source):
    row = SaveRow(
        _field(s, "class"),
        _transform(_field(s, "transform")),
        _field(s, "key"),
        source,
    )
    row.names = _flatten_slot_array(_field(s, "names"))
    row.ints = _flatten_slot_array(_field(s, "ints"))
    row.floats = _flatten_slot_array(_field(s, "floats"))
    row.bools = _flatten_slot_array(_field(s, "bools"))
    row.strings = _flatten_slot_array(_field(s, "strings"))
    return row


def _row_from_primitive(s):
    row = SaveRow(_field(s, "class"), _transform(_field(s, "transform")),
                  _field(s, "key"), "primitives")
    row.json = _field(s, "json") or ""
    return row


class SceneManifest:
    def __init__(self):
        self.level = ""
        self.header = {}
        self.scalars = {}
        self.objects = []        # SaveRow (world props/entities/NPC/vehicles)
        self.primitives = []     # SaveRow (piles/grime)
        self.gobj_stack = []     # slot -> [SaveRow] (container contents)
        self.equipment = []      # SaveRow (held/hotbar items)
        self.inventory = []      # SaveRow
        self.player_transform = None
        self.car_transform = None
        self.roaches = []        # [(quat, loc, scale)]
        self.non_scene_keys = []

    def rows_by_category(self):
        cats = {}
        for row in self.objects:
            cats.setdefault(row.category, []).append(row)
        return cats


_SCALAR_KEEP = (
    "Points", "Version", "Day", "moonPhase", "totalTime", "food", "sleep",
    "battery", "health", "maxHealth", "Level", "dailyDelivery",
)


def build_manifest(header, props):
    m = SceneManifest()
    m.header = header
    m.level = props.get("Level", "") or ""
    for k in _SCALAR_KEEP:
        if k in props:
            m.scalars[k] = props[k]
    if "savedtime" in props:
        m.scalars["savedtime"] = props["savedtime"]

    for s in props.get("objectsData", []) or []:
        m.objects.append(_row_from_struct_save(s, "objects"))
    for s in props.get("primitivesData", []) or []:
        m.primitives.append(_row_from_primitive(s))
    for layer in props.get("GObjStack", []) or []:
        inner = _field(layer, "obj") or []
        m.gobj_stack.append([_row_from_struct_save(s, "contained") for s in inner])
    for s in props.get("inventoryData", []) or []:
        m.inventory.append(_row_from_struct_save(s, "inventory"))
    for e in props.get("equipment", []) or []:
        data = _field(e, "data")
        if isinstance(data, dict):
            row = _row_from_struct_save(data, "equipment")
            if row.class_path and row.class_path != "None":
                m.equipment.append(row)

    if "playerTransform" in props:
        m.player_transform = _transform(props["playerTransform"])
    if "carTransform" in props:
        m.car_transform = _transform(props["carTransform"])
    for t in props.get("roache", []) or []:
        m.roaches.append(_transform(t))

    scene_keys = {"objectsData", "primitivesData", "GObjStack", "inventoryData",
                  "equipment", "playerTransform", "carTransform", "roache"}
    m.non_scene_keys = sorted(k for k in props if k not in scene_keys)

    def container_slot(row):
        """container row -> its GObjStack slot index (ints[0][0]), or -1."""
        if row.ints and row.ints[0]:
            v = row.ints[0][0]
            if isinstance(v, int) and 0 <= v < len(m.gobj_stack):
                return v
        return -1
    m.container_slot = container_slot
    return m
