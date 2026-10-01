#!/usr/bin/env python3
"""tools/i18n/extract.py -- mint trans/example.json from the mod's own UI strings.

Every label the mod draws is written in English at its call site, so the English text IS the
translation key (see coop/text/i18n.h). That makes this script the one owner of the list a
translator is handed: the inventory is read out of the source, so it cannot drift from what the
code asks for.

  (no flags)   write src/votv-coop/trans/example.json -- every key mapped to its own English text
  --check      read that file and fail when it and the source disagree (a stale key nothing asks
               for, or a UI literal the template is missing). Cheap enough for CI.

Run from the repository root:  python tools/i18n/extract.py
"""
import argparse
import json
import pathlib
import re
import sys

SRC = pathlib.Path("src/votv-coop")
SCAN_DIRS = [SRC / "src"]
UI_DIRS = ["src/votv-coop/src/ui/", "src/votv-coop/include/ui/"]

STR = r'"([^"\\]|\\.)*"'
WSTR = r'L"([^"\\]|\\.)*"'

# Every ImGui entry point whose text a player reads, with the group index of the label literal.
# The transform's own output is the best evidence of what the game LOOKS UP: every wired string
# appears as Tr("...") or TrW(L"..."), including the ones composed in code that no ImGui pattern
# can see ("Select a server", " servers", "s ago"). Extracting those is what keeps the template
# from silently losing a hand-added key on the next regeneration.
WIRED = [
    (r"coop::i18n::TrW?\s*\(\s*(" + STR + r")", 1),
]

IMGUI = [
    (r"ImGui::Text\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::TextUnformatted\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::TextDisabled\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::TextWrapped\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::BulletText\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::TextColored\s*\([^,]+,\s*(" + STR + r")", 1),
    (r"ImGui::Button\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::SmallButton\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::MenuItem\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::Selectable\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::Checkbox\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::RadioButton\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::SetTooltip\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::Begin\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::BeginChild\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::BeginTabItem\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::CollapsingHeader\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::TreeNodeEx?\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::Combo\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::InputText\w*\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::Slider\w+\s*\(\s*(" + STR + r")", 1),
    (r"ImGui::LabelText\s*\(\s*(" + STR + r")\s*,\s*(" + STR + r")", 0),
]

# The native Slate screens are labelled with wide literals, collected by SHAPE rather than by call
# site because they pass text through a dozen small helpers. That also collects the non-text wide
# literals every screen uses, so those are named here rather than guessed at: add to this list,
# never remove from it silently.
WIDE_EXCLUDE = {
    "Widget", "HorizontalBox", "VerticalBox", "SizeBox", "ScaleBox", "Overlay", "CanvasPanel",
    "Border", "Button", "TextBlock", "Image", "ScrollBox", "WidgetSwitcher", "Spacer",
    "HorizontalBoxSlot", "VerticalBoxSlot", "OverlaySlot", "SizeBoxSlot", "PanelSlot",
    "RichTextBlock", "EditableText", "EditableTextBox", "ProgressBar", "CheckBox", "ComboBox",
    "ListView", "Throbber", "CircularThrobber", "BackgroundBlur", "MenuAnchor", "WrapBox",
    "WidgetTree",
}
TOKEN_LIKE = re.compile(r"^[A-Za-z0-9_.:/-]+$")


CONV_RE = re.compile(r"%[-+ #0-9.*hlLjztI]*[A-Za-z]")


def is_translatable(value):
    """Is this a line a player READS, or a fragment the compiler glues to its neighbours?

    Dropped, with the reason each rule needs to exist: a doc marker ("###coop_boot_warning"), a
    value that is only conversions once they are stripped ("%d", "%-7s"), a joiner that carries
    its own padding (\" and \"), and a run that begins with spaces -- real labels do not, and the
    fragments of a description split across lines always do."""
    if value.lstrip().startswith("#"):
        return False
    if value[:1].isspace():
        return False
    if any(t in value for t in ("::", "{", "}", ";", "->")):
        return False   # a fragment of the C++ around a literal, not a string anyone reads
    stripped = CONV_RE.sub("", value)
    if len(re.findall(r"[A-Za-z]{2,}", stripped)) == 0:
        return False
    return len(value.strip()) >= 4


