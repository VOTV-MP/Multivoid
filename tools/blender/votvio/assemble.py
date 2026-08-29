"""Scene assembly: SceneManifest (+ GameSource) -> Blender collections/objects.

P1 scope: every save row placed; rows with resolvable static meshes get real geometry
(shared mesh datablocks), SK classes and unresolved classes get labelled placeholders.
The umap (static world) is the P2 lane and is not touched here.
"""
import json

import bpy

from . import convert
from . import decals as decals_mod
from . import landscape as landscape_mod
from . import materials as materials_mod
from . import mesh_build
from . import spline_mesh
from . import template_resolver
from . import umap_import


def _get_or_create_collection(name, parent):
    for c in parent.children:
        if c.name == name or c.name.startswith(name + "."):
            return c
    c = bpy.data.collections.new(name)
    parent.children.link(c)
    return c


class _Builder:
    def __init__(self, manifest, game, options, progress=None):
        self.m = manifest
        self.game = game
        self.opt = options
        self.progress = progress or (lambda done, total, label: None)
        self.warnings = []
        self.caches = {"mat": {}, "img": {}, "mesh": {}}
        self.resolver = template_resolver.TemplateResolver(game) if game else None
        self.list_props = game.list_props() if game else {}
        self.counts = {"meshed": 0, "placeholders": 0, "sk_placeholders": 0, "objects": 0,
                       "culled": 0, "level_kept": 0, "decals_projected": 0,
                       "decals_missed": 0}
        self.decal_queue = []   # (name, collection, world matrix, material path)
        self.unresolved = {}
        self.radius = float(options.get("import_radius", 0.0) or 0.0)
        self.origin = None  # Blender-space Vector, set in run() when radius > 0
        self.keep_actors = set()   # umap actor idx kept alive by a matching keyed row
        self._level_keys = {}      # save key -> (actor idx, actor type, UE root loc)

    def within(self, loc_bl):
        """Import-radius test (XY, meters). True when no radius is set."""
        if self.radius <= 0.0 or self.origin is None:
            return True
        dx = loc_bl[0] - self.origin[0]
        dy = loc_bl[1] - self.origin[1]
        return (dx * dx + dy * dy) <= self.radius * self.radius

    def _find_base_origin(self):
        """The base (garage / coordinate panels) = the baseBuilding_C actor's root."""
        map_path = self.game.find_content_package(self.m.level or "untitled_1") \
            if self.game else None
        if map_path:
            dicts = self.game.package_dict(map_path)
            by_name = {e.get("Name"): e for e in dicts if isinstance(e, dict)}
            for e in dicts:
                if isinstance(e, dict) and e.get("Type") == "baseBuilding_C" \
                        and e.get("Outer") == "PersistentLevel":
                    root = by_name.get(str(((e.get("Properties") or {}).get("RootComponent")
                                            or {}).get("ObjectName", "")))
                    rl = ((root or {}).get("Properties") or {}).get("RelativeLocation")
                    if isinstance(rl, dict):
                        return convert.pos((rl.get("X", 0), rl.get("Y", 0), rl.get("Z", 0)))
        if self.m.player_transform:  # fallback: wherever the save's player is
            _q, loc, _s = self.m.player_transform
            return convert.pos(loc)
        return convert.pos((0.0, 0.0, 0.0))

    # -- assets ------------------------------------------------------------
    def _append_materials(self, me, mesh_pkg_path, mat_paths):
        base = mesh_pkg_path.rsplit("/", 1)[-1]
        for mp in mat_paths:
            mp = materials_mod.resolve_slot(base, mp)
            me.materials.append(materials_mod.get_material(
                self.game, mp, self.caches, self.warnings,
                with_textures=self.opt.get("with_textures", True)))

    def ensure_mesh(self, mesh_pkg_path):
        cache = self.caches["mesh"]
        if mesh_pkg_path in cache:
            return cache[mesh_pkg_path]
        me, mat_paths = mesh_build.build_mesh(self.game, mesh_pkg_path, self.warnings)
        if me is not None:
            self._append_materials(me, mesh_pkg_path, mat_paths)
        cache[mesh_pkg_path] = me
        return me

    def ensure_spline_mesh(self, mesh_pkg_path, params):
        cache = self.caches["mesh"]
        key = (mesh_pkg_path, spline_mesh.cache_key(params))
        if key in cache:
            return cache[key]
        me, mat_paths = mesh_build.build_spline_mesh(
            self.game, mesh_pkg_path, params, self.warnings)
        if me is not None:
            self._append_materials(me, mesh_pkg_path, mat_paths)
        cache[key] = me
        return me

    def build_bsp(self, bsp, surf_paths, name):
        me, unique = mesh_build.build_bsp_mesh(bsp, surf_paths, name, self.warnings)
        if me is not None:
            for mp in unique:
                me.materials.append(materials_mod.get_material(
                    self.game, mp if mp.startswith("/Game/") else "", self.caches,
                    self.warnings, with_textures=self.opt.get("with_textures", True)))
        return me

    def queue_decal(self, name, col, matrix, mat_path):
        """Decals are projected in one post-pass once the receivers exist."""
        self.decal_queue.append((name, col, matrix, mat_path))

    def _project_decals(self):
        if not self.decal_queue:
            return
        # receivers = structure only (Statics incl. BSP, Landscape): grime in
        # the game lands on walls/floors; hiding props/piles/foliage for the
        # cast shrinks the ray scene ~6x (scene.ray_cast walks every object)
        toggled = []
        for c in bpy.data.collections:
            base = c.name.split(".")[0]
            if base in ("Props", "NPC", "Vehicles", "Piles", "Foliage",
                        "Contained", "Player") and not c.hide_viewport:
                c.hide_viewport = True
                toggled.append(c)
        bpy.context.view_layer.update()
        deps = bpy.context.evaluated_depsgraph_get()
        scene = bpy.context.scene
        built = []
        for name, col, matrix, mat_path in self.decal_queue:
            res = decals_mod.project_decal(scene, deps, matrix)
            if res is None:
                self.counts["decals_missed"] += 1
                continue
            verts, faces, uvs = res
            me = bpy.data.meshes.new(name + ".decal")
            me.from_pydata(verts, [], faces)
            try:
                layer = me.uv_layers.new(name="UVMap")
                for li, loop in enumerate(me.loops):
                    layer.data[li].uv = uvs[loop.vertex_index]
            except Exception:  # noqa: BLE001
                pass
            me.materials.append(materials_mod.get_decal_material(
                self.game, mat_path, self.caches, self.warnings,
                with_textures=self.opt.get("with_textures", True)))
            me.validate()
            built.append((name, me, col))
        for c in toggled:
            c.hide_viewport = False
        # link only after every ray is cast: a linked decal would itself
        # intercept the next decal's rays
        for name, me, col in built:
            ob = bpy.data.objects.new(name, me)
            col.objects.link(ob)
            self.counts["objects"] += 1
            self.counts["decals_projected"] += 1

    # -- objects -----------------------------------------------------------
    def _new_object(self, name, data, col, matrix):
        ob = bpy.data.objects.new(name, data)
        ob.matrix_world = matrix
        col.objects.link(ob)
        self.counts["objects"] += 1
        return ob

    def _placeholder(self, name, col, matrix, kind="PLAIN_AXES", size=0.15):
        ob = self._new_object(name, None, col, matrix)
        ob.empty_display_type = kind
        ob.empty_display_size = size
        return ob

    def _build_level_keys(self, save_classes):
        """key -> keyed LEVEL actor (the game's own gatherDataFromKey identity).
        A save row matching a level actor's key AT the cooked transform means the
        actor is a fixture whose state the row carries: keep the cooked components
        (they hold the exact UCS-built ISM instances the class template lacks)."""
        map_path = self.game.find_content_package(self.m.level or "untitled_1")
        if not map_path:
            return
        dicts = self.game.package_dict(map_path)
        by_comp = {}
        for e in dicts:
            if isinstance(e, dict):
                by_comp[(e.get("Name"), e.get("Outer"))] = e
        for i, e in enumerate(dicts):
            if not isinstance(e, dict) or e.get("Outer") != "PersistentLevel":
                continue
            atype = e.get("Type", "")
            if atype not in save_classes:
                continue
            p = e.get("Properties") or {}
            key = p.get("key")
            if not isinstance(key, str) or key in ("", "None"):
                pkg = self.game.class_package(atype)
                key = self.resolver.cdo_key(pkg) if pkg else ""
            if not key:
                continue
            root = by_comp.get((str((p.get("RootComponent") or {}).get("ObjectName", "")),
                                e.get("Name")))
            rl = ((root or {}).get("Properties") or {}).get("RelativeLocation")
            loc = (rl.get("X", 0.0), rl.get("Y", 0.0), rl.get("Z", 0.0)) \
                if isinstance(rl, dict) else None
            self._level_keys[key] = (i, atype, loc)

    def place_row(self, row, col, allow_level_keep=False, queue_decals=True):
        label = row.prop_name or row.class_name.removesuffix("_C") or "unknown"
        quat, loc, scale = row.transform
        if allow_level_keep and row.key not in ("", "None") and self._level_keys:
            hit = self._level_keys.get(row.key)
            if hit is not None and hit[1] == row.class_name and hit[2] is not None \
                    and all(abs(a - b) <= 1.0 for a, b in zip(loc, hit[2])):
                self.keep_actors.add(hit[0])
                self.counts["level_kept"] += 1
                return
        actor_m = convert.matrix(quat, loc, scale)
        if not self.within(actor_m.translation):
            self.counts["culled"] += 1
            return
        seed = row.key if row.key not in ("", "None") else \
            f"{row.class_name}@{round(loc[0])}:{round(loc[1])}"
        placed_mesh = False
        if self.game and self.opt.get("import_meshes", True) and row.class_path:
            for mesh_path, local_m, kind in self.resolver.spawn_plan(
                    row, self.list_props, seed):
                if kind == "DECAL":
                    if queue_decals and self.opt.get("import_decals", True):
                        self.queue_decal(label + ".decal", col,
                                         actor_m @ local_m, mesh_path)
                        self.counts["meshed"] += 1
                        placed_mesh = True
                    continue
                if umap_import.is_technical(mesh_path, self.opt):
                    continue
                if kind == "SK":
                    self._placeholder(label + ".sk", col, actor_m @ local_m, "CUBE", 0.4)
                    self.counts["sk_placeholders"] += 1
                    placed_mesh = True
                    continue
                me = self.ensure_mesh(mesh_path)
                if me is None:
                    continue
                self._new_object(label, me, col, actor_m @ local_m)
                self.counts["meshed"] += 1
                placed_mesh = True
        if not placed_mesh:
            if self.opt.get("placeholders", True):
                self._placeholder(label, col, actor_m)
            self.counts["placeholders"] += 1
            self.unresolved[row.class_name] = self.unresolved.get(row.class_name, 0) + 1

    # -- top level ---------------------------------------------------------
    def run(self):
        slot = self.m.scalars.get("Level", "VotV") or "VotV"
        master = bpy.data.collections.new(f"VotvIO {slot}")
        bpy.context.scene.collection.children.link(master)
        save_col = _get_or_create_collection("Save", master)
        cols = {cat: _get_or_create_collection(cat, save_col)
                for cat in ("Props", "NPC", "Vehicles", "Piles")}
        contained_col = _get_or_create_collection("Contained", master)
        player_col = _get_or_create_collection("Player", master)

        if self.radius > 0.0:
            self.origin = self._find_base_origin()

        rows = self.m.objects
        if self.game and self.opt.get("import_map", True) and \
                self.opt.get("import_meshes", True):
            save_classes = {r.class_name for r in rows
                            if r.class_path and r.class_path != "None"}
            self._build_level_keys(save_classes)
        total = len(rows) + len(self.m.primitives)
        for i, row in enumerate(rows):
            self.place_row(row, cols.get(row.category, cols["Props"]),
                           allow_level_keep=True)
            if i % 50 == 0:
                self.progress(i, total, "props")
        for i, row in enumerate(self.m.primitives):
            # primitivesData is load-decisive for int_primitive classes: grime
            # marks resolve to their class-template DECALS, piles to whatever
            # visuals the resolver can offer (placeholder empty otherwise)
            self.place_row(row, cols["Piles"])
            if i % 100 == 0:
                self.progress(len(rows) + i, total, "piles")

        if self.opt.get("show_contained", False) or True:  # always build; hide below
            for slot_rows in self.m.gobj_stack:
                for row in slot_rows:
                    if row.class_path and row.class_path != "None":
                        # no decals for stored items: their transforms are stale
                        # world coords and the projection would land on nothing
                        self.place_row(row, contained_col, queue_decals=False)
        contained_col.hide_viewport = not self.opt.get("show_contained", False)
        contained_col.hide_render = True

        for row in self.m.equipment + self.m.inventory:
            self.place_row(row, player_col)
        if self.m.player_transform:
            quat, loc, scale = self.m.player_transform
            m = convert.matrix(quat, loc, scale)
            self._placeholder("Player", player_col, m, "ARROWS", 0.5)
            cam = bpy.data.cameras.new("PlayerCam")
            cam_ob = self._new_object("PlayerCam", cam, player_col, m)
            cam_ob.matrix_world = m
            cam_ob.location.z += 0.65
            cam_ob.rotation_mode = "XYZ"
            cam_ob.rotation_euler.x += 1.5708  # look forward, not down
        for t in self.m.roaches:
            quat, loc, scale = t
            rm = convert.matrix(quat, loc, scale)
            if self.within(rm.translation):
                self._placeholder("roach", cols["Piles"], rm, "SPHERE", 0.03)

        self.map_stats = {}
        if self.game and self.opt.get("import_map", True):
            map_path = self.game.find_content_package(self.m.level or "untitled_1")
            if map_path:
                map_col = _get_or_create_collection("Map", master)
                mcols = {name: _get_or_create_collection(name, map_col)
                         for name in ("Statics", "Landscape", "Foliage", "Lights",
                                      "Decals", "Events", "Unplaced")}
                save_classes = {r.class_name for r in self.m.objects
                                if r.class_path and r.class_path != "None"}
                imp = umap_import.MapImporter(self.game, self.resolver, self, self.opt,
                                              save_classes, self.keep_actors)
                self.map_stats = imp.run(map_path, mcols, self.progress)
                # event-scripted level actors: imported for completeness, hidden --
                # the game shows them only from their Blueprints at runtime
                mcols["Events"].hide_viewport = True
                mcols["Events"].hide_render = True
                # runtime-arranged ChildActors (dynamicClutter): parked transforms
                mcols["Unplaced"].hide_viewport = True
                mcols["Unplaced"].hide_render = True
                if self.opt.get("import_landscape", True):
                    land_mat = materials_mod.terrain_material(
                        self.opt.get("terrain_style", "GREEN"))
                    self.map_stats["landscape"] = landscape_mod.build_landscape(
                        self.game, map_path, self.game.package_dict(map_path),
                        mcols["Landscape"], self.warnings, land_mat, builder=self)
            else:
                self.warnings.append(f"map package not found for level {self.m.level!r}")

        self._project_decals()
        self._write_manifest_text()
        return self._report()

    def _write_manifest_text(self):
        info = {
            "level": self.m.level,
            "scalars": {k: (list(v) if isinstance(v, tuple) else v)
                        for k, v in self.m.scalars.items()},
            "counts": {
                "objectsData": len(self.m.objects),
                "primitivesData": len(self.m.primitives),
                "gobj_stack_items": sum(len(s) for s in self.m.gobj_stack),
                "equipment": len(self.m.equipment),
                "inventory": len(self.m.inventory),
                "roaches": len(self.m.roaches),
            },
            "placed": self.counts,
            "map": getattr(self, "map_stats", {}),
            "unresolved_classes": dict(sorted(self.unresolved.items(),
                                              key=lambda kv: -kv[1])),
            "non_scene_save_keys": self.m.non_scene_keys,
            "warnings": self.warnings[:200],
        }
        txt = bpy.data.texts.new("votvio_manifest.json")
        txt.write(json.dumps(info, indent=1, ensure_ascii=False, default=str))

    def _report(self):
        return {
            "rows": len(self.m.objects),
            "primitives": len(self.m.primitives),
            **self.counts,
            "map": getattr(self, "map_stats", {}),
            "distinct_meshes": len(self.caches["mesh"]),
            "materials": len(self.caches["mat"]),
            "images": len([i for i in self.caches["img"].values() if i]),
            "warnings": len(self.warnings),
            "unresolved_top": sorted(self.unresolved.items(), key=lambda kv: -kv[1])[:10],
        }


def build_scene(manifest, game, options, progress=None):
    return _Builder(manifest, game, options, progress).run()
