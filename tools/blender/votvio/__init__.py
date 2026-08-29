"""VotvIO -- import a Voices of the Void .sav save into Blender as a full scene.

Reads the save (GVAS) plus the game's own pak directly (no manual export steps),
and reconstructs the world the way the game's loadObjects would express it.
Design of record: VOTV_MP repo, research/findings/tooling/
votv-blender-sav-importer-DESIGN-2026-08-29.md; living doc docs/BLENDER_ARC.md.
"""
import os
import sys

import bpy

bl_info = {
    "name": "VotvIO",
    "author": "pelmentor",
    "version": (0, 1, 0),
    "blender": (4, 2, 0),
    "location": "File > Import > VotV Save (.sav)",
    "description": "Import a Voices of the Void .sav (props, entities, NPC) straight from the game files",
    "category": "Import-Export",
}


def _bootstrap_vendor():
    """Make the vendored UE4Parse importable as a top-level package.

    UE4Parse uses absolute self-imports throughout, so it must live on sys.path.
    Only inserted when no UE4Parse is importable already (never shadow a user's own).
    """
    try:
        import UE4Parse  # noqa: F401
        return
    except ImportError:
        pass
    vendor = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor")
    if vendor not in sys.path:
        sys.path.insert(0, vendor)


_bootstrap_vendor()

from . import import_op  # noqa: E402
from . import prefs  # noqa: E402

_classes = (
    prefs.VotvIOPreferences,
    import_op.VOTVIO_OT_import_sav,
)


def _menu_import(self, context):
    self.layout.operator(import_op.VOTVIO_OT_import_sav.bl_idname, text="VotV Save (.sav)")


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
