#!/usr/bin/env python3
"""bp_reflect.py -- VOTV Blueprint reflection (extract + disassemble cooked BP logic).

WHY: VOTV's interactables (keypad, door, lightswitch, ...) are PURE Blueprint -- their
logic is compiled kismet bytecode in the cooked .uasset, NOT native code in the .exe, so
IDA cannot read them and our C++ reflection only sees field/function SIGNATURES, not LOGIC.
Born 2026-06-06: we burned a day GUESSING the keypad/door logic and got it wrong (e.g.
`isAcc` is a crosshair-HOVER flag, not "accepted"; the door has NO keypad lock -- the E-press
opens via doorOpen(bypassCheck=true) ignoring power). This tool ends the guessing: it reads
the REAL Blueprint graph.

PIPELINE: repak (unpack the game's unencrypted v11 .pak) -> kismet-analyzer `to-json`
(disassemble the kismet bytecode to structured JSON). Both are trumank's MIT CLI tools,
auto-downloaded to research/pak_re/tools/ on first run. Output: research/bp_reflection/<name>.json
(full disassembly, readable as structured EX_* expressions) + <name>.functions.txt (a quick
function index). Read the JSON directly, or hand it to an agent for a plain-English logic pass.

RULES: dev/RE tool ONLY -- nothing here ships (RULE 3). We READ the cooked assets for analysis;
we never modify or repack them (RULE 1, no original-file edits). The extracted assets + raw JSON
live under research/pak_re/ + research/bp_reflection/ which are gitignored (copyrighted game
content); the human-readable FINDINGS go in research/findings/ as usual.

Usage:
  python tools/bp_reflect.py passwordLock door lightswitch swinger   # disassemble by BP name
  python tools/bp_reflect.py --list garbageClump                     # just show matching pak paths
"""
import glob
import json
import os
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAK = os.path.join(ROOT, "Game_0.9.0n_HOST", "WindowsNoEditor", "VotV", "Content", "Paks", "VotV-WindowsNoEditor.pak")
REDIR = os.path.join(ROOT, "research", "pak_re")
TOOLS = os.path.join(REDIR, "tools")
EXTRACT = os.path.join(REDIR, "extracted")
OUT = os.path.join(ROOT, "research", "bp_reflection")

REPAK_URL = "https://github.com/trumank/repak/releases/download/v0.2.3/repak_cli-x86_64-pc-windows-msvc.zip"
KA_URL = "https://github.com/trumank/kismet-analyzer/releases/download/latest/kismet-analyzer-e8982e9-win-x64.zip"


def _find(pattern):
    hits = glob.glob(pattern, recursive=True)
    return hits[0] if hits else None


def ensure_tools():
    os.makedirs(TOOLS, exist_ok=True)
    repak = _find(os.path.join(TOOLS, "**", "repak.exe"))
    if not repak:
        print("  downloading repak ...")
        z = os.path.join(TOOLS, "repak.zip")
        urllib.request.urlretrieve(REPAK_URL, z)
        with zipfile.ZipFile(z) as zf:
            zf.extractall(TOOLS)
        repak = _find(os.path.join(TOOLS, "**", "repak.exe"))
    ka = _find(os.path.join(TOOLS, "**", "kismet-analyzer.exe"))
    if not ka:
        print("  downloading kismet-analyzer ...")
        z = os.path.join(TOOLS, "ka.zip")
        urllib.request.urlretrieve(KA_URL, z)
        with zipfile.ZipFile(z) as zf:
            zf.extractall(os.path.join(TOOLS, "kismet-analyzer"))
        ka = _find(os.path.join(TOOLS, "**", "kismet-analyzer.exe"))
    if not repak or not ka:
        sys.exit("FATAL: could not obtain repak/kismet-analyzer")
    return repak, ka


def pak_list(repak):
    cache = os.path.join(REDIR, "paklist.txt")
    if not os.path.exists(cache) or os.path.getsize(cache) == 0:
        os.makedirs(REDIR, exist_ok=True)
        env = dict(os.environ, NO_COLOR="1")
        with open(cache, "w", encoding="utf-8", errors="replace") as f:
            subprocess.run([repak, "list", PAK], stdout=f, env=env, check=True)
    return open(cache, encoding="utf-8", errors="replace").read().splitlines()


def find_asset(entries, name):
    needle = f"/{name.lower()}.uasset"
    cand = [e.strip() for e in entries if e.strip().lower().endswith(needle)]
    # Prefer a Blueprint home (objects/ or blueprints/) over a mesh/audio asset of the same name.
    cand.sort(key=lambda e: (0 if ("/objects/" in e.lower() or "blueprint" in e.lower()) else 1, len(e)))
    return cand[0] if cand else None


def disassemble(repak, ka, entries, name, list_only):
    asset = find_asset(entries, name)
    if not asset:
        print(f"[{name}] NOT FOUND in pak (try --list to search)")
        return
    print(f"[{name}] {asset}")
    if list_only:
        return
    ua = os.path.join(EXTRACT, asset.replace("/", os.sep))
    if not os.path.exists(ua):
        # repak include is directory/prefix based -> extract the containing dir (cached after first time).
        d = "/".join(asset.split("/")[:-1])
        subprocess.run([repak, "unpack", "-o", EXTRACT, "-i", d, PAK], check=True, stdout=subprocess.DEVNULL)
    if not os.path.exists(ua):
        print(f"  extract FAILED ({ua})")
        return
    os.makedirs(OUT, exist_ok=True)
    outjson = os.path.join(OUT, f"{name}.json")
    with open(outjson, "w", encoding="utf-8") as f:
        subprocess.run([ka, "to-json", ua], stdout=f, check=True)
    try:
        data = json.load(open(outjson, encoding="utf-8"))
        funcs = [e.get("ObjectName") for e in data.get("Exports", []) if "Function" in str(e.get("$type", ""))]
        with open(os.path.join(OUT, f"{name}.functions.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(x for x in funcs if x))
        print(f"  -> {os.path.relpath(outjson, ROOT)}  ({len(funcs)} functions)")
    except Exception as ex:  # noqa: BLE001 -- the JSON still landed; the index is a convenience
        print(f"  -> {os.path.relpath(outjson, ROOT)}  (function index skipped: {ex})")


def main():
    names = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not names:
        print(__doc__)
        return
    if not os.path.exists(PAK):
        sys.exit(f"FATAL: pak not found at {PAK}")
    repak, ka = ensure_tools()
    entries = pak_list(repak)
    list_only = "--list" in sys.argv
    for name in names:
        disassemble(repak, ka, entries, name, list_only)


if __name__ == "__main__":
    main()
