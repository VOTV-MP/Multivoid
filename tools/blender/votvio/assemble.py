"""Scene assembly: SceneManifest (+ GameSource) -> Blender collections/objects.

P1 scope: every save row placed; rows with resolvable static meshes get real geometry
(shared mesh datablocks), SK classes and unresolved classes get labelled placeholders.
The umap (static world) is the P2 lane and is not touched here.
"""
import json

import bpy

from . import convert
from . import landscape as landscape_mod
from . import materials as materials_mod
from . import mesh_build
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
        self.counts = {"meshed": 0, "placeholders": 0, "sk_placeholders": 0, "objects": 0}
        self.unresolved = {}

    # -- assets ------------------------------------------------------------
    def ensure_mesh(self, mesh_pkg_path):
        cache = self.caches["mesh"]
        if mesh_pkg_path in cache:
            return cache[mesh_pkg_path]
        me, mat_paths = mesh_build.build_mesh(self.game, mesh_pkg_path, self.warnings)
        if me is not None:
            for mp in mat_paths:
                me.materials.append(materials_mod.get_material(
                    self.game, mp, self.caches, self.warnings,
                    with_textures=self.opt.get("with_textures", True)))
        cache[mesh_pkg_path] = me
        return me

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

    def place_row(self, row, col):
        label = row.prop_name or row.class_name.removesuffix("_C") or "unknown"
        quat, loc, scale = row.transform
        actor_m = convert.matrix(quat, loc, scale)
        placed_mesh = False
        if self.game and self.opt.get("import_meshes", True) and row.class_path:
            for mesh_path, tmpl, kind in self.resolver.resolve_row(row, self.list_props):
                if kind == "SK":
                    self._placeholder(label + ".sk", col, actor_m, "CUBE", 0.4)
                    self.counts["sk_placeholders"] += 1
                    placed_mesh = True
                    continue
                me = self.ensure_mesh(mesh_path)
                if me is None:
                    continue
                m = actor_m
                if tmpl is not None:
                    m = actor_m @ convert.matrix_from_rotator(
                        tmpl.rel_rot, tmpl.rel_loc, tmpl.rel_scale)
                self._new_object(label, me, col, m)
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

        rows = self.m.objects
        total = len(rows) + len(self.m.primitives)
        for i, row in enumerate(rows):
            self.place_row(row, cols.get(row.category, cols["Props"]))
            if i % 50 == 0:
                self.progress(i, total, "props")
        for i, row in enumerate(self.m.primitives):
            quat, loc, scale = row.transform
            self._placeholder(row.class_name.removesuffix("_C") or "pile",
                              cols["Piles"], convert.matrix(quat, loc, scale),
                              "CUBE", 0.08)
            if i % 100 == 0:
                self.progress(len(rows) + i, total, "piles")

        if self.opt.get("show_contained", False) or True:  # always build; hide below
            for slot_rows in self.m.gobj_stack:
                for row in slot_rows:
                    if row.class_path and row.class_path != "None":
                        self.place_row(row, contained_col)
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
            self._placeholder("roach", cols["Piles"], convert.matrix(quat, loc, scale),
                              "SPHERE", 0.03)

        self.map_stats = {}
        if self.game and self.opt.get("import_map", True):
            map_path = self.game.find_content_package(self.m.level or "untitled_1")
            if map_path:
                map_col = _get_or_create_collection("Map", master)
                mcols = {name: _get_or_create_collection(name, map_col)
                         for name in ("Statics", "Landscape", "Foliage", "Lights")}
                imp = umap_import.MapImporter(self.game, self.resolver, self, self.opt)
                self.map_stats = imp.run(map_path, mcols, self.progress)
                if self.opt.get("import_landscape", True):
                    land_mat = bpy.data.materials.new("votv_landscape")
                    land_mat.diffuse_color = (0.18, 0.24, 0.12, 1.0)
                    self.map_stats["landscape"] = landscape_mod.build_landscape(
                        self.game, map_path, self.game.package_dict(map_path),
                        mcols["Landscape"], self.warnings, land_mat)
            else:
                self.warnings.append(f"map package not found for level {self.m.level!r}")

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
