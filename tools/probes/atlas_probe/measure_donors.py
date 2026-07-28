# M5 of the arc-D gate -- donor measurement.
#
# Answers "which hanzi set counts as common, and what does it actually cost" with
# bytes rather than estimates: subsets each candidate donor to the codepoint set
# named, saves it, and measures the result. The candidate sets are Dear ImGui's
# OWN vendored ranges (parsed out of imgui_draw.cpp), so the answer is a defined
# list rather than a preference.
#
#   pip install fonttools
#   python tools/probes/atlas_probe/measure_donors.py <donor-dir>
#
# The donor dir must hold, downloaded by hand (they are release assets, not repo
# files -- decision D-f):
#   NotoSansSC-VF.ttf     https://github.com/google/fonts  ofl/notosanssc   (OFL 1.1)
#   NotoSansJP-VF.ttf     https://github.com/google/fonts  ofl/notosansjp   (OFL 1.1)
#   Twemoji.Mozilla.ttf   https://github.com/mozilla/twemoji-colr  v0.7.0   (MIT + CC-BY 4.0)
#
# Results as of 2026-07-28 are recorded in
# research/findings/join-identity/votv-arc-d-gate-measurements-2026-07-28.md
import io
import json
import os
import re
import sys

from fontTools.ttLib import TTFont
from fontTools import subset
from fontTools.varLib import instancer

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
IMGUI_DRAW = os.path.join(REPO, "src", "votv-coop", "third_party", "imgui", "imgui_draw.cpp")


def parse_imgui_sets():
    """Extract GetGlyphRangesChineseSimplifiedCommon + GetGlyphRangesJapanese as codepoints."""
    src = open(IMGUI_DRAW, "r", encoding="utf-8", errors="replace").read()
    out = {}
    for fname, key in (("GetGlyphRangesChineseSimplifiedCommon", "cn_common"),
                       ("GetGlyphRangesJapanese", "jp")):
        i = src.index("ImFontAtlas::" + fname)
        body = src[i:src.index("return &full_ranges[0];", i)]
        offs = [int(x) for x in re.findall(
            r"-?\d+", re.search(r"accumulative_offsets_from_0x4E00\[\]\s*=\s*\{(.*?)\};",
                                body, re.S).group(1))]
        cps, base = [], 0x4E00
        for o in offs:
            base += o
            cps.append(base)
        nums = [int(x, 16) for x in re.findall(
            r"0x([0-9A-Fa-f]+)", re.search(r"base_ranges\[\]\s*=.*?\{(.*?)\};", body, re.S).group(1))]
        base_cps = []
        for a, b in zip(nums[0::2], nums[1::2]):
            base_cps.extend(range(a, b + 1))
        out[key] = {"ideographs": cps, "base": base_cps}
    return out


def subset_bytes(src_path, codepoints, tag, wght=400):
    f = TTFont(src_path, lazy=False)
    if "fvar" in f:
        f = instancer.instantiateVariableFont(f, {"wght": wght}, inplace=False,
                                              updateFontNames=False)
    have = set(f.getBestCmap().keys())
    want = sorted(set(codepoints) & have)
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.drop_tables = ["DSIG"]
    s = subset.Subsetter(options=opts)
    s.populate(unicodes=want)
    s.subset(f)
    buf = io.BytesIO()
    f.save(buf)
    return {"tag": tag, "requested": len(set(codepoints)), "kept": len(want),
            "missing_in_donor": len(set(codepoints)) - len(want), "bytes": len(buf.getvalue())}


def probe(path, label):
    """Colour-format probe: FT_DISABLE_PNG=ON means the emoji donor MUST be COLR/CPAL."""
    f = TTFont(path, lazy=True, fontNumber=0)
    cm = f.getBestCmap()
    return {"label": label, "file": os.path.basename(path), "bytes": os.path.getsize(path),
            "COLR": ("COLR" in f) and getattr(f["COLR"], "version", "?"), "CPAL": "CPAL" in f,
            "CBDT": "CBDT" in f, "sbix": "sbix" in f, "SVG": "SVG " in f,
            "cmap_cp": len(cm), "astral_cp": len([k for k in cm if k > 0xFFFF])}


def main():
    if len(sys.argv) < 2:
        print("usage: measure_donors.py <donor-dir>")
        return 2
    d = sys.argv[1]
    sets = parse_imgui_sets()
    cn, jp = sets["cn_common"], sets["jp"]
    cn_all = set(cn["ideographs"]) | set(cn["base"])
    jp_all = set(jp["ideographs"]) | set(jp["base"])
    print("=== ImGui's own vendored CJK sets ===")
    print(f"  ChineseSimplifiedCommon : {len(cn['ideographs'])} + {len(cn['base'])} = {len(cn_all)}")
    print(f"  Japanese                : {len(jp['ideographs'])} + {len(jp['base'])} = {len(jp_all)}")
    print(f"  UNION                   : {len(cn_all | jp_all)}")

    sc = os.path.join(d, "NotoSansSC-VF.ttf")
    jpf = os.path.join(d, "NotoSansJP-VF.ttf")
    tw = os.path.join(d, "Twemoji.Mozilla.ttf")

    print("\n=== donor probes ===")
    for p, lab in ((sc, "Noto Sans SC (OFL 1.1)"), (jpf, "Noto Sans JP (OFL 1.1)"),
                   (tw, "Twemoji Mozilla (MIT + CC-BY 4.0)")):
        print(" ", json.dumps(probe(p, lab)))

    print("\n=== MEASURED subset bytes (wght=400 instance) ===")
    tw_cm = set(TTFont(tw, lazy=True).getBestCmap().keys())
    emoji_single = {c for c in tw_cm
                    if c >= 0x1F000 or (0x2600 <= c <= 0x27BF) or (0x2190 <= c <= 0x21FF)}
    rows = [
        subset_bytes(sc, cn_all, "NotoSansSC / ImGui ChineseSimplifiedCommon"),
        subset_bytes(jpf, jp_all, "NotoSansJP / ImGui Japanese"),
        subset_bytes(sc, cn_all | jp_all, "NotoSansSC / UNION(CNcommon+JP)"),
        subset_bytes(jpf, set(range(0x3000, 0x3100)), "NotoSansJP / kana + CJK punctuation only"),
        subset_bytes(tw, emoji_single, "Twemoji / single-codepoint emoji"),
    ]
    for r in rows:
        print("  {tag:<46} kept={kept:<5} missing={missing_in_donor:<5} {bytes:>10,} bytes".format(**r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
