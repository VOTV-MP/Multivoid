#!/usr/bin/env python3
"""tools/i18n/apply.py -- wrap the mod's own UI strings so a language pack can replace them.

Pairs with extract.py: that one mints the key list out of the source, this one puts the lookup at
the call sites. A literal is only touched when its RUNTIME value is a key in the template, so the
widget names, ini keys and log format strings that share the files are left alone by construction.

Per call site, the shape chosen is the one that cannot be broken by a translation:
  * a label with no % conversions is passed through ImGui's UNFORMATTED path wherever one exists.
    This matters: a Chinese translation may contain a literal '%' (a percentage sign in prose), and passing that
    as a format string would read arguments nobody passed.
  * a label that IS a format string keeps its position (the pack is checked to keep the same
    conversions), so Tr() wraps the format itself.
  * the calls with no unformatted variant are given the translation as an ARGUMENT ('"%s"'), so
    any '%' inside it is inert.

Usage:  python tools/i18n/apply.py [--write]
"""
import argparse
import json
import pathlib
import re
import sys

SRC = pathlib.Path("src/votv-coop")
SCAN_DIRS = [SRC / "src", SRC / "include"]
CONV = re.compile(r"%[-+ #0-9.*hlLjztI]*[A-Za-z]")

# ImGui entry points whose first argument is a label, and how to wrap it.
#   unformatted -> ImGui::TextUnformatted(Tr(x))          (no format string at all)
#   fmt         -> ImGui::Foo(Tr(x), args...)             (x keeps its place as the format)
#   arg         -> ImGui::Foo("%s", Tr(x), args...)       (no unformatted variant exists)
IMGUI = {
    "Text": "unformatted",
    "TextUnformatted": "wrap",
    "TextDisabled": "arg",
    "TextWrapped": "arg",
    "BulletText": "arg",
    "SetTooltip": "arg",
    "Button": "wrap",
    "SmallButton": "wrap",
    "MenuItem": "wrap",
    "Selectable": "wrap",
    "Checkbox": "wrap",
    "RadioButton": "wrap",
    "Begin": "wrap",
    "BeginChild": "wrap",
    "BeginTabItem": "wrap",
    "CollapsingHeader": "wrap",
    "TreeNode": "wrap",
    "TreeNodeEx": "wrap",
    "Combo": "wrap",
}

# Files whose wide literals live in a namespace-scope table (static initialization). Calling the
# loader from there would read the pack before boot has settled, so those are wired by hand.
WIDE_SKIP_FILES = {"browser_input_screens.cpp"}

RUN_N = r'"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'
RUN_W = r'L"(?:[^"\\]|\\.)*"(?:\s*L?"(?:[^"\\]|\\.)*")*'


def literal_body(s):
    out = []
    for piece in re.findall(r'L?"(?:[^"\\]|\\.)*"', s):
        body = piece[1:] if piece.startswith("L") else piece
        out.append(body[1:-1])
    return "".join(out)


def unescape(body):
    out, i = [], 0
    simple = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", '"': '"', "\\": "\\", "'": "'"}
    while i < len(body):
        c = body[i]
        if c != "\\" or i + 1 >= len(body):
            out.append(c)
            i += 1
            continue
        nxt = body[i + 1]
        out.append(simple.get(nxt, nxt))
        i += 2
    return "".join(out)


def value_of(run):
    return unescape(literal_body(run))


