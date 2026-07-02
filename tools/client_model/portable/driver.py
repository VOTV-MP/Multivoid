#!/usr/bin/env python3
"""convert_model -- portable one-shot GoldSrc .mdl -> VOTV coop client-model .pak.

Drop the converter files next to a Half-Life 1 model and run it; the .pak appears
in the same folder (textures are read from inside the .mdl itself):

  convert.bat                       # or: convert_model.exe / python convert_model.pyz
  convert.bat barney.mdl            # pick the mdl explicitly when several are present
  ... --name hl_einstein_v1sc      # cook under a different package/pak name
  ... --profile my_profile.json    # override the embedded repose profile
  ... --keep-work                  # keep <name>_work/ (atlas.png preview, tpose.obj)

Pipeline (the exact repo recipe, docs/COOP_CLIENT_MODEL.md section 4):
  mdl_extract -> repose (default library profile) -> atlas -> ue_cook (skeletal mesh)
  -> ue_skelmesh round-trip validation -> ue_tex (atlas texture) -> repak pack.

The pipeline stages are the SAME modules as the repo tools -- make_portable.py bundles
them unmodified (single source of truth; this driver is the only portable-specific
code). Embedded data: the cook templates (kerfurOmega_KelSkin mesh, tex_kel3_skin
texture) + the default repose profile. repak.exe stays a sibling file on purpose.
Dev/RE tool (RULE 3) -- nothing here ships at mod runtime.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile

for _s in (sys.stdout, sys.stderr):          # cp1251/cp866 consoles: never die on print
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass

_here = os.path.dirname(os.path.abspath(__file__))
_repo_pkg = os.path.dirname(_here)                        # tools/client_model when in-repo
if os.path.isfile(os.path.join(_repo_pkg, "ue_cook.py")):
    sys.path.insert(0, _repo_pkg)

try:
    import numpy                                          # noqa: F401 -- fail early, clearly
    from PIL import Image                                 # noqa: F401
except ImportError as e:
    sys.exit(f"missing python package '{e.name}' -- run: pip install numpy pillow")

import atlas
import mdl_extract
import repose
import ue_cook
import ue_skelmesh
import ue_tex

DATA_FILES = ("kerfurOmega_KelSkin.uasset", "kerfurOmega_KelSkin.uexp",
              "tex_kel3_skin.uasset", "tex_kel3_skin.uexp", "profile.json")
_REPO_ROOT = os.path.dirname(os.path.dirname(_repo_pkg))
_REPO_DATA = {                                            # in-repo run: no bundle, read live files
    "kerfurOmega_KelSkin.uasset": "research/pak_re/extracted/VotV/Content/meshes/kerfurAnthro/sk/kerfurOmega_KelSkin.uasset",
    "kerfurOmega_KelSkin.uexp":   "research/pak_re/extracted/VotV/Content/meshes/kerfurAnthro/sk/kerfurOmega_KelSkin.uexp",
    "tex_kel3_skin.uasset":       "research/pak_re/extracted/VotV/Content/meshes/kel/4/tex_kel3_skin.uasset",
    "tex_kel3_skin.uexp":         "research/pak_re/extracted/VotV/Content/meshes/kel/4/tex_kel3_skin.uexp",
    "profile.json":               "tools/client_model/profiles/tpose_v1_narrow_2026-07-01.json",
}
DLL_MODEL_NAME = "hl_einstein_v1sc"          # the object path the coop DLL loads today


def _bundle_zip():
    """Inside a .pyz, __file__ = <archive>/__main__.py; the first EXISTING ancestor
    is the archive itself. Returns the archive path or None (plain-dir run)."""
    p = os.path.abspath(__file__)
    while p and not os.path.exists(p):
        parent = os.path.dirname(p)
        if parent == p:
            return None
        p = parent
    return p if os.path.isfile(p) and zipfile.is_zipfile(p) else None


def load_data(name):
    if getattr(sys, "frozen", False):                     # PyInstaller onefile
        return open(os.path.join(sys._MEIPASS, "data", name), "rb").read()
    z = _bundle_zip()
    if z:
        with zipfile.ZipFile(z) as zf:
            return zf.read("data/" + name)
    return open(os.path.join(_REPO_ROOT, _REPO_DATA[name]), "rb").read()


def find_repak():
    cands = []
    if getattr(sys, "frozen", False):
        cands.append(os.path.join(os.path.dirname(sys.executable), "repak.exe"))
    z = _bundle_zip()
    if z:
        cands.append(os.path.join(os.path.dirname(z), "repak.exe"))
    cands.append(os.path.join(os.getcwd(), "repak.exe"))
    cands.append(os.path.join(_REPO_ROOT, "research", "pak_re", "tools", "repak.exe"))
    for c in cands:
        if os.path.isfile(c):
            return c
    sys.exit("repak.exe not found -- keep it next to the converter files")


def pick_mdl(arg):
    if arg:
        if not os.path.isfile(arg):
            sys.exit(f"not found: {arg}")
        return os.path.abspath(arg)
    mdls = [f for f in sorted(os.listdir(".")) if f.lower().endswith(".mdl")]
    if len(mdls) > 1:
        # GoldSrc side files: <name>t.mdl external textures, <name>01.mdl sequence groups
        prim = [m for m in mdls if not re.search(r"(t|\d\d)\.mdl$", m.lower())]
        if len(prim) == 1:
            mdls = prim
    if len(mdls) != 1:
        sys.exit("expected exactly one .mdl in this folder (or pass one: convert.bat "
                 "<model.mdl>); found: " + (", ".join(mdls) or "none"))
    return os.path.abspath(mdls[0])


def main(argv=None):
    ap = argparse.ArgumentParser(prog="convert_model",
                                 description="GoldSrc .mdl -> VOTV coop client-model .pak")
    ap.add_argument("mdl", nargs="?", help="the model (default: the only .mdl here)")
    ap.add_argument("--name", help="package/pak name (default: mdl filename)")
    ap.add_argument("--profile", help="repose profile json (default: embedded library default)")
    ap.add_argument("--keep-work", action="store_true", help="keep the <name>_work folder")
    a = ap.parse_args(argv)

    mdl = pick_mdl(a.mdl)
    folder = os.path.dirname(mdl)
    stem = os.path.splitext(os.path.basename(mdl))[0]
    name = a.name or re.sub(r"[^A-Za-z0-9_]", "_", stem)
    work = os.path.join(folder, name + "_work")
    print(f"[convert] {os.path.basename(mdl)} -> {name}.pak")

    if os.path.isdir(work):
        if not os.path.isdir(os.path.join(work, "_data")):   # not ours -- refuse to wipe
            sys.exit(f"{work} exists and is not a converter work folder -- move it away")
        shutil.rmtree(work)
    dat = os.path.join(work, "_data")
    os.makedirs(dat)
    for n in DATA_FILES:
        open(os.path.join(dat, n), "wb").write(load_data(n))
    profile = os.path.abspath(a.profile) if a.profile else os.path.join(dat, "profile.json")

    print("[1/6] extract mdl")
    mdl_extract.extract(mdl, os.path.join(work, "extract"))
    print("[2/6] repose to the VOTV T-pose standard")
    repose.apply(os.path.join(work, "extract"), profile, os.path.join(work, "tpose.obj"))
    print("[3/6] pack the texture atlas")
    atlas.pack(os.path.join(work, "extract", "tex"), os.path.join(work, "atlas"))
    print("[4/6] cook the skeletal mesh")
    ue_cook.cook(os.path.join(dat, "kerfurOmega_KelSkin"), os.path.join(work, "tpose.obj"),
                 os.path.join(work, "extract", "model.bones.json"), os.path.join(work, name),
                 atlas_json=os.path.join(work, "atlas.json"))
    if not ue_skelmesh.main(os.path.join(work, name)):
        sys.exit(f"cooked mesh failed the round-trip parse -- aborting (work kept: {work})")
    print("[5/6] cook the atlas texture")
    ue_tex.cook(os.path.join(work, "atlas.png"), os.path.join(work, "tex_" + name),
                pkg_path="/Game/Mods/VOTVCoop/tex_" + name, obj_name="tex_" + name,
                template=os.path.join(dat, "tex_kel3_skin"))
    print("[6/6] pack the .pak")
    stage = os.path.join(work, "pak", "VotV", "Content", "Mods", "VOTVCoop")
    os.makedirs(stage)
    for f in (name + ".uasset", name + ".uexp", "tex_" + name + ".uasset", "tex_" + name + ".uexp"):
        shutil.copy2(os.path.join(work, f), stage)
    pak = os.path.join(folder, name + ".pak")
    subprocess.run([find_repak(), "pack", "--version", "V11", os.path.join(work, "pak"), pak],
                   check=True)

    print(f"\nDONE -> {pak}")
    print(f"  mesh object: /Game/Mods/VOTVCoop/{name}.kerfurOmega_KelSkin")
    print(f"  texture:     /Game/Mods/VOTVCoop/tex_{name}.tex_{name}")
    print(f"  install:     <Game>\\VotV\\Content\\Paks\\LogicMods\\votv-coop\\{name}.pak")
    if name != DLL_MODEL_NAME:
        print(f"  NOTE: the coop DLL currently loads '{DLL_MODEL_NAME}' -- to make THIS "
              f"model the one shown in game, rerun with --name {DLL_MODEL_NAME}")
    if a.keep_work:
        print(f"  work kept: {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
