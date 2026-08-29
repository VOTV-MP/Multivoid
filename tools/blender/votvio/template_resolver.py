"""ClassTemplateResolver: what does an actor of class X look like by default?

Cooked BP packages carry the SCS component templates (name..._GEN_VARIABLE) with the
StaticMesh/SkeletalMesh refs and relative transforms; placed instances delta-serialize
against them (measured: 57% of umap SMComponents carry no StaticMesh property).
ONE resolver, two consumers: save-row spawning (P1) and umap placement (P2).

P1 scope: per-class template list from the class package + SuperStruct chain
(leaf-most definition wins). UCS-created components are invisible to this walk --
the measured residual (193 comps / ~11 classes) rides the curated supplement in P3.
"""

_GEN = "_GEN_VARIABLE"


class TemplateComp:
    __slots__ = ("name", "kind", "mesh", "rel_loc", "rel_rot", "rel_scale", "hidden")

    def __init__(self, name, kind, mesh):
        self.name = name
        self.kind = kind          # 'SM' | 'SK'
        self.mesh = mesh          # '/Game/...' package path, or ''
        self.rel_loc = (0.0, 0.0, 0.0)
        self.rel_rot = (0.0, 0.0, 0.0)      # Rotator: pitch, yaw, roll (degrees)
        self.rel_scale = (1.0, 1.0, 1.0)
        self.hidden = False


def _obj_ref_package(v):
    """Import-style object ref dict -> its package path string, or ''."""
    if not isinstance(v, dict):
        return ""
    outer = v.get("OuterIndex") or {}
    pkg = outer.get("ObjectName") or v.get("Outer") or ""
    return str(pkg) if pkg else ""


def _vec3(v, default):
    if isinstance(v, dict):
        return (float(v.get("X", 0.0)), float(v.get("Y", 0.0)), float(v.get("Z", 0.0)))
    return default


def _rot3(v):
    if isinstance(v, dict):
        return (float(v.get("Pitch", 0.0)), float(v.get("Yaw", 0.0)), float(v.get("Roll", 0.0)))
    return (0.0, 0.0, 0.0)


# Classes whose visuals are runtime-built (UCS/particles) -- measured; curated meshes.
# dirthole_item: no SMComponent at all (mound sizes S/M/L exist as meshes).
BUILTIN_SUPPLEMENT = {
    "dirthole_item_C": "/Game/meshes/dirthole2/dirthole2_dirthole_M",
}


class TemplateResolver:
    def __init__(self, game):
        self.game = game
        self._cache = {}   # package path (normalized) -> {base name: TemplateComp}
        self._cdo_name = {}  # package path -> CDO 'name' property ('' when absent)

    def templates(self, package_path, _depth=0):
        """All mesh-bearing component templates for the class in `package_path`,
        including inherited ones (leaf-most definition wins)."""
        key = self.game.norm(package_path)
        if key in self._cache:
            return self._cache[key]
        out = {}
        parent_pkg = None
        cdo_name = ""
        if _depth < 8:
            for e in self.game.package_dict(key):
                if not isinstance(e, dict):
                    continue
                ty = e.get("Type")
                nm = str(e.get("Name", ""))
                if nm.startswith("Default__"):
                    props = e.get("Properties") or {}
                    v = props.get("name")
                    if isinstance(v, str) and v not in ("", "None"):
                        cdo_name = v
                if ty in ("StaticMeshComponent", "SkeletalMeshComponent"):
                    props = e.get("Properties") or {}
                    nm = e.get("Name", "")
                    base = nm[:-len(_GEN)] if nm.endswith(_GEN) else nm
                    mesh = _obj_ref_package(props.get("StaticMesh") or props.get("SkeletalMesh"))
                    if base in out:
                        continue  # leaf class already defined this component
                    t = TemplateComp(base, "SM" if ty == "StaticMeshComponent" else "SK", mesh)
                    t.rel_loc = _vec3(props.get("RelativeLocation"), t.rel_loc)
                    t.rel_rot = _rot3(props.get("RelativeRotation"))
                    t.rel_scale = _vec3(props.get("RelativeScale3D"), t.rel_scale)
                    t.hidden = bool(props.get("bHiddenInGame", False)) or \
                        (props.get("bVisible") is False)
                    out[base] = t
                elif ty == "BlueprintGeneratedClass":
                    sup = e.get("SuperStruct") or {}
                    pkg = _obj_ref_package(sup)
                    if pkg.startswith("/Game/"):
                        parent_pkg = pkg
            if parent_pkg:
                for base, t in self.templates(parent_pkg, _depth + 1).items():
                    out.setdefault(base, t)
                if not cdo_name:
                    cdo_name = self._cdo_name.get(self.game.norm(parent_pkg), "")
        self._cache[key] = out
        self._cdo_name[key] = cdo_name
        return out

    def cdo_name(self, package_path):
        """The class's inherited 'name' property (prop_C's list_props discriminator)."""
        self.templates(package_path)
        return self._cdo_name.get(self.game.norm(package_path), "")

    def visible_mesh_templates(self, package_path):
        return [t for t in self.templates(package_path).values()
                if t.mesh and not t.hidden and t.mesh.startswith("/Game/")]

    def resolve_row(self, row, list_props_table):
        """SaveRow -> list of (mesh package path, TemplateComp-or-None, kind).

        Resolution order (all measured):
        1. generic prop.prop_C rows: names[0][0] -> list_props row -> mesh
           (GetDataTableRowFromName -> row.mesh -> SetStaticMesh in the bytecode);
        2. SCS/inherited component templates that carry a mesh;
        3. the class's CDO 'name' -> list_props (prop_C sets its mesh from its own
           'name' at construction -- potato/coal_s/drive/paper... measured 5/5);
        4. curated supplement for runtime-built visuals (dirthole mounds ...).
        """
        if row.class_name == "prop_C":
            mesh = list_props_table.get(row.prop_name, "")
            return [(mesh, None, "SM")] if mesh else []
        tms = self.visible_mesh_templates(row.package_path)
        if tms:
            return [(t.mesh, t, t.kind) for t in tms]
        name = self.cdo_name(row.package_path)
        mesh = list_props_table.get(name, "") if name else ""
        if mesh:
            return [(mesh, None, "SM")]
        mesh = BUILTIN_SUPPLEMENT.get(row.class_name, "")
        if mesh:
            return [(mesh, None, "SM")]
        return []
