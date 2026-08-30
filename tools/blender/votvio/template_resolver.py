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

from . import convert, decals as decals_mod, pose_random

_GEN = "_GEN_VARIABLE"

_INST_TYPES = {
    "InstancedStaticMeshComponent": "ISM",
    "HierarchicalInstancedStaticMeshComponent": "ISM",
    "FoliageInstancedStaticMeshComponent": "ISM",
}

# Classes whose visuals are runtime-built (UCS/particles) -- curated meshes.
BUILTIN_SUPPLEMENT = {
    "dirthole_item_C": "/Game/meshes/dirthole2/dirthole2_dirthole_M",
}


class TemplateComp:
    __slots__ = ("name", "kind", "mesh", "rel_loc", "rel_rot", "rel_scale",
                 "hidden", "has_override_mats", "f_hidden", "f_visible",
                 "f_render_main", "fields")

    def __init__(self, name, kind, mesh):
        self.name = name
        self.kind = kind          # 'SM' | 'SK' | 'SCENE' | 'ISM'
        self.mesh = mesh
        self.rel_loc = (0.0, 0.0, 0.0)
        self.rel_rot = (0.0, 0.0, 0.0)
        self.rel_scale = (1.0, 1.0, 1.0)
        self.hidden = False
        self.has_override_mats = False
        self.f_hidden = False       # bHiddenInGame
        self.f_visible = True       # bVisible
        self.f_render_main = True   # bRenderInMainPass
        self.fields = set()         # which props this template export carried itself

    def inherit(self, parent):
        """A child BP's template export is a DELTA vs the parent class's template
        (measured: ladder_old_C.segment1 carries no StaticMesh -- the mesh lives on
        ladder_C.segment1). Fill every field this export did not set itself."""
        if "mesh" not in self.fields and parent.mesh:
            self.mesh = parent.mesh
        if "loc" not in self.fields:
            self.rel_loc = parent.rel_loc
        if "rot" not in self.fields:
            self.rel_rot = parent.rel_rot
        if "scale" not in self.fields:
            self.rel_scale = parent.rel_scale
        if "hidden" not in self.fields:
            self.f_hidden = parent.f_hidden
        if "visible" not in self.fields:
            self.f_visible = parent.f_visible
        if "render_main" not in self.fields:
            self.f_render_main = parent.f_render_main
        if "override" not in self.fields:
            self.has_override_mats = parent.has_override_mats
        self.hidden = self.f_hidden or not self.f_visible or not self.f_render_main


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
        self._inst = {}   # (normalized package path, base) -> instance matrices | None

    # ------------------------------------------------------------------ load
    def _load(self, package_path, _depth=0):
        key = self.game.norm(package_path)
        if key in self._info:
            return self._info[key]
        templates = {}
        children = {}
        decal_templates = {}    # base -> {material, size, fields}
        node_tmpl = {}      # SCS node name -> template base
        node_kids = {}      # SCS node name -> [child node names]
        child_nodes = set()
        parent_pkg = None
        cdo_name = ""
        cdo_key = ""
        cdo_material = ""   # the grime BP's runtime decal material variable
        cdo_max_process = 0.0   # grime maxProcess (display alpha denominator)
        ifaces = set()
        ancestors = set()   # class names up the SuperStruct chain (self included)
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
                    v = props.get("key")
                    if isinstance(v, str) and v not in ("", "None"):
                        cdo_key = v
                    v = props.get("material")
                    if isinstance(v, dict):
                        mp = _obj_ref_package(v)
                        if mp.startswith("/Game/"):
                            cdo_material = mp
                    v = props.get("maxProcess")
                    if isinstance(v, (int, float)) and v > 0:
                        cdo_max_process = float(v)
                if ty == "DecalComponent":
                    base = _strip_gen(nm)
                    if base not in decal_templates:
                        d = {"material": _obj_ref_package(props.get("DecalMaterial")),
                             "size": _vec3(props.get("DecalSize"), (128.0, 256.0, 256.0)),
                             "fields": set()}
                        if "DecalMaterial" in props:
                            d["fields"].add("material")
                        if "DecalSize" in props:
                            d["fields"].add("size")
                        decal_templates[base] = d
                if ty.endswith("Component"):
                    # every SceneComponent-derived template carries the rel transform
                    # the umap deltas omit (the locker door's hinge is an Arrow)
                    base = _strip_gen(nm)
                    if base.startswith("ICH-") or base in templates:
                        continue
                    mesh = _obj_ref_package(props.get("StaticMesh") or props.get("SkeletalMesh"))
                    kind = {"StaticMeshComponent": "SM", "SkeletalMeshComponent": "SK",
                            "SceneComponent": "SCENE"}.get(ty) or _INST_TYPES.get(ty, "OTHER")
                    t = TemplateComp(base, kind, mesh)
                    if "StaticMesh" in props or "SkeletalMesh" in props:
                        t.fields.add("mesh")
                    if "RelativeLocation" in props:
                        t.fields.add("loc")
                    if "RelativeRotation" in props:
                        t.fields.add("rot")
                    if "RelativeScale3D" in props:
                        t.fields.add("scale")
                    if "bHiddenInGame" in props:
                        t.fields.add("hidden")
                    if "bVisible" in props:
                        t.fields.add("visible")
                    if "bRenderInMainPass" in props:
                        t.fields.add("render_main")
                    if "OverrideMaterials" in props:
                        t.fields.add("override")
                    t.rel_loc = _vec3(props.get("RelativeLocation"), t.rel_loc)
                    t.rel_rot = _rot3(props.get("RelativeRotation"))
                    t.rel_scale = _vec3(props.get("RelativeScale3D"), t.rel_scale)
                    t.f_hidden = props.get("bHiddenInGame") is True
                    t.f_visible = props.get("bVisible") is not False
                    t.f_render_main = props.get("bRenderInMainPass") is not False
                    t.hidden = t.f_hidden or not t.f_visible or not t.f_render_main
                    t.has_override_mats = bool(props.get("OverrideMaterials"))
                    templates[base] = t
                elif ty == "SCS_Node":
                    tmpl = _strip_gen(_ref_name(props.get("ComponentTemplate")))
                    kids = [_ref_name(k) for k in props.get("ChildNodes") or []]
                    node_tmpl[nm] = tmpl
                    node_kids[nm] = kids
                    child_nodes.update(kids)
                elif ty == "BlueprintGeneratedClass":
                    ancestors.add(nm)
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
                "decals": decal_templates, "cdo_material": cdo_material,
                "cdo_max_process": cdo_max_process,
                "cdo_name": cdo_name, "cdo_key": cdo_key, "ifaces": ifaces,
                "ancestors": ancestors}
        if parent_pkg:
            par = self._load(parent_pkg, _depth + 1)
            for base, t in par["templates"].items():
                own = info["templates"].get(base)
                if own is None:
                    info["templates"][base] = t
                else:
                    own.inherit(t)   # child template export is a DELTA vs the parent's
            for base, pd in par["decals"].items():
                own = info["decals"].get(base)
                if own is None:
                    info["decals"][base] = pd
                else:
                    if "material" not in own["fields"] and pd["material"]:
                        own["material"] = pd["material"]
                    if "size" not in own["fields"]:
                        own["size"] = pd["size"]
            for base, kids in par["children"].items():
                info["children"].setdefault(base, kids)
            have = set(info["templates"])
            info["roots"] = list(dict.fromkeys(
                info["roots"] + [r for r in par["roots"] if r in have]))
            if not info["cdo_name"]:
                info["cdo_name"] = par["cdo_name"]
            if not info["cdo_key"]:
                info["cdo_key"] = par["cdo_key"]
            if not info["cdo_material"]:
                info["cdo_material"] = par["cdo_material"]
            if not info["cdo_max_process"]:
                info["cdo_max_process"] = par["cdo_max_process"]
            info["ifaces"] |= par["ifaces"]
            info["ancestors"] |= par["ancestors"]
        self._info[key] = info
        return info

    # ------------------------------------------------------------- queries
    def cdo_name(self, package_path):
        return self._load(package_path)["cdo_name"]

    def cdo_key(self, package_path):
        """Class-default int_save key (radiotower_C keys itself 'radiotower' in the
        CDO; most keyed actors carry a per-instance delta key instead)."""
        return self._load(package_path)["cdo_key"]

    def implements(self, package_path, interface_class_name):
        return interface_class_name in self._load(package_path)["ifaces"]

    def is_descendant(self, package_path, class_name):
        """True when the BP class (or an ancestor up the SuperStruct chain)
        is named class_name."""
        return class_name in self._load(package_path)["ancestors"]

    def process_alpha(self, package_path, row_json):
        """Grime display opacity = clamp(saved process / class maxProcess).
        Measured (probe_v9b): poo persists 50 against its OWN maxProcess=50
        (full), oil 300 vs inherited 100 (full, just 3x the mopping), a
        half-mopped stain saves process<max and renders faded."""
        _v, process = decals_mod.row_variant_process(row_json)
        denom = self._load(package_path)["cdo_max_process"] or 100.0
        return min(max(process / denom, 0.0), 1.0)

    def templates(self, package_path):
        """Flat base->TemplateComp view (umap per-component fallbacks)."""
        return self._load(package_path)["templates"]

    def tree_info(self, package_path):
        """{'templates', 'children', 'roots', ...} (save-row spawn planning)."""
        return self._load(package_path)

    def template_instances(self, package_path, base, _depth=0):
        """Baked per-instance matrices of a class-template ISM component (an umap
        delta with no native tail inherits these). Walks the SuperStruct chain."""
        key = (self.game.norm(package_path), base)
        if key in self._inst:
            return self._inst[key]
        result = None
        dicts = self.game.package_dict(key[0])
        pkg = self.game.load_package(key[0])
        em = pkg.ExportMap if pkg is not None else []
        paired = len(em) == len(dicts)
        parent_pkg = None
        for i, e in enumerate(dicts):
            if not isinstance(e, dict):
                continue
            if e.get("Type") == "BlueprintGeneratedClass":
                p = _obj_ref_package(e.get("SuperStruct") or {})
                if p.startswith("/Game/"):
                    parent_pkg = p
                continue
            if e.get("Type") not in _INST_TYPES:
                continue
            if _strip_gen(str(e.get("Name", ""))) != base:
                continue
            if paired:
                inst = getattr(em[i].exportObject, "instance_matrices", None)
                if inst is not None and len(inst) > 0:
                    result = inst
            break
        if result is None and parent_pkg and _depth < 8:
            result = self.template_instances(parent_pkg, base, _depth + 1)
        self._inst[key] = result
        return result

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
                ok = t.mesh.startswith("/Game/") or \
                    (t.mesh.startswith("/Engine/") and t.has_override_mats)
                if ok and t.kind == "ISM":
                    ti = self.template_instances(row.package_path, base)
                    for inst in (ti if ti is not None else ()):
                        out.append((t.mesh, m @ convert.ue_fmatrix_to_bl(inst), "SM"))
                elif ok:
                    out.append((t.mesh, m, t.kind))
            dt = info["decals"].get(base)
            if dt is not None:
                # the game's runtime pick: the SAVED variant when the row
                # carries one (primitives json = [variant, process]), then the
                # per-type variant family, then the CDO 'material' variable,
                # then the template's own material. json[1] is mop DURABILITY
                # (probe_v9b) and feeds process_alpha() - never the size.
                variant, _process = decals_mod.row_variant_process(
                    getattr(row, "json", ""))
                if variant < 0:
                    variant = decals_mod.poster_index(row)
                dmat = decals_mod.grime_material(row.class_name, pose_seed,
                                                 variant)
                if not dmat:
                    dmat = info["cdo_material"] or str(dt["material"])
                if dmat.startswith("/Game/"):
                    m2 = m
                    spin = decals_mod.grime_spin(row.class_name, pose_seed)
                    if spin is not None:
                        m2 = m @ spin
                    out.append((dmat, m2 @ decals_mod.size_matrix(dt["size"]),
                                "DECAL"))
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
