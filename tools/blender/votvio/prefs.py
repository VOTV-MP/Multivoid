"""Addon preferences: where the game lives."""
import bpy

from . import game_dirs


class VotvIOPreferences(bpy.types.AddonPreferences):
    # Extensions load as bl_ext.<repo>.votvio; legacy as votvio. Use the real package name.
    bl_idname = __package__

    game_dir: bpy.props.StringProperty(
        name="VotV game folder",
        description="Any folder of the game install (the importer finds Content/Paks below it). "
                    "Leave empty to auto-detect",
        subtype="DIR_PATH",
        default="",
    )

    def draw(self, context):
        col = self.layout.column()
        col.prop(self, "game_dir")
        found = game_dirs.find_paks_dir(self.game_dir)
        if found:
            col.label(text=f"pak: {found}", icon="CHECKMARK")
        else:
            col.label(text="VotV-WindowsNoEditor.pak not found - set the game folder", icon="ERROR")


def get_prefs():
    addon = bpy.context.preferences.addons.get(__package__)
    return addon.preferences if addon else None
