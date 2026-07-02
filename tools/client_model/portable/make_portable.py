#!/usr/bin/env python3
"""make_portable -- build the drop-in-folder client-model converter bundle.

Assembles dist/ from the LIVE repo modules -- single source of truth: the originals
keep living in tools/client_model/ (user 2026-07-02), nothing is forked or copied
into version control. Rebuild after any pipeline-module change.

  dist/convert_model.pyz    zipapp: driver (__main__) + the seven pipeline modules
                            + data/ (mesh template, tex template, default profile)
  dist/repak.exe            the pak packer (a sibling file on purpose, per the user)
  dist/convert.bat          double-click launcher: exe if present, else py/python
  dist/README.txt           user instructions (RU)
  dist/convert_model.exe    with --exe: PyInstaller onefile, no python needed

Usage:  python tools/client_model/portable/make_portable.py [--exe]
dist/ + build/ are gitignored (game-derived template bytes + binaries).
Dev tool (RULE 3).
"""
import os
import shutil
import subprocess
import sys
import zipapp

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.dirname(HERE)                                   # tools/client_model
ROOT = os.path.dirname(os.path.dirname(PKG))                  # repo root
DIST = os.path.join(HERE, "dist")
BUILD = os.path.join(HERE, "build")

MODULES = ("mdl_extract.py", "repose.py", "atlas.py", "ue_cook.py", "ue_tex.py",
           "ue_skelmesh.py", "ue_pkg.py")
DATA = {   # bundle name -> repo source
    "kerfurOmega_KelSkin.uasset": "research/pak_re/extracted/VotV/Content/meshes/kerfurAnthro/sk/kerfurOmega_KelSkin.uasset",
    "kerfurOmega_KelSkin.uexp":   "research/pak_re/extracted/VotV/Content/meshes/kerfurAnthro/sk/kerfurOmega_KelSkin.uexp",
    "tex_kel3_skin.uasset":       "research/pak_re/extracted/VotV/Content/meshes/kel/4/tex_kel3_skin.uasset",
    "tex_kel3_skin.uexp":         "research/pak_re/extracted/VotV/Content/meshes/kel/4/tex_kel3_skin.uexp",
    "profile.json":               "tools/client_model/profiles/tpose_v1_narrow_2026-07-01.json",
}
REPAK = os.path.join(ROOT, "research", "pak_re", "tools", "repak.exe")

BAT = """@echo off
setlocal
cd /d "%~dp0"
if exist "%~dp0convert_model.exe" (
  "%~dp0convert_model.exe" %*
  goto :done
)
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 "%~dp0convert_model.pyz" %*
) else (
  python "%~dp0convert_model.pyz" %*
)
:done
echo.
pause
"""

README = """VOTV coop — конвертер модели клиента (GoldSrc .mdl -> .pak)
=============================================================

1. Положите файлы конвертера в папку с моделью (model.mdl; текстуры читаются
   из самого .mdl, отдельные .bmp не нужны).
2. Запустите convert.bat (или convert_model.exe, или: python convert_model.pyz).
3. В той же папке появится <имя>.pak.
4. Положите .pak в <игра>\\VotV\\Content\\Paks\\LogicMods\\votv-coop\\ на ВСЕХ пирах.

ВАЖНО: мод сейчас грузит модель по ФИКСИРОВАННОМУ имени hl_einstein_v1sc.
Чтобы увидеть ДРУГУЮ модель в игре, соберите её под этим именем:
    convert.bat mymodel.mdl --name hl_einstein_v1sc

Опции:
    --name <имя>         имя пакета/пака (по умолчанию = имя .mdl файла)
    --profile <json>     свой repose-профиль (по умолчанию встроен v1 narrow)
    --keep-work          оставить папку <имя>_work (atlas.png, tpose.obj — отладка)

Требования: convert_model.exe — ничего; convert_model.pyz — Python 3.9+ с
numpy и pillow (pip install numpy pillow). repak.exe должен лежать рядом.

Источник: tools/client_model/ в репо VOTV_MP; сборка make_portable.py.
"""


def build_pyz(stage):
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(os.path.join(stage, "data"))
    shutil.copy2(os.path.join(HERE, "driver.py"), os.path.join(stage, "__main__.py"))
    for m in MODULES:
        shutil.copy2(os.path.join(PKG, m), stage)
    for name, src in DATA.items():
        shutil.copy2(os.path.join(ROOT, src), os.path.join(stage, "data", name))
    out = os.path.join(DIST, "convert_model.pyz")
    zipapp.create_archive(stage, out, interpreter=None, compressed=True)
    print(f"  {out}  ({os.path.getsize(out)} B)")
    return stage


def build_exe(stage):
    try:
        import PyInstaller  # noqa: F401
    except ImportError:
        print("  PyInstaller not installed -- skipping the exe (pip install pyinstaller)")
        return False
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--console",
           "--name", "convert_model", "--distpath", DIST,
           "--workpath", os.path.join(BUILD, "pyi"), "--specpath", BUILD,
           "--paths", PKG,
           "--add-data", os.path.join(stage, "data") + os.pathsep + "data",
           os.path.join(HERE, "driver.py")]
    print("  " + " ".join(cmd[2:]))
    subprocess.run(cmd, check=True)
    exe = os.path.join(DIST, "convert_model.exe")
    print(f"  {exe}  ({os.path.getsize(exe)} B)")
    return True


def main():
    for name, src in DATA.items():
        p = os.path.join(ROOT, src)
        assert os.path.isfile(p), f"missing data source: {p}"
    assert os.path.isfile(REPAK), f"missing {REPAK}"
    os.makedirs(DIST, exist_ok=True)

    print("[make_portable] pyz:")
    stage = build_pyz(os.path.join(BUILD, "stage"))
    shutil.copy2(REPAK, os.path.join(DIST, "repak.exe"))
    open(os.path.join(DIST, "convert.bat"), "w", newline="\r\n").write(BAT)
    open(os.path.join(DIST, "README.txt"), "w", encoding="utf-8-sig").write(README)
    print(f"  repak.exe + convert.bat + README.txt -> {DIST}")

    if "--exe" in sys.argv:
        print("[make_portable] exe:")
        build_exe(stage)
    print("[make_portable] done.")


if __name__ == "__main__":
    main()
