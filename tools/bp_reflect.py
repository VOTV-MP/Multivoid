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
import hashlib
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

# SHA-256 of each archive, checked BEFORE extraction; a mismatch aborts.
#
# WHY (external source review of the public tree, 2026-08-30): this script
# downloads two archives and then `subprocess.run`s the .exe files inside them,
# on a developer's machine with their privileges, and nothing verified a byte of
# it. Every third-party action in .github/workflows/ is SHA-pinned; this was the
# one place that wasn't.
#
# The review also suggested pinning KA_URL to a real tag instead of the mutable
# `latest`. MEASURED 2026-08-30: that is not available -- kismet-analyzer
# publishes exactly ONE release ("Development Build", tag `latest`, prerelease),
# so there is no immutable tag upstream to point at. That makes this hash the
# ONLY integrity control on that archive rather than a second one, which is why
# it fails closed.
#
# Honest about what this does and does not buy: these two values were taken from
# the copies already on disk, the ones every finding in research/bp_reflection/
# was produced with. So it pins REPRODUCIBILITY -- from here on, everyone gets
# the bytes this project's RE was done against, and a silent substitution
# upstream becomes a loud stop. It is NOT a claim that those bytes were ever
# audited. Updating a hash is deliberately a human decision, not a retry: read
# the abort message, confirm upstream really did rebuild, then edit this table.
TOOL_SHA256 = {
    REPAK_URL: "6720d602144d75df477a99d5bedb6ea780997546afc335901d4937cafeaa73fa",
    KA_URL: "12df3d6eb9d19e7de7aab1eaf0d43fee6955457671af38b9f1d0ad28fe57cc88",
}


def _fetch_verified(url, dest):
    """Download `url` to `dest`, abort unless it matches its recorded SHA-256."""
    expected = TOOL_SHA256.get(url)
    if not expected:
        sys.exit(f"FATAL: no SHA-256 recorded for {url} -- add one to TOOL_SHA256 "
                 f"before this script will run it")
    urllib.request.urlretrieve(url, dest)
    h = hashlib.sha256()
    with open(dest, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    got = h.hexdigest()
    if got != expected:
        os.remove(dest)  # never leave an unverified archive where a later run may trust it
        sys.exit(f"FATAL: checksum mismatch for {url}\n"
                 f"  expected {expected}\n"
                 f"  got      {got}\n"
                 f"This archive is downloaded and then EXECUTED, so it is not extracted.\n"
                 f"`latest` is a mutable tag, so upstream rebuilding it is the benign\n"
                 f"explanation -- but confirm that yourself before updating TOOL_SHA256.")


def _safe_extract(zip_path, dest_dir):
    """Extract `zip_path` into `dest_dir`, refusing entries that escape it.

    `ZipFile.extractall` honours absolute paths and `..` in member names (zip
    slip), and these archives are fetched over the network, so the member names
    are as untrusted as the bytes.
    """
    dest_root = os.path.realpath(dest_dir)
    with zipfile.ZipFile(zip_path) as zf:
        for member in zf.namelist():
            target = os.path.realpath(os.path.join(dest_root, member))
            if target != dest_root and not target.startswith(dest_root + os.sep):
                sys.exit(f"FATAL: refusing to extract '{member}' from {zip_path} -- "
                         f"it resolves outside {dest_root}")
        zf.extractall(dest_root)


def _find(pattern):
    hits = glob.glob(pattern, recursive=True)
    return hits[0] if hits else None


def ensure_tools():
    os.makedirs(TOOLS, exist_ok=True)
    repak = _find(os.path.join(TOOLS, "**", "repak.exe"))
    if not repak:
        print("  downloading repak ...")
        z = os.path.join(TOOLS, "repak.zip")
        _fetch_verified(REPAK_URL, z)
        _safe_extract(z, TOOLS)
        repak = _find(os.path.join(TOOLS, "**", "repak.exe"))
    ka = _find(os.path.join(TOOLS, "**", "kismet-analyzer.exe"))
    if not ka:
        print("  downloading kismet-analyzer ...")
        z = os.path.join(TOOLS, "ka.zip")
        _fetch_verified(KA_URL, z)
        _safe_extract(z, os.path.join(TOOLS, "kismet-analyzer"))
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
