"""ClassTemplateResolver: what an actor of class X looks like by default.

Cooked BP packages carry the SCS component templates (…_GEN_VARIABLE) AND the SCS
node graph (SCS_Node exports: ComponentTemplate + ChildNodes + ParentComponentOrVariableName),
so a class's default shape is a TREE, not a flat list — dish_C's head hangs off
axis_Z (yaw) → axis_Y (pitch); flattening it buries the head in the base (the
2026-08-29 dish bug). One resolver serves save-row spawning and umap placement.

Resolution ladder for a save row (each rung measured):
1. generic prop.prop_C: names[0][0] -> list_props row -> mesh;
2. the SCS template tree (composed transforms + random working pose per pose_random);
3. the class CDO 'name' -> list_props (prop_C sets its own mesh that way at construction);
4. curated supplement for runtime-built visuals.
"""
from mathutils import Matrix

from . import convert, pose_random

_GEN = "_GEN_VARIABLE"

# Classes whose visuals are runtime-built (UCS/particles) -- curated meshes.
BUILTIN_SUPPLEMENT = {
    "dirthole_item_C": "/Game/meshes/dirthole2/dirthole2_dirthole_M",
}


class TemplateComp:
    __slots__ = ("name", "kind", "mesh", "rel_loc", "rel_rot", "rel_scale",
                 "hidden", "has_override_mats")

    def __init__(self, name, kind, mesh):
        self.name = name
        self.kind = kind          # 'SM' | 'SK' | 'SCENE'
        self.mesh = mesh
        self.rel_loc = (0.0, 0.0, 0.0)
        self.rel_rot = (0.0, 0.0, 0.0)
        self.rel_scale = (1.0, 1.0, 1.0)
        self.hidden = False
        self.has_override_mats = False


def _obj_ref_package(v):
    if not isinstance(v, dict):
        return ""
    outer = v.get("OuterIndex") or {}
    pkg = outer.get("ObjectName") or v.get("Outer") or ""
    return str(pkg) if pkg else ""


def _ref_name(v):
    return str(v.get("ObjectName", "")) if isinstance(v, dict) else ""


def _vec3(v, default):
    if isinstance(v, dict):
        return (float(v.get("X", 0.0)), float(v.get("Y", 0.0)), float(v.get("Z", 0.0)))
    return default


def _rot3(v):
    if isinstance(v, dict):
        return (float(v.get("Pitch", 0.0)), float(v.get("Yaw", 0.0)), float(v.get("Roll", 0.0)))
    return (0.0, 0.0, 0.0)


def _strip_gen(name):
    return name[:-len(_GEN)] if name.endswith(_GEN) else name


