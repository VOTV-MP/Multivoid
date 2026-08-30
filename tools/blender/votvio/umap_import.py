"""The static world: walk the persistent umap's exports and place its mesh components.

Placement model (measured 2026-08-29, probe_base*): the cooked umap serializes an
export for EVERY component of every level actor -- including ChildActorComponent
children (the `*_CAT_N` actors sit in PersistentLevel at their live transforms) --
and only the PROPERTIES are delta-vs-archetype. So one FLAT pass over the exports,
with per-property template fallbacks (mesh, relative transform, visibility, ISM
instances), reconstructs the world; there is no separate "assemble the class tree"
pass. (v4's tree-first assembly double-applied the root component's transform --
the base building floated 61 m up -- and is retired whole.)

Reconcile rule (interim v1, mirrors the measured loadObjects semantics without the
kismet gatherer table yet): actors of classes implementing int_save are SKIPPED here
when the save has rows of that class -- the game Phase-A destroys them on load and
the save rows re-express them. int_primitive classes likewise (primitivesData is
load-decisive). Everything else is placed as cooked.
"""
import bpy
from mathutils import Matrix

from . import convert, decals, spline_mesh

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

# Runtime render-proxy components the game toggles itself (radiotower's 90m 'rend'
# cube imposter). Named table, grown only with a measured case.
IMPOSTER_COMPONENTS = {"rend"}

# The game's own utility meshes (trigger volumes, blockers, dev cubes). Hidden by
# default; the "Show technical meshes" import option brings them back.
TECHNICAL_MESHES = {
    "/Game/meshes/misc/cube",
    "/Game/meshes/misc/qweqwe",
}

# Level actors that exist only for scripted events (parked in the world, shown
# by their Blueprints at runtime -- cooked data carries no hidden flag). The
# WHOLE trigger family descends from triggerBase_C (measured: spawnProp/agrav/
# tpChamberSpawn/box/... all chain there) and their meshes are designer
# previews: trigger_spawnProp carries a `prop` list_props key + a save `key`,
# and the REAL object arrives as a SAVE row once the event fires (the obelisk
# is an ordinary keyed prop from that moment on). The family is matched by
# CLASS HIERARCHY (resolver.is_descendant); this curated set only lists
# event actors OUTSIDE the family, grown with a measured case each.
EVENT_ACTOR_CLASSES = {
    "arirShip_tower_C",
}