def in_static_init(text, pos):
    """True when the literal sits in a namespace-scope initializer list.

    Such an expression is evaluated WHILE THE DLL IS LOADING -- before boot, before the config
    layer, before the pack is read. Wrapping it there is not merely useless (Tr answers English
    until boot), it is what took the whole DLL down once: a constexpr table cannot call a function
    at all (C2131), and a non-constexpr one calls into a subsystem that is not up yet.
    """
    ls = text.rfind("\n", 0, pos) + 1
    for _ in range(80):
        prev_end = text.rfind("\n", 0, ls - 1)
        if prev_end < 0:
            return False
        prev = text[prev_end + 1:ls - 1]
        # ONLY constexpr is skipped, and for a different reason than the one below: a constexpr
        # initializer cannot call a function AT ALL (C2131) -- that is a compile error, full stop.
        # A plain `const ...[] = {` table is only an ORDER-OF-EVALUATION question, and i18n.h's
        # boot gate already answers English until the pack is read, so wrapping one is safe: a
        # namespace-scope table stays English, a function-local one (evaluated on first use, after
        # boot) shows the translation. Skipping those 'to be safe' cost four real buttons their
        # translation -- safety that removes a feature is not safety.
        if re.match(r"\s*constexpr\b.*\{\s*$", prev):
            return True
        if re.match(r"\s*\};", prev):
            return False
        if re.search(r"\)\s*;\s*$", prev):
            return False
        ls = prev_end + 1
    return False


def wrap_narrow(text, keys, stats):
    for name, kind in IMGUI.items():
        pattern = re.compile(r"ImGui::" + name + r"\s*\(\s*(" + RUN_N + r")")
        pos = 0
        while True:
            m = pattern.search(text, pos)
            if not m:
                break
            pos = m.end()
            run = m.group(1)
            if value_of(run) not in keys:
                continue
            if in_static_init(text, m.start()):
                stats["skipped_static"] += 1
                continue
            has_conv = bool(CONV.search(value_of(run)))
            if kind == "unformatted" and not has_conv:
                new = "ImGui::TextUnformatted(coop::i18n::Tr(" + run + ")"
            elif kind == "arg" and not has_conv:
                new = "ImGui::" + name + "(\"%s\", coop::i18n::Tr(" + run + ")"
            else:
                new = "ImGui::" + name + "(coop::i18n::Tr(" + run + ")"
            text = text[:m.start()] + new + text[m.end():]
            pos = m.start() + len(new)
            stats["narrow"] += 1
    return text


def wrap_wide(text, keys, stats):
    pattern = re.compile(RUN_W)
    out, last, pos = [], 0, 0
    for m in pattern.finditer(text):
        run = m.group(0)
        before = text[max(0, m.start() - 60):m.start()]
        if "TrW(" in text[max(0, m.start() - 4):m.start()] or "UE_LOG" in before:
            continue
        if value_of(run) not in keys:
            continue
        if in_static_init(text, m.start()):
            stats["skipped_static"] += 1
            continue
        out.append(text[last:m.start()])
        out.append("coop::i18n::TrW(" + run + ")")
        last = m.end()
        stats["wide"] += 1
    out.append(text[last:])
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--write", action="store_true", help="rewrite the files (default: dry run)")
    ap.add_argument("--keys", default=str(SRC / "trans" / "example.json"))
    args = ap.parse_args()

    keys = set(json.loads(pathlib.Path(args.keys).read_text(encoding="utf-8")))
    stats = {"narrow": 0, "wide": 0, "files": 0, "include": 0, "skipped_static": 0}
    changed = []

    for base in SCAN_DIRS:
        for path in sorted(base.rglob("*.cpp")):
            text = path.read_text(encoding="utf-8")
            before = text
            text = wrap_narrow(text, keys, stats)
            if path.name not in WIDE_SKIP_FILES:
                text = wrap_wide(text, keys, stats)
            if text == before:
                continue
            if "#include \"coop/text/i18n.h\"" not in text:
                m = list(re.finditer(r'^#include "[^"]+"', text, re.M))
                if m:
                    at = m[-1].end() if path.suffix == ".cpp" else m[0].start()
                    text = text[:at] + "\n#include \"coop/text/i18n.h\"" + text[at:]
                    stats["include"] += 1
            changed.append(str(path))
            if args.write:
                path.write_text(text, encoding="utf-8", newline="\n")

    print(("WROTE " if args.write else "DRY RUN ") + str(stats["narrow"]) + " narrow + "
          + str(stats["wide"]) + " wide call site(s) across " + str(len(changed))
          + " file(s), " + str(stats["include"]) + " include(s) added, "
          + str(stats["skipped_static"]) + " skipped (static-init context)")
    if not args.write:
        for c in changed[:15]:
            print("  would touch " + c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
