"""The static world: walk the persistent umap's exports and place its mesh components.

Reconcile rule (interim v1, mirrors the measured loadObjects semantics without the
kismet gatherer table yet): actors of classes implementing int_save are SKIPPED here --
the game Phase-A destroys them on load and the save rows re-express them (gatherer
fixtures included: their rows carry live transforms). int_primitive classes likewise
(primitivesData is load-decisive). Everything else is placed as cooked.
"""
import bpy
from mathutils import Matrix

from . import convert, pose_random

MESH_COMPS = {
    "StaticMeshComponent",
    "InstancedStaticMeshComponent",
    "HierarchicalInstancedStaticMeshComponent",
    "FoliageInstancedStaticMeshComponent",
}
INSTANCED = {
    "InstancedStaticMeshComponent",
    "HierarchicalInstancedStaticMeshComponent",
    "FoliageInstancedStaticMeshComponent",
}
LIGHT_COMPS = {"PointLightComponent": "POINT", "SpotLightComponent": "SPOT",
               "RectLightComponent": "AREA"}


def _ref(v):
    """object-ref dict -> (object name, outer name)."""
    if not isinstance(v, dict):
        return ("", "")
    return (str(v.get("ObjectName", "")),
            str((v.get("OuterIndex") or {}).get("ObjectName", "")))


def _ref_pkg(v):
    if not isinstance(v, dict):
        return ""
    outer = v.get("OuterIndex") or {}
    return str(outer.get("ObjectName") or v.get("Outer") or "")


def _vec(v, d=(0.0, 0.0, 0.0)):
    if isinstance(v, dict):
        return (float(v.get("X", 0.0)), float(v.get("Y", 0.0)), float(v.get("Z", 0.0)))
    return d


def _rot(v):
    if isinstance(v, dict):
        return (float(v.get("Pitch", 0.0)), float(v.get("Yaw", 0.0)), float(v.get("Roll", 0.0)))
    return (0.0, 0.0, 0.0)


