"""The one owner of pak access: mount, package cache, class->package resolution, DataTables.

Wraps the vendored (patched) pyUE4Parse. Everything downstream (template resolver,
mesh/texture builders) talks to a GameSource, never to the provider directly.
"""
import logging
import os

# CRITICAL: the vendored parser's known-cosmetic post-LOD tail failures on some
# StaticMesh exports (occluder/speedtree section) log at ERROR; geometry is intact.
logging.getLogger("UE4Parse").setLevel(logging.CRITICAL)

from UE4Parse.Provider import DefaultFileProvider  # noqa: E402
from UE4Parse.Versions import EUEVersion, VersionContainer  # noqa: E402
from UE4Parse.Assets.Objects.FGuid import FGuid  # noqa: E402

from . import bsp_model  # noqa: E402,F401  (registers the cooked-UModel reader)
from . import ism_component  # noqa: E402,F401  (registers the instanced-mesh export readers)
from . import sk_model  # noqa: E402,F401  (registers the cooked-USkeletalMesh reader)


class GameSource:
    def __init__(self, paks_dir):
        self.paks_dir = paks_dir
        self.provider = None
        self._pkg_cache = {}       # norm path -> Package or None
        self._dict_cache = {}      # norm path -> exports list
        self._base2path = {}       # lowercase basename -> [package paths]
        self.warnings = []

    # -- lifecycle ---------------------------------------------------------
    def mount(self):
        import gc
        gc.disable()
        try:
            self.provider = DefaultFileProvider(
                self.paks_dir, VersionContainer(EUEVersion.GAME_UE4_27))
            self.provider.initialize()
            mounted = self.provider.submit_key(FGuid(0, 0, 0, 0), None)
        finally:
            gc.enable()
        if not mounted:
            raise RuntimeError(f"no pak mounted from {self.paks_dir}")
        for k, _v in self.provider.files:
            s = str(k)
            if s.startswith("VotV/Content/"):
                self._base2path.setdefault(s.rsplit("/", 1)[-1].lower(), []).append(s)
        return self

    # -- packages ----------------------------------------------------------
    @staticmethod
    def norm(path):
        """'/Game/x/y' -> 'VotV/Content/x/y'; passthrough otherwise."""
        if path.startswith("/Game/"):
            return "VotV/Content/" + path[len("/Game/"):]
        return path

    def load_package(self, path):
        p = self.norm(path)
        if p in self._pkg_cache:
            return self._pkg_cache[p]
        try:
            pkg = self.provider.try_load_package(p)
        except Exception as e:  # noqa: BLE001 - a broken asset must not kill the import
            self.warnings.append(f"package load failed: {p}: {type(e).__name__} {e}")
            pkg = None
        self._pkg_cache[p] = pkg
        return pkg

    def package_dict(self, path):
        """Exports of a package as list-of-dicts (pyUE4Parse get_dict shape)."""
        p = self.norm(path)
        if p in self._dict_cache:
            return self._dict_cache[p]
        pkg = self.load_package(p)
        exports = []
        if pkg is not None:
            try:
                d = pkg.get_dict()
                exports = d if isinstance(d, list) else []
            except Exception as e:  # noqa: BLE001
                self.warnings.append(f"package dict failed: {p}: {type(e).__name__} {e}")
        self._dict_cache[p] = exports
        return exports

    def find_export(self, path, export_type):
        pkg = self.load_package(path)
        if pkg is None:
            return None
        try:
            return pkg.find_export_of_type(export_type)
        except Exception as e:  # noqa: BLE001
            self.warnings.append(f"{export_type} parse failed: {path}: {type(e).__name__} {e}")
            return None

    def class_package(self, class_name):
        """'door_C' -> 'VotV/Content/objects/door' (basename convention, verified by load).
        On a basename collision prefer the BP home (objects/): 'clocks_C' collides
        with the MESH package meshes/clocks/clocks (measured)."""
        base = class_name[:-2] if class_name.endswith("_C") else class_name
        cands = self._base2path.get(base.lower(), [])
        for cand in cands:
            if "/objects/" in cand.lower():
                return cand
        for cand in cands:
            return cand
        return None

    def find_content_package(self, basename):
        """Case-insensitive basename lookup ('Untitled_1' -> 'VotV/Content/maps/untitled_1')."""
        for cand in self._base2path.get(str(basename).lower(), []):
            return cand
        return None

    # -- data tables -------------------------------------------------------
    def datatable_rows(self, path):
        for e in self.package_dict(path):
            if isinstance(e, dict) and e.get("Type") == "DataTable":
                return e.get("Rows") or {}
        return {}

    def list_props(self):
        """list_props rows: prop name -> mesh package path ('' when none)."""
        if not hasattr(self, "_list_props"):
            table = {}
            rows = self.datatable_rows("VotV/Content/main/datatables/list_props")
            for name, row in rows.items():
                mesh = ""
                if isinstance(row, dict):
                    for k, v in row.items():
                        if k.startswith("mesh_") and isinstance(v, dict):
                            outer = v.get("OuterIndex") or {}
                            mesh = outer.get("ObjectName") or v.get("Outer") or ""
                            break
                table[str(name)] = str(mesh) if mesh else ""
            self._list_props = table
        return self._list_props


def open_game(paks_dir):
    if not paks_dir or not os.path.isdir(paks_dir):
        raise RuntimeError(f"paks dir not found: {paks_dir}")
    return GameSource(paks_dir).mount()