def is_technical(mesh_path, opt):
    if opt.get("show_technical", False):
        return False
    return mesh_path in TECHNICAL_MESHES


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
    def __init__(self, game, resolver, builder, options, save_classes=None,
                 keep_actors=None):
        self.game = game
        self.resolver = resolver
        self.b = builder                    # the assemble._Builder (mesh/material caches)
        self.opt = options
        self.save_classes = save_classes or set()
        self.keep_actors = keep_actors or set()   # keyed fixtures a save row re-expresses
        self.stats = {"placed": 0, "instances": 0, "skipped_saveclass": 0,
                      "no_mesh": 0, "hidden": 0, "lights": 0, "sk_skipped": 0,
                      "landscape": 0, "culled": 0, "events": 0, "decals": 0,
                      "splines": 0, "unplaced": 0}
        self._world_m = {}
        self._actor = {}
        self._save_class = {}
        self._event_class = {}

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

    def _is_event_class(self, actor_type):
        """Event-scripted actor: hidden until its event fires -> the Events
        collection. The triggerBase_C family by hierarchy + the curated set."""
        r = self._event_class.get(actor_type)
        if r is None:
            r = actor_type in EVENT_ACTOR_CLASSES
            if not r and actor_type.endswith("_C"):
                pkg = self.game.class_package(actor_type)
                if pkg:
                    r = self.resolver.is_descendant(pkg, "triggerBase_C")
            self._event_class[actor_type] = r
        return r

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
        # PER-PROPERTY delta-else-template: a delta export that writes SOME
        # transform fields still inherits the rest from the class template
        # (cooked delta-vs-archetype is per field - the all-or-nothing form
        # zeroed template rotations whenever a delta carried only a location).
        p = self.dicts[idx].get("Properties") or {}
        t = self._template_for(idx)
        loc = _vec(p.get("RelativeLocation")) if "RelativeLocation" in p else \
            (t.rel_loc if t is not None else (0.0, 0.0, 0.0))
        rot = _rot(p.get("RelativeRotation")) if "RelativeRotation" in p else \
            (t.rel_rot if t is not None else (0.0, 0.0, 0.0))
        scale = _vec(p.get("RelativeScale3D"), (1.0, 1.0, 1.0)) \
            if "RelativeScale3D" in p else \
            (t.rel_scale if t is not None else (1.0, 1.0, 1.0))
        return convert.matrix_from_rotator(rot, loc, scale)

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

    # ---- per-property template fallbacks --------------------------------
    def _mesh_path_for(self, idx, ty):
        p = self.dicts[idx].get("Properties") or {}
        sm = p.get("StaticMesh")
        pkg = _ref_pkg(sm)
        if pkg.startswith("/Game/") or pkg.startswith("/Engine/"):
            return pkg
        t = self._template_for(idx)
        if t is not None and t.kind in ("SM", "ISM") and t.mesh:
            return t.mesh
        return ""

    def _comp_hidden(self, idx):
        """Per-flag delta-else-template visibility. Measured cases: fuse drafts
        (delta bVisible=False), rain-collision shell (delta bRenderInMainPass=False),
        template-hidden helpers."""
        p = self.dicts[idx].get("Properties") or {}
        t = self._template_for(idx)
        hid = p.get("bHiddenInGame", t.f_hidden if t else False)
        vis = p.get("bVisible", t.f_visible if t else True)
        rmp = p.get("bRenderInMainPass", t.f_render_main if t else True)
        return hid is True or vis is False or rmp is False

    def _mesh_allowed(self, idx, mesh_path):
        """/Game always; /Engine only when not a BasicShape, or a BasicShape that an
        override material turns into deliberate art (delta or template)."""
        if mesh_path.startswith("/Game/"):
            return True
        if not mesh_path.startswith("/Engine/"):
            return False
        if not mesh_path.startswith("/Engine/BasicShapes"):
            return True
        p = self.dicts[idx].get("Properties") or {}
        if "OverrideMaterials" in p:
            return True
        t = self._template_for(idx)
        return t is not None and t.has_override_mats

    def _instances_for(self, idx):
        """Instance matrices: the umap delta export's own native tail first; a
        delta with none inherits the class template's baked instances."""
        inst = None
        if self.paired:
            inst = getattr(self.em[idx].exportObject, "instance_matrices", None)
        if inst is not None and len(inst) > 0:
            return inst
        actor_idx = self._actor_of(idx)
        if actor_idx is None:
            return None
        at = self.dicts[actor_idx].get("Type", "")
        if not at.endswith("_C"):
            return None
        pkg = self.game.class_package(at)
        if not pkg:
            return None
        return self.resolver.template_instances(pkg, self.dicts[idx].get("Name", ""))

    # ---- run ------------------------------------------------------------
    def run(self, map_path, cols, progress):
        self.dicts = self.game.package_dict(map_path)
        self.pkg = self.game.load_package(map_path)
        self.em = self.pkg.ExportMap if self.pkg is not None else []
        self.paired = len(self.em) == len(self.dicts)

        self.by_actor_name = {}
        self.by_comp = {}
        for i, e in enumerate(self.dicts):
            if not isinstance(e, dict):
                continue
            nm, outer = e.get("Name"), e.get("Outer")
            self.by_actor_name.setdefault(nm, i)
            self.by_comp[(nm, outer)] = i

        # Runtime-arranged children: the base BP parks ChildActors under its
        # 'scene_dynamicClutter' scene component and places them from the event
        # graph at BeginPlay (measured: ladder1..5 + cargoLift_doors CATs all
        # serialize AT the base origin). Cooked transforms are the parking spot,
        # not the world truth -> their whole child chains go to the hidden
        # Unplaced collection.
        self._unplaced = set()
        pending = []
        for i, e in enumerate(self.dicts):
            if not isinstance(e, dict) or e.get("Type") != "ChildActorComponent":
                continue
            p = e.get("Properties") or {}
            if _ref(p.get("AttachParent"))[0] == "scene_dynamicClutter":
                tname = _ref(p.get("ChildActor"))[0]
                if tname:
                    pending.append(tname)
        while pending:
            nm = pending.pop()
            ai = self.by_actor_name.get(nm)
            if ai is None or ai in self._unplaced:
                continue
            self._unplaced.add(ai)
            for c in self.dicts:
                if isinstance(c, dict) and c.get("Type") == "ChildActorComponent" \
                        and c.get("Outer") == nm:
                    t2 = _ref((c.get("Properties") or {}).get("ChildActor"))[0]
                    if t2:
                        pending.append(t2)

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
                    a = self._actor_of(i)
                    if a is not None and a in self._unplaced:
                        self.stats["hidden"] += 1
                    else:
                        self._place_light(i, ty, cols["Lights"])
                continue
            if ty == "DecalComponent":
                if self.opt.get("import_decals", True):
                    self._place_decal(i, cols)
                continue
            if ty == "SplineMeshComponent":
                self._place_spline(i, cols)
                continue
            if ty == "SkeletalMeshComponent":
                self.stats["sk_skipped"] += 1
                continue
            if ty not in MESH_COMPS:
                continue
            if e.get("Name") in IMPOSTER_COMPONENTS:
                self.stats["hidden"] += 1
                continue
            actor_idx = self._actor_of(i)
            atype = self.dicts[actor_idx].get("Type", "") if actor_idx is not None else ""
            if atype and actor_idx not in self.keep_actors and self._class_is_save(atype):
                self.stats["skipped_saveclass"] += 1
                continue
            aprops = (self.dicts[actor_idx].get("Properties") or {}) \
                if actor_idx is not None else {}
            if aprops.get("bHidden") is True:
                self.stats["hidden"] += 1
                continue
            if self._comp_hidden(i):
                self.stats["hidden"] += 1
                continue
            mesh_path = self._mesh_path_for(i, ty)
            if not mesh_path or not self._mesh_allowed(i, mesh_path):
                self.stats["no_mesh"] += 1
                continue
            if is_technical(mesh_path, self.opt):
                self.stats["hidden"] += 1
                continue
            world = self._world_matrix(i)
            label = (self.dicts[actor_idx].get("Name") if actor_idx is not None
                     else e.get("Name")) or "map"
            is_event = self._is_event_class(atype)
            is_unplaced = actor_idx in self._unplaced
            # ensure_mesh only AFTER the radius gate: building meshes (and
            # their materials/textures) for culled far-map components wasted
            # bench time and left hundreds of zero-user datablocks
            if ty in INSTANCED:
                inst = self._instances_for(i)
                if inst is None or len(inst) == 0:
                    self.stats["no_mesh"] += 1
                    continue
                n = len(inst)
                take = range(n) if density >= 1.0 else range(0, n, max(1, int(1.0 / density)))
                keep = []
                for k in take:
                    m = world @ convert.ue_fmatrix_to_bl(inst[k])
                    if self.b.within(m.translation):
                        keep.append(m)
                    else:
                        self.stats["culled"] += 1
                if not keep:
                    continue
                me = self.b.ensure_mesh(mesh_path)
                if me is None:
                    self.stats["no_mesh"] += 1
                    continue
                target = cols["Unplaced"] if is_unplaced else (
                    cols["Events"] if is_event else (
                        cols["Foliage"] if ty == "FoliageInstancedStaticMeshComponent"
                        else cols["Statics"]))
                skey = "unplaced" if is_unplaced else ("events" if is_event else "instances")
                for m in keep:
                    self.b._new_object(label, me, target, m)
                    self.stats[skey] += 1
            else:
                if not self.b.within(world.translation):
                    self.stats["culled"] += 1
                    continue
                me = self.b.ensure_mesh(mesh_path)
                if me is None:
                    self.stats["no_mesh"] += 1
                    continue
                target = cols["Unplaced"] if is_unplaced else (
                    cols["Events"] if is_event else cols["Statics"])
                self.b._new_object(label, me, target, world)
                self.stats["unplaced" if is_unplaced
                           else ("events" if is_event else "placed")] += 1
        self._place_bsp(cols)
        return self.stats

    # ---- level BSP (the alpha bunker's structure) ------------------------
    def _bsp_material(self, idx):
        """FPackageIndex (import) -> the material's package path via ImportMap."""
        try:
            if idx >= 0:
                return ""
            im = self.pkg.ImportMap[-idx - 1]
            outer = getattr(im.OuterIndex, "Resource", None)
            if outer is not None:
                return str(getattr(outer, "ObjectName", ""))
        except Exception:  # noqa: BLE001
            pass
        return ""

    def _place_bsp(self, cols):
        """The level's own Model = cooked brush geometry (the alpha bunker's
        walls/floors, keyhole rooms). Brush-ACTOR models are volumes: only the
        Model outered directly to PersistentLevel renders."""
        if not self.paired:
            return
        for i, e in enumerate(self.dicts):
            if not isinstance(e, dict) or e.get("Type") != "Model" \
                    or e.get("Outer") != "PersistentLevel":
                continue
            mo = self.em[i].exportObject
            bsp = getattr(mo, "bsp", None)
            if not bsp or not bsp["polys"]:
                continue
            paths = [self._bsp_material(s[0]) for s in bsp["surfs"]]
            me = self.b.build_bsp(bsp, paths, e.get("Name", "bsp"))
            if me is None:
                continue
            self.b._new_object(e.get("Name", "bsp"), me, cols["Statics"],
                               Matrix.Identity(4))
            self.stats["bsp"] = self.stats.get("bsp", 0) + len(bsp["polys"])

    # ---- decals + spline meshes -----------------------------------------
    def _actor_gates(self, i):
        """Common actor-level gates -> (actor_idx, atype, target_kind) or None.
        target_kind: '' normal, 'events', 'unplaced'; None = skip entirely."""
        actor_idx = self._actor_of(i)
        atype = self.dicts[actor_idx].get("Type", "") if actor_idx is not None else ""
        if atype and actor_idx not in self.keep_actors and self._class_is_save(atype):
            self.stats["skipped_saveclass"] += 1
            return None
        aprops = (self.dicts[actor_idx].get("Properties") or {}) \
            if actor_idx is not None else {}
        if aprops.get("bHidden") is True:
            self.stats["hidden"] += 1
            return None
        kind = "unplaced" if actor_idx in self._unplaced else (
            "events" if self._is_event_class(atype) else "")
        return actor_idx, atype, kind

    def _place_decal(self, i, cols):
        gates = self._actor_gates(i)
        if gates is None:
            return
        actor_idx, atype, tkind = gates
        if self._comp_hidden(i):
            self.stats["hidden"] += 1
            return
        p = self.dicts[i].get("Properties") or {}
        label = (self.dicts[actor_idx].get("Name") if actor_idx is not None
                 else self.dicts[i].get("Name")) or "decal"
        # the game's runtime pick: per-type variant family (seeded per instance)
        mat = decals.grime_material(atype, label) or ""
        size = _vec(p.get("DecalSize"), (0.0, 0.0, 0.0))
        if not mat.startswith("/Game/"):
            mat = _ref_pkg(p.get("DecalMaterial"))
        info = None
        if (not mat.startswith("/Game/") or size == (0.0, 0.0, 0.0)) \
                and atype.endswith("_C"):
            pkg = self.game.class_package(atype)
            info = self.resolver.tree_info(pkg) if pkg else None
        if info is not None:
            dt = info["decals"].get(self.dicts[i].get("Name", ""))
            if not mat.startswith("/Game/"):
                mat = info["cdo_material"] or (str(dt["material"]) if dt else "")
            if size == (0.0, 0.0, 0.0) and dt is not None:
                size = tuple(dt["size"])
        if size == (0.0, 0.0, 0.0):
            size = (128.0, 256.0, 256.0)
        if not mat.startswith("/Game/"):
            self.stats["no_mesh"] += 1
            return
        world = self._world_matrix(i)
        if not self.b.within(world.translation):
            self.stats["culled"] += 1
            return
        spin = decals.grime_spin(atype, label)
        if spin is not None:
            world = world @ spin
        target = cols["Unplaced"] if tkind == "unplaced" else (
            cols["Events"] if tkind == "events" else cols["Decals"])
        self.b.queue_decal(label, target, world @ decals.size_matrix(size), mat)
        self.stats["unplaced" if tkind == "unplaced"
                   else ("events" if tkind == "events" else "decals")] += 1

    def _place_spline(self, i, cols):
        gates = self._actor_gates(i)
        if gates is None:
            return
        actor_idx, atype, tkind = gates
        if self._comp_hidden(i):
            self.stats["hidden"] += 1
            return
        p = self.dicts[i].get("Properties") or {}
        mesh_path = _ref_pkg(p.get("StaticMesh"))
        if not mesh_path.startswith("/Game/") or is_technical(mesh_path, self.opt):
            self.stats["no_mesh"] += 1
            return
        params = spline_mesh.parse_params(p)
        me = self.b.ensure_spline_mesh(mesh_path, params)
        if me is None:
            self.stats["no_mesh"] += 1
            return
        world = self._world_matrix(i)
        if not self.b.within(world.translation):
            self.stats["culled"] += 1
            return
        label = (self.dicts[actor_idx].get("Name") if actor_idx is not None
                 else self.dicts[i].get("Name")) or "spline"
        target = cols["Unplaced"] if tkind == "unplaced" else (
            cols["Events"] if tkind == "events" else cols["Statics"])
        self.b._new_object(label, me, target, world)
        self.stats["unplaced" if tkind == "unplaced"
                   else ("events" if tkind == "events" else "splines")] += 1

    def _place_light(self, idx, ty, col):
        p = self.dicts[idx].get("Properties") or {}
        ld = bpy.data.lights.new(self.dicts[idx].get("Name", "light"), LIGHT_COMPS[ty])
        intensity = float(p.get("Intensity", 5000.0) or 5000.0)
        ld.energy = max(1.0, intensity * 0.01)
        c = p.get("LightColor")
        if isinstance(c, dict):
            ld.color = (c.get("R", 255) / 255.0, c.get("G", 255) / 255.0,
                        c.get("B", 255) / 255.0)
        m = self._world_matrix(idx)
        if not self.b.within(m.translation):
            self.stats["culled"] += 1
            return
        ob = bpy.data.objects.new(ld.name, ld)
        ob.matrix_world = m
        ob.show_in_front = True   # USER: lights always visible through geometry
        col.objects.link(ob)
        self.stats["lights"] += 1