class MapImporter:
    def __init__(self, game, resolver, builder, options, save_classes=None):
        self.game = game
        self.resolver = resolver
        self.b = builder                    # the assemble._Builder (mesh/material caches)
        self.opt = options
        self.save_classes = save_classes or set()
        self.stats = {"placed": 0, "instances": 0, "skipped_saveclass": 0,
                      "no_mesh": 0, "hidden": 0, "lights": 0, "sk_skipped": 0,
                      "landscape": 0}
        self._world_m = {}
        self._actor = {}
        self._save_class = {}

    # ---- graph ----------------------------------------------------------
    def _actor_of(self, idx):
        if idx in self._actor:
            return self._actor[idx]
        e = self.dicts[idx]
        outer = e.get("Outer")
        cur = self.by_actor_name.get(outer)
        seen = 0
        while cur is not None and seen < 8:
            ce = self.dicts[cur]
            if ce.get("Outer") == "PersistentLevel":
                self._actor[idx] = cur
                return cur
            cur = self.by_actor_name.get(ce.get("Outer"))
            seen += 1
        self._actor[idx] = None
        return None

    def _class_is_save(self, actor_type):
        """Skip a level actor iff the SAVE re-expresses its class: rows of this class
        exist (loadObjects destroys the level copies and respawns from rows), or the
        class is int_primitive (primitivesData is load-decisive). int_save classes
        with NO rows visibly persist in-game (gather/skipPreDelete) -> keep them."""
        if actor_type in self._save_class:
            return self._save_class[actor_type]
        result = actor_type in self.save_classes
        if not result and actor_type.endswith("_C"):
            pkg = self.game.class_package(actor_type)
            if pkg:
                result = self.resolver.implements(pkg, "int_primitive_C")
        self._save_class[actor_type] = result
        return result

    def _template_for(self, idx):
        actor_idx = self._actor_of(idx)
        if actor_idx is None:
            return None
        at = self.dicts[actor_idx].get("Type", "")
        if not at.endswith("_C"):
            return None
        pkg = self.game.class_package(at)
        if not pkg:
            return None
        return self.resolver.templates(pkg).get(self.dicts[idx].get("Name", ""))

    def _rel_matrix(self, idx):
        p = self.dicts[idx].get("Properties") or {}
        if any(k in p for k in ("RelativeLocation", "RelativeRotation", "RelativeScale3D")):
            m = convert.matrix_from_rotator(
                _rot(p.get("RelativeRotation")),
                _vec(p.get("RelativeLocation")),
                _vec(p.get("RelativeScale3D"), (1.0, 1.0, 1.0)))
        else:
            t = self._template_for(idx)
            if t is not None:
                m = convert.matrix_from_rotator(t.rel_rot, t.rel_loc, t.rel_scale)
            else:
                m = Matrix.Identity(4)
        actor_idx = self._actor_of(idx)
        if actor_idx is not None:
            atype = self.dicts[actor_idx].get("Type", "")
            if pose_random.has_pose(atype):
                pose = pose_random.pose_rotation(
                    atype, self.dicts[idx].get("Name", ""),
                    self.dicts[actor_idx].get("Name", ""))
                if pose is not None:
                    m = m @ pose
        return m

    def _world_matrix(self, idx, depth=0):
        if idx in self._world_m:
            return self._world_m[idx]
        m = self._rel_matrix(idx)
        if depth < 12:
            p = self.dicts[idx].get("Properties") or {}
            pname, pouter = _ref(p.get("AttachParent"))
            if pname:
                pidx = self.by_comp.get((pname, pouter))
                if pidx is None:
                    pidx = self.by_comp.get((pname, self.dicts[idx].get("Outer")))
                if pidx is not None and pidx != idx:
                    m = self._world_matrix(pidx, depth + 1) @ m
        self._world_m[idx] = m
        return m

    # ---- placement ------------------------------------------------------
    def _mesh_path_for(self, idx, ty):
        p = self.dicts[idx].get("Properties") or {}
        sm = p.get("StaticMesh")
        pkg = _ref_pkg(sm)
        if pkg.startswith("/Game/") or pkg.startswith("/Engine/"):
            return pkg
        t = self._template_for(idx)
        if t is not None and t.kind == "SM" and t.mesh.startswith("/Game/"):
            return t.mesh
        return ""

    def run(self, map_path, cols, progress):
        self.dicts = self.game.package_dict(map_path)
        pkg = self.game.load_package(map_path)
        em = pkg.ExportMap if pkg is not None else []
        paired = len(em) == len(self.dicts)

        self.by_actor_name = {}
        self.by_comp = {}
        for i, e in enumerate(self.dicts):
            if not isinstance(e, dict):
                continue
            nm, outer = e.get("Name"), e.get("Outer")
            self.by_actor_name.setdefault(nm, i)
            self.by_comp[(nm, outer)] = i

        density = max(0.0, min(1.0, self.opt.get("foliage_density", 1.0)))
        total = len(self.dicts)
        for i, e in enumerate(self.dicts):
            if i % 2000 == 0:
                progress(i, total, "map")
            if not isinstance(e, dict):
                continue
            ty = e.get("Type", "")
            if ty in LIGHT_COMPS:
                if self.opt.get("import_lights", True):
                    self._place_light(i, ty, cols["Lights"])
                continue
            if ty == "SkeletalMeshComponent":
                self.stats["sk_skipped"] += 1
                continue
            if ty not in MESH_COMPS:
                continue
            p = e.get("Properties") or {}
            if p.get("bHiddenInGame") is True or p.get("bVisible") is False:
                self.stats["hidden"] += 1
                continue
            actor_idx = self._actor_of(i)
            atype = self.dicts[actor_idx].get("Type", "") if actor_idx is not None else ""
            if atype and self._class_is_save(atype):
                self.stats["skipped_saveclass"] += 1
                continue
            mesh_path = self._mesh_path_for(i, ty)
            if not mesh_path.startswith("/Game/"):
                self.stats["no_mesh"] += 1
                continue
            me = self.b.ensure_mesh(mesh_path)
            if me is None:
                self.stats["no_mesh"] += 1
                continue
            world = self._world_matrix(i)
            label = (self.dicts[actor_idx].get("Name") if actor_idx is not None
                     else e.get("Name")) or "map"
            if ty in INSTANCED:
                inst = None
                if paired:
                    inst = getattr(em[i].exportObject, "instance_matrices", None)
                if inst is None or len(inst) == 0:
                    self.stats["no_mesh"] += 1
                    continue
                n = len(inst)
                take = range(n) if density >= 1.0 else range(0, n, max(1, int(1.0 / density)))
                target = cols["Foliage"] if ty == "FoliageInstancedStaticMeshComponent" \
                    else cols["Statics"]
                for k in take:
                    self.b._new_object(label, me, target,
                                       world @ convert.ue_fmatrix_to_bl(inst[k]))
                    self.stats["instances"] += 1
            else:
                self.b._new_object(label, me, cols["Statics"], world)
                self.stats["placed"] += 1
        return self.stats

    def _place_light(self, idx, ty, col):
        p = self.dicts[idx].get("Properties") or {}
        ld = bpy.data.lights.new(self.dicts[idx].get("Name", "light"), LIGHT_COMPS[ty])
        intensity = float(p.get("Intensity", 5000.0) or 5000.0)
        ld.energy = max(1.0, intensity * 0.01)
        c = p.get("LightColor")
        if isinstance(c, dict):
            ld.color = (c.get("R", 255) / 255.0, c.get("G", 255) / 255.0,
                        c.get("B", 255) / 255.0)
        ob = bpy.data.objects.new(ld.name, ld)
        ob.matrix_world = self._world_matrix(idx)
        col.objects.link(ob)
        self.stats["lights"] += 1