def literal_body(literal):
    """The bytes between the quotes of one C++ literal, its L prefix dropped.

    collect_run returns the CONCATENATION of these, so each piece must arrive bare: leaving the
    quotes on the later pieces is what put a pair of stray quotes in the middle of every joined
    key -- a key no call site can ever match, since the runtime string has no quotes there."""
    body = literal[1:] if literal.startswith("L") else literal
    return body[1:-1]


def unescape(body):
    """The RUNTIME value of a C++ literal BODY (already bare, no quotes)."""
    out = []
    i = 0
    simple = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", '"': '"', "\\": "\\", "'": "'"}
    while i < len(body):
        c = body[i]
        if c != "\\" or i + 1 >= len(body):
            out.append(c)
            i += 1
            continue
        nxt = body[i + 1]
        if nxt in simple:
            out.append(simple[nxt])
        else:
            out.append(nxt)
        i += 2
    return "".join(out)


def collect_run(text, start, wide):
    """The full literal at start, with adjacent literals joined -- the codebase wraps its long
    sentences that way, and the runtime sees one string."""
    pat = WSTR if wide else STR
    m = re.match(pat, text[start:])
    if not m:
        return "", start
    parts = [literal_body(m.group(0))]
    pos = start + m.end()
    while True:
        n = re.match(r"\s*(" + pat + r")", text[pos:])
        if not n:
            break
        parts.append(literal_body(n.group(1)))
        pos += n.end()
    return "".join(parts), pos


def scan_file(path):
    text = path.read_text(encoding="utf-8")
    found = set()
    for pattern, group in WIRED + IMGUI:
        for m in re.finditer(pattern, text):
            for g in ((1, 2) if group == 0 else (group,)):
                run, _ = collect_run(text, m.start(g), False)
                value = unescape(run)
                if is_translatable(value):
                    found.add(value)
    rel = str(path).replace("\\", "/")
    if any(rel.startswith(d) for d in UI_DIRS):
        for m in re.finditer(WSTR, text):
            run, _ = collect_run(text, m.start(), True)
            value = unescape(run)
            if value in WIDE_EXCLUDE or not is_translatable(value):
                continue
            # A native label is a sentence or a capitalised word. What the screens ALSO pass as
            # wide literals is their own furniture -- button_back, scrollboxRoot, image_border,
            # switcher_widgets -- and that is lowercase with no space, so this one line is what
            # keeps a translator from being handed ImGui's plumbing.
            if " " not in value and not (value[:1].isupper() and value.isalpha()):
                continue
            if TOKEN_LIKE.match(value) and len(value) <= 3:
                continue
            found.add(value)
    return found


def scan_all():
    out = {}
    for base in SCAN_DIRS:
        for path in sorted(base.rglob("*.cpp")) + sorted(base.rglob("*.h")):
            try:
                keys = scan_file(path)
            except (UnicodeDecodeError, OSError):
                continue
            for k in keys:
                out.setdefault(k, []).append(str(path).replace("\\", "/"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="fail when the template and source disagree")
    ap.add_argument("--out", default=str(SRC / "trans" / "example.json"))
    args = ap.parse_args()

    found = scan_all()
    out_path = pathlib.Path(args.out)

    if not args.check:
        payload = {k: k for k in sorted(found)}
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with out_path.open("w", encoding="utf-8", newline="\n") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2, sort_keys=False)
            f.write("\n")
        print("wrote " + str(out_path) + " -- " + str(len(payload)) + " key(s)")
        return 0

    if not out_path.is_file():
        print("FAIL: " + str(out_path) + " is missing -- run without --check first")
        return 1
    known = json.loads(out_path.read_text(encoding="utf-8"))
    stale = sorted(set(known) - set(found))
    missing = sorted(set(found) - set(known))
    for k in stale[:20]:
        print("FAIL stale key (nothing in the source asks for it): " + repr(k))
    for k in missing[:20]:
        print("FAIL missing key (in the source, absent from the template): " + repr(k) + " -- " + found[k][0])
    if stale or missing:
        print("FAIL: " + str(len(stale)) + " stale, " + str(len(missing)) + " missing")
        return 1
    print("OK: " + str(len(known)) + " key(s) match the source")
    return 0


if __name__ == "__main__":
    sys.exit(main())
