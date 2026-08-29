"""Headless smoke: blender --background --factory-startup --python tests/smoke.py [-- <slot.sav>]

Parses a real save, mounts the real pak, builds the scene, prints the report.
Env: VOTVIO_PAKS to point at the paks dir; VOTVIO_SMOKE_BLEND=<path> to save the result.
"""
import os
import sys
import time

_ADDON_PARENT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _ADDON_PARENT)  # .../tools/blender

import votvio  # noqa: E402  (bootstraps the vendored UE4Parse path)
from votvio import assemble, game_dirs, gvas, save_model, ue_provider  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
sav = argv[0] if argv else os.path.join(
    os.environ.get("LOCALAPPDATA", ""), "VotV", "Saved", "SaveGames", "s_1234.sav")

t0 = time.time()
header, props = gvas.parse_sav(sav)
t_parse = time.time() - t0
manifest = save_model.build_manifest(header, props)
print(f"[sav] {os.path.basename(sav)}: {header['file_size']:,} B parsed in {t_parse:.1f}s; "
      f"class={header['save_class']} level={manifest.level} "
      f"rows={len(manifest.objects)} prims={len(manifest.primitives)} "
      f"contained={sum(len(s) for s in manifest.gobj_stack)}")

paks = game_dirs.find_paks_dir("")
print(f"[paks] {paks}")
t1 = time.time()
game = ue_provider.open_game(paks)
print(f"[mount] {time.time()-t1:.1f}s")

t2 = time.time()
report = assemble.build_scene(
    manifest, game,
    {"import_meshes": True, "with_textures": True, "show_contained": False,
     "placeholders": True, "import_map": True, "import_landscape": True,
     "import_lights": True, "import_decals": True, "foliage_density": 1.0,
     "terrain_style": "GREEN",
     "import_radius": float(os.environ.get("VOTVIO_SMOKE_RADIUS", "0") or 0)},
)
t_build = time.time() - t2
print(f"[build] {t_build:.1f}s")
print("[report]", report)
for w in game.warnings[:15]:
    print("  [warn]", w)

out = os.environ.get("VOTVIO_SMOKE_BLEND", "")
if out:
    import bpy
    # drop zero-user datablocks (meshes/materials/images built for content
    # that ended up culled) so the saved bench file stays clean
    bpy.data.orphans_purge(do_recursive=True)
    bpy.ops.wm.save_as_mainfile(filepath=out)
    print(f"[blend] saved {out}")
print(f"[total] {time.time()-t0:.1f}s  SMOKE-DONE")
