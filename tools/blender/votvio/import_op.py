"""File > Import > VotV Save (.sav)."""
import time
import traceback

import bpy
from bpy_extras.io_utils import ImportHelper

from . import assemble, game_dirs, gvas, save_model


class VOTVIO_OT_import_sav(bpy.types.Operator, ImportHelper):
    bl_idname = "votvio.import_sav"
    bl_label = "Import VotV Save (.sav)"
    bl_description = "Read a Voices of the Void save and rebuild its world from the game files"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".sav"
    filter_glob: bpy.props.StringProperty(default="*.sav", options={"HIDDEN"})

    import_meshes: bpy.props.BoolProperty(
        name="Real meshes from the game pak",
        description="Resolve classes to their meshes and textures straight from "
                    "VotV-WindowsNoEditor.pak (needs the game folder in the addon "
                    "preferences). Off = placeholders only",
        default=True,
    )
    with_textures: bpy.props.BoolProperty(
        name="Textures", description="Decode and assign base-color textures", default=True)
    show_contained: bpy.props.BoolProperty(
        name="Show container contents",
        description="Items stored inside containers (GObjStack) - imported either way, "
                    "hidden unless enabled", default=False)
    placeholders: bpy.props.BoolProperty(
        name="Placeholders for unresolved",
        description="Put an empty where a class has no resolvable mesh yet", default=True)

    def invoke(self, context, event):
        if not self.filepath:
            d = game_dirs.default_save_dir()
            if d:
                self.filepath = d + "\\"
        return super().invoke(context, event)

    def execute(self, context):
        t0 = time.time()
        wm = context.window_manager
        try:
            header, props = gvas.parse_sav(self.filepath)
        except Exception as e:  # noqa: BLE001
            self.report({"ERROR"}, f"not a readable VotV save: {e}")
            return {"CANCELLED"}
        if "saveSlot" not in header.get("save_class", ""):
            self.report({"WARNING"},
                        f"unexpected save class {header.get('save_class')!r} - trying anyway")
        manifest = save_model.build_manifest(header, props)
        if manifest.level and manifest.level != "Untitled_1":
            self.report({"WARNING"},
                        f"save is on map '{manifest.level}' (not the main world) - "
                        "importing generically")

        game = None
        if self.import_meshes:
            from . import prefs, ue_provider
            p = prefs.get_prefs()
            paks = game_dirs.find_paks_dir(p.game_dir if p else "")
            if paks:
                try:
                    game = ue_provider.open_game(paks)
                except Exception as e:  # noqa: BLE001
                    traceback.print_exc()
                    self.report({"WARNING"}, f"pak mount failed ({e}) - placeholders only")
            else:
                self.report({"WARNING"},
                            "game pak not found - set the game folder in Preferences > "
                            "Add-ons > VotvIO; importing placeholders only")

        options = {
            "import_meshes": self.import_meshes and game is not None,
            "with_textures": self.with_textures,
            "show_contained": self.show_contained,
            "placeholders": self.placeholders,
        }
        wm.progress_begin(0, 100)
        try:
            rep = assemble.build_scene(
                manifest, game, options,
                progress=lambda done, total, label:
                    wm.progress_update(int(done * 100 / max(1, total))))
        except Exception as e:  # noqa: BLE001
            traceback.print_exc()
            self.report({"ERROR"}, f"import failed: {e}")
            return {"CANCELLED"}
        finally:
            wm.progress_end()

        dt = time.time() - t0
        self.report({"INFO"},
                    f"VotvIO: {rep['rows']} rows + {rep['primitives']} piles in {dt:.1f}s - "
                    f"{rep['meshed']} meshed, {rep['placeholders']} placeholders, "
                    f"{rep['distinct_meshes']} meshes, {rep['images']} textures "
                    f"({rep['warnings']} warnings - see votvio_manifest.json)")
        return {"FINISHED"}