class TemplateResolver:
    def __init__(self, game):
        self.game = game
        self._info = {}   # normalized package path -> class info dict

    # ------------------------------------------------------------------ load
    def _load(self, package_path, _depth=0):
        key = self.game.norm(package_path)
        if key in self._info:
            return self._info[key]
        templates = {}
        children = {}
        node_tmpl = {}      # SCS node name -> template base
        node_kids = {}      # SCS node name -> [child node names]
        child_nodes = set()
        parent_pkg = None
        cdo_name = ""
        ifaces = set()
        if _depth < 8:
            for e in self.game.package_dict(key):
                if not isinstance(e, dict):
                    continue
                ty = e.get("Type")
                nm = str(e.get("Name", ""))
                props = e.get("Properties") or {}
                if nm.startswith("Default__"):
                    v = props.get("name")
                    if isinstance(v, str) and v not in ("", "None"):
                        cdo_name = v
                if ty in ("StaticMeshComponent", "SkeletalMeshComponent", "SceneComponent"):
                    base = _strip_gen(nm)
                    if base.startswith("ICH-") or base in templates:
                        continue
                    mesh = _obj_ref_package(props.get("StaticMesh") or props.get("SkeletalMesh"))
                    kind = {"StaticMeshComponent": "SM", "SkeletalMeshComponent": "SK",
                            "SceneComponent": "SCENE"}[ty]
                    t = TemplateComp(base, kind, mesh)
                    t.rel_loc = _vec3(props.get("RelativeLocation"), t.rel_loc)
                    t.rel_rot = _rot3(props.get("RelativeRotation"))
                    t.rel_scale = _vec3(props.get("RelativeScale3D"), t.rel_scale)
                    t.hidden = bool(props.get("bHiddenInGame", False)) or \
                        (props.get("bVisible") is False)
                    t.has_override_mats = bool(props.get("OverrideMaterials"))
                    templates[base] = t
                elif ty == "SCS_Node":
                    tmpl = _strip_gen(_ref_name(props.get("ComponentTemplate")))
                    kids = [_ref_name(k) for k in props.get("ChildNodes") or []]
                    node_tmpl[nm] = tmpl
                    node_kids[nm] = kids
                    child_nodes.update(kids)
                elif ty == "BlueprintGeneratedClass":
                    sup = e.get("SuperStruct") or {}
                    pkg = _obj_ref_package(sup)
                    if pkg.startswith("/Game/"):
                        parent_pkg = pkg
                    for itf in e.get("Interfaces") or []:
                        inm = _ref_name(itf)
                        if inm:
                            ifaces.add(inm)
        # SCS node graph -> template-base tree
        roots = []
        for node, tmpl in node_tmpl.items():
            kid_bases = [node_tmpl.get(k, "") for k in node_kids.get(node, [])]
            children[tmpl] = [b for b in kid_bases if b]
            if node not in child_nodes:
                roots.append(tmpl)
        info = {"templates": templates, "children": children, "roots": roots,
                "cdo_name": cdo_name, "ifaces": ifaces}
        if parent_pkg:
            par = self._load(parent_pkg, _depth + 1)
            for base, t in par["templates"].items():
                info["templates"].setdefault(base, t)
            for base, kids in par["children"].items():
                info["children"].setdefault(base, kids)
            have = set(info["templates"])
            info["roots"] = list(dict.fromkeys(
                info["roots"] + [r for r in par["roots"] if r in have]))
            if not info["cdo_name"]:
                info["cdo_name"] = par["cdo_name"]
            info["ifaces"] |= par["ifaces"]
        self._info[key] = info
        return info

    # ------------------------------------------------------------- queries
    def cdo_name(self, package_path):
        return self._load(package_path)["cdo_name"]

    def implements(self, package_path, interface_class_name):
        return interface_class_name in self._load(package_path)["ifaces"]

    def templates(self, package_path):
        """Flat base->TemplateComp view (umap per-component fallbacks)."""
        return self._load(package_path)["templates"]

    def tree_info(self, package_path):
        """{'templates', 'children', 'roots', ...} for tree-first umap assembly."""
        return self._load(package_path)

    # ------------------------------------------------------------- spawning
    def spawn_plan(self, row, list_props_table, pose_seed):
        """SaveRow -> [(mesh package path, local matrix rel. to the actor, kind)]."""
        if row.class_name == "prop_C":
            mesh = list_props_table.get(row.prop_name, "")
            return [(mesh, Matrix.Identity(4), "SM")] if mesh else []

        info = self._load(row.package_path)
        out = []

        def walk(base, parent_m, depth=0):
            if depth > 10:
                return
            t = info["templates"].get(base)
            # a pivot SceneComponent with no overridden defaults has NO template
            # export at all (dish axis_Z/axis_Y) -- identity rel, keep walking
            if t is None:
                m = parent_m
            else:
                m = parent_m @ convert.matrix_from_rotator(t.rel_rot, t.rel_loc, t.rel_scale)
            pose = pose_random.pose_rotation(row.class_name, base, pose_seed)
            if pose is not None:
                m = m @ pose
            if t is not None and not t.hidden and t.mesh:
                if t.mesh.startswith("/Game/"):
                    out.append((t.mesh, m, t.kind))
                elif t.mesh.startswith("/Engine/") and t.has_override_mats:
                    out.append((t.mesh, m, t.kind))
            for kid in info["children"].get(base, ()):
                walk(kid, m, depth + 1)

        for root in info["roots"]:
            walk(root, Matrix.Identity(4))

        if not any(kind == "SM" for _, _, kind in out):
            name = info["cdo_name"]
            mesh = list_props_table.get(name, "") if name else ""
            if not mesh:
                mesh = BUILTIN_SUPPLEMENT.get(row.class_name, "")
            if mesh:
                out.append((mesh, Matrix.Identity(4), "SM"))
        if not out and any(t.kind == "SK" for t in info["templates"].values()):
            # native-parent skeletal body (kerfur CharacterMesh0) - not in the SCS tree
            out.append(("", Matrix.Identity(4), "SK"))
        return out
