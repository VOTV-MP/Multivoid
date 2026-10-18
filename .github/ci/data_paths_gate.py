#!/usr/bin/env python3
"""data_paths_gate -- the mod's own data is built from its own folder, never from the exe's.

Everything the mod keeps for itself (the ini, the server profile, the player profiles, the seen list,
the thanks cache, the identity key) lives in `ue_wrap::paths::ModDataDir()`, so a copied game folder
carries none of it and a manager's reinstall starts clean. `ue_wrap::paths::ExeDir()` is the game
exe's folder: it stays the home of the logs, the reports, the dumps and what a player authors (a
translator's `.po` files). A data builder written against `ExeDir()` again would put a file back
beside the exe, where a copy of the game folder carries it and a reinstall misses it. This gate holds,
over `src/votv-coop/src` (comments and string literals set aside):

  1. a call of `ExeDir(` stands only in the files of EXE_DIR_FILES, each of which builds a file that
     belongs beside the exe (a log, a report, a diagnostic, a rig signal, a translator's input, the
     game's own files read);
  2. a `GetModuleFileName`, `W` or `A`, whose module argument is null (`nullptr`, `NULL`, `0`, or a
     `GetModuleHandle` of null) -- the exe's PATH asked of Windows directly, the same folder by
     another road -- stands only in the files of EXE_PATH_FILES, all four of which read only.
     A `GetModuleHandle(nullptr)` used for the module's base is not a path and is not matched.

The lists are keyed by file, not by line: a line moves with every edit. A file added to a list is a
reviewed decision.

  python .github/ci/data_paths_gate.py                  # PASS/FAIL over the tree
  python .github/ci/data_paths_gate.py --tree <dir>     # the same over <dir>/src/votv-coop/src
  python .github/ci/data_paths_gate.py --drill          # the gate must fail each planted fault
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# What each file builds from ExeDir(): the reason it stays beside the exe.
EXE_DIR_FILES = {
    "ue_wrap/core/paths.cpp": "ExeDir itself, and ExistsBesideExe",
    "bootstrap/boot.cpp": "multivoid-loaded.txt, the install marker",
    "ue_wrap/core/log.cpp": "multivoid.log and multivoid.prev.log",
    "harness/compat_report.cpp": "the compat report",
    "coop/bug_report/report_files.cpp": "the bundle's entry for the compat report",
    "coop/bug_report/report_bundle.cpp": "multivoid_reports",
    "harness/fingerprint_capture.cpp": "the fingerprint capture, a dev diagnostic",
    "harness/screenshot.cpp": "coop-screenshots, a dev diagnostic",
    "coop/dev/dependency_dump.cpp": "the dev dump",
    "coop/dev/rehost_rejoin.cpp": "the rejoin trigger, a rig signal",
    "l10n/l10n.cpp": "multivoid_locale, a translator's own files, read only",
    "coop/player/skin_registry.cpp": "Content/Paks/LogicMods, the game's pak tree, read only",
    "harness/mod_environment.cpp": "Mods/mods.txt and LogicMods, a census of other mods, read only",
    "ue_wrap/core/game_build.cpp": "the game's main pak, read only",
}

# The files that may ask Windows for the exe's path with a null module; all read only.
EXE_PATH_FILES = {
    "ue_wrap/core/paths.cpp": "ExeDir itself",
    "loader/cppmod_entry.cpp": "the scan for an older mod DLL beside the exe",
    "harness/sdk_check.cpp": "the compatibility report's exe line",
    "ue_wrap/core/reflection.cpp": "LogGameVersion, the exe's file version",
}

EXE_DIR_CALL = re.compile(r"\bExeDir\s*\(")
NULL_ARG = r"(?:nullptr|NULL|0)"
EXE_PATH_CALL = re.compile(
    r"\bGetModuleFileName[WA]?\s*\(\s*(?:" + NULL_ARG +
    r"|(?:::\s*)?GetModuleHandle[WA]?\s*\(\s*" + NULL_ARG + r"\s*\))\s*,")
SUFFIXES = (".cpp", ".h", ".hpp", ".inc")
HEX_DIGITS = frozenset("0123456789abcdefABCDEF")  # a set: the empty string at a text edge is not a member


def number_run(text, i):
    """Whether the word behind text[i] starts with a digit: a number (1, 700, 0xFF, 0xAB'CD'EF; the walk
    crosses the earlier separators), where a quote is a separator; case'a' and u8'a' are words, and
    their quote opens a character literal."""
    j = i
    while j > 0 and (text[j - 1].isalnum() or text[j - 1] in "_'"):
        j -= 1
    return text[j:j + 1].isdigit()


def strip(text):
    """The code of a C++ text, its comments and string and character literals blanked (newlines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == "'" and text[i - 1:i] in HEX_DIGITS and text[i + 1:i + 2] in HEX_DIGITS and number_run(text, i):
            out.append(c)  # a C++14 digit separator (1'700'000'000), not the start of a character literal
            i += 1
        elif c == '"' or c == "'":
            m = re.match(r'([^()\\ ]{0,16})\(', text[i + 1:i + 19]) if c == '"' and text[i - 1:i] == "R" else None
            if m:
                end = text.find(")" + m.group(1) + '"', i)
                j = n if end < 0 else end + len(m.group(1)) + 2
            else:
                j = i + 1
                while j < n and text[j] != c:
                    j += 2 if text[j] == "\\" else 1
                j = min(n, j + 1)
            out.append("\n" * text.count("\n", i, j))
            out.append('""' if c == '"' else "''")
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check(files):
    """files: {path relative to src/votv-coop/src, '/'-separated: text}. Returns the faults."""
    faults = []
    for path, text in sorted(files.items()):
        code = strip(text)
        for m in EXE_DIR_CALL.finditer(code):
            if path not in EXE_DIR_FILES:
                faults.append("%s:%d: ExeDir( builds a path from the exe's folder; the mod's own data goes "
                              "through ModDataDir (allowed files: data_paths_gate.EXE_DIR_FILES)"
                              % (path, code.count("\n", 0, m.start()) + 1))
        for m in EXE_PATH_CALL.finditer(code):
            if path not in EXE_PATH_FILES:
                faults.append("%s:%d: GetModuleFileName of the null module is the exe's path by another "
                              "road (allowed files: data_paths_gate.EXE_PATH_FILES)"
                              % (path, code.count("\n", 0, m.start()) + 1))
    return faults


def tree_files(src):
    out = {}
    for p in sorted(src.rglob("*")):
        if p.is_file() and p.suffix in SUFFIXES:
            out[p.relative_to(src).as_posix()] = p.read_text(encoding="utf-8", errors="replace")
    return out


def drill():
    allowed_dir = next(iter(EXE_DIR_FILES))
    allowed_path = next(iter(EXE_PATH_FILES))
    call = "std::wstring p = ue_wrap::paths::ExeDir() + L\"\\\\multivoid.ini\";\n"
    cases = {
        "ExeDir call in a data builder": {"coop/config/config.cpp": call},
        "ExeDir call over two lines": {"coop/x/a.cpp": "auto d =\n    paths::ExeDir\n    ();\n"},
        "ExeDir call in a header": {"coop/x/a.h": "inline auto D() { return paths::ExeDir(); }\n"},
        "GetModuleFileNameW(nullptr": {"coop/x/a.cpp": "::GetModuleFileNameW(nullptr, buf, MAX_PATH);\n"},
        "GetModuleFileNameA(NULL": {"coop/x/a.cpp": "GetModuleFileNameA(NULL, buf, n);\n"},
        "GetModuleFileNameW(0": {"coop/x/a.cpp": "GetModuleFileNameW( 0 , buf, n);\n"},
        "GetModuleFileName of GetModuleHandle(nullptr)": {
            "coop/x/a.cpp": "::GetModuleFileNameW(::GetModuleHandleW(nullptr), exe, MAX_PATH);\n"},
        "ExeDir after a digit separator": {
            "coop/x/a.cpp": "constexpr long k = 1'700'000'000;\nauto d = paths::ExeDir();\n// it's\n"},
        "ExeDir after a 3-group hex literal": {
            "coop/x/a.cpp": "auto k = 0xAB'CD'EF; auto d = paths::ExeDir();\n"},
        "ExeDir after a letter-led hex group": {
            "coop/x/a.cpp": "auto k = 0xDEAD'BEEF'CAFE; auto d = paths::ExeDir();\n"},
        "ExeDir after case'a'": {"coop/x/a.cpp": "switch (x) { case'a': auto d = paths::ExeDir(); }\n"},
        "ExeDir after u8'a'": {"coop/x/a.cpp": "auto s = u8'a'; auto d = paths::ExeDir();\n"},
        "GetModuleFileName(NULL, over two lines": {
            "coop/x/a.cpp": "GetModuleFileNameW(\n    NULL,\n    buf, n);\n"},
    }
    clean = {
        "an allowed file calls ExeDir": {allowed_dir: call},
        "an allowed file asks the null module": {allowed_path: "GetModuleFileNameW(nullptr, p, n);\n"},
        "ExeDir in a comment": {"coop/x/a.cpp": "// the old ExeDir() place\n/* ExeDir( */\n"},
        "ExeDir in a string": {"coop/x/a.cpp": 'const char* s = "ExeDir()";\n'},
        "a module variable": {"coop/x/a.cpp": "GetModuleFileNameW(mod, buf, n);\n"},
        "the module's base": {"coop/x/a.cpp": "auto b = ::GetModuleHandleW(nullptr);\n"},
        "GetModuleFileNameEx of a process": {
            "coop/x/a.cpp": "K32GetModuleFileNameExW(proc, nullptr, path, MAX_PATH);\n"},
        "a character literal beside a digit separator": {
            "coop/x/a.cpp": "char c = 'a'; // ExeDir(\nint n = 0xFF'FF; char d = '\\'';\n"},
        "ModDataDir": {"coop/config/config.cpp": "auto d = ue_wrap::paths::ModDataDir();\n"},
    }
    bad = 0
    for name, files in cases.items():
        if not check(files):
            print("drill: the gate PASSED a planted fault: %s" % name)
            bad += 1
    for name, files in clean.items():
        if check(files):
            print("drill: the gate FAILED a clean file: %s" % name)
            bad += 1
    print("data_paths_gate drill: %s (%d planted fault(s), %d clean case(s))"
          % ("PASS" if not bad else "FAIL", len(cases), len(clean)))
    return 1 if bad else 0


def main():
    if "--drill" in sys.argv:
        return drill()
    base = ROOT
    if "--tree" in sys.argv:
        base = pathlib.Path(sys.argv[sys.argv.index("--tree") + 1])
    src = base / "src" / "votv-coop" / "src"
    if not src.is_dir():
        print("data_paths_gate: FAIL (no source folder %s)" % src)
        return 1
    files = tree_files(src)
    faults = check(files)
    for f in faults:
        print(f)
    print("data_paths_gate: %s (%d file(s) read, %d fault(s))"
          % ("PASS" if not faults else "FAIL", len(files), len(faults)))
    return 1 if faults else 0


if __name__ == "__main__":
    sys.exit(main())
