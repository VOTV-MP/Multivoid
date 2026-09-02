#!/usr/bin/env python3
"""bp_cpp.py -- VOTV Blueprint DECOMPILER front-end (readable pseudocode from cooked BPs).

WHY: bp_cfg.py answers control-flow questions; this answers "what does the logic SAY"
in one readable file. Adopted 2026-09-02 from the UE-Modding-Tools survey, and the
verdict ORDER matters (it was measured, not read off READMEs -- the first survey pass
SKIPPED BlueprintToCpp on a README summary and the user's question forced the test
that flipped it):

  - BlueprintToCpp (Krowe-moh, MIT, CUE4Parse-based) is the PRIMARY lens: it
    decompiles EVERYTHING measured, including the three assets KismetKompiler dies
    on (ATV / mainPlayer / mainGamemode -- our hottest citation targets), with CDO
    property defaults inline (`float speed_turbo = 3200`) and full asset-path
    FindObject references. Validated 2026-09-02: ejectWheel reads as the real
    wheel-birth lane (deferred spawn of prop_atvWheel_C + durability/dirt/fixes
    seeding + velocity handoff); mainPlayer has exactly ONE `dead = true;` write and
    no `dead = false` (DEATH_ARC's latent-chain fact); mainGamemode shows the
    `UGameplayStatics::OpenLevel` hop the death arc detours.
  - Yangff's kismet-analyzer fork adds `decompile`: a statement listing where EVERY
    LINE CARRIES ITS BYTECODE OFFSET -- the exact `@offset` currency the docs cite.
    Use --offsets when writing a doc that pins facts to offsets.
  - KismetKompiler (research/pak_re/tools/KismetKompiler/) stays as a TERTIARY lens
    for small/mid BPs only (property-flag-rich .kms; UTF-16LE output).

BOTH tools are dotnet + source-built (no upstream release binaries), so this wrapper
does NOT auto-download; it locates the local builds and prints the exact build
commands when missing. Pins (also in the survey doc):
  BlueprintToCpp  commit a504452   research/pak_re/tools/src/BlueprintToCpp/
      git clone --recursive https://github.com/Krowe-moh/BlueprintToCpp
      dotnet build BlueprintToCpp.sln -c Release
  ka-yangff       commit 3a7122b   research/pak_re/tools/src/ka-yangff/
      git clone https://github.com/Yangff/kismet-analyzer ka-yangff
      git config submodule.UAssetAPI.url https://github.com/Yangff/UAssetAPI.git
      git submodule update --init && dotnet build -c Release

CULTURE: both run with DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 -- on a ru-RU box
floats otherwise render with COMMA decimals ("0,9" inside an argument list),
measured 2026-09-02 and fixed by the invariant culture.

RULES: dev/RE tool ONLY -- nothing ships (RULE 3). Read-only on game assets
(RULE 1). Outputs land in research/bp_reflection/cpp/ (gitignored -- derived game
content); distilled findings go to research/findings/ as usual.

Usage:
  python tools/bp_cpp.py ATV                    # -> research/bp_reflection/cpp/ATV.cpp
  python tools/bp_cpp.py mainPlayer --offsets   # + mainPlayer.offsets.txt (Yangff)
"""
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bp_reflect as br  # noqa: E402
import bp_cfg as bc      # noqa: E402  (ensure_extracted for the --offsets input)

OUT = os.path.join(br.ROOT, "research", "bp_reflection", "cpp")
SRC = os.path.join(br.TOOLS, "src")
B2C_EXE = os.path.join(SRC, "BlueprintToCpp", "BlueprintToCpp", "bin", "Release",
                       "net8.0", "Main.exe")
YFF_EXE = os.path.join(SRC, "ka-yangff", "bin", "Release", "net7.0",
                       "kismet-analyzer.exe")

ENV = dict(os.environ,
           DOTNET_ROLL_FORWARD="LatestMajor",
           DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="1")


def _require(exe, name, build_hint):
    if not os.path.exists(exe):
        sys.exit(f"FATAL: {name} not built at {exe}\n  build it:\n{build_hint}\n"
                 f"  (pins + provenance: the module docstring and the 2026-09-02 survey doc)")


def decompile_cpp(asset):
    """BlueprintToCpp: config.json beside its exe, output mirrors the pak path."""
    _require(B2C_EXE, "BlueprintToCpp",
             "    cd research/pak_re/tools/src/BlueprintToCpp && "
             "git submodule update --init --depth 1 && "
             "dotnet build BlueprintToCpp.sln -c Release")
    exedir = os.path.dirname(B2C_EXE)
    cfg = {"PakFolderPath": os.path.dirname(br.PAK).replace("\\", "/"),
           "BlueprintPath": asset,
           "UsmapPath": "",
           "Version": "GAME_UE4_27"}
    with open(os.path.join(exedir, "config.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    subprocess.run([B2C_EXE], cwd=exedir, env=ENV, check=True,
                   stdout=subprocess.DEVNULL)
    produced = os.path.join(exedir, os.path.splitext(asset)[0].replace("/", os.sep) + ".cpp")
    if not os.path.exists(produced):
        sys.exit(f"FATAL: BlueprintToCpp reported no output at {produced}")
    return produced


def decompile_offsets(ua, outdir):
    """Yangff fork: offset-annotated statement listing from the extracted .uasset."""
    _require(YFF_EXE, "ka-yangff (Yangff kismet-analyzer fork)",
             "    cd research/pak_re/tools/src/ka-yangff && "
             "git config submodule.UAssetAPI.url https://github.com/Yangff/UAssetAPI.git && "
             "git submodule update --init --depth 1 && dotnet build -c Release")
    subprocess.run([YFF_EXE, "decompile", ua, outdir], env=ENV, check=True,
                   stdout=subprocess.DEVNULL)
    stem = os.path.splitext(os.path.basename(ua))[0]
    produced = os.path.join(outdir, stem + ".txt")
    return produced if os.path.exists(produced) else None


def main():
    args = sys.argv[1:]
    offsets = "--offsets" in args
    names = [a for a in args if not a.startswith("--")]
    if not names:
        print(__doc__)
        return
    if not os.path.exists(br.PAK):
        sys.exit(f"FATAL: pak not found at {br.PAK}")
    repak, _ = br.ensure_tools()
    entries = br.pak_list(repak)
    os.makedirs(OUT, exist_ok=True)
    for name in names:
        asset = br.find_asset(entries, name)
        if not asset:
            print(f"[{name}] NOT FOUND in pak (try bp_reflect.py --list)")
            continue
        print(f"[{name}] {asset}")
        produced = decompile_cpp(asset)
        dest = os.path.join(OUT, name + ".cpp")
        shutil.copyfile(produced, dest)
        print(f"  -> {os.path.relpath(dest, br.ROOT)}  ({os.path.getsize(dest):,} bytes)")
        if offsets:
            ua = bc.ensure_extracted(repak, asset)
            listing = decompile_offsets(ua, OUT) if ua else None
            if listing:
                final = os.path.join(OUT, name + ".offsets.txt")
                if os.path.abspath(listing) != os.path.abspath(final):
                    shutil.move(listing, final)
                print(f"  -> {os.path.relpath(final, br.ROOT)}  (every line carries its bytecode offset)")
            else:
                print("  --offsets: Yangff decompile produced no listing (see its stderr)")


if __name__ == "__main__":
    main()
