#!/usr/bin/env python3
"""bp_cfg.py -- VOTV Blueprint CONTROL-FLOW GRAPHS (visualize cooked kismet bytecode).

WHY: bp_reflect.py gives the bytecode as structured JSON -- complete, but every
control-flow question ("what jumps here?", "which branch reaches the SpawnActor?")
is answered by hand-walking EX_* offsets. kismet-analyzer has carried a CFG half
(`gen-cfg`) since the day we pinned it, and for three months nobody switched it on:
docs cite `to-json` only. Born 2026-09-02 from the UE-Modding-Tools survey (catalog
pointed at by Moddy in public); validated same day against measured facts before
adoption: ATV playerSit -> ubergraph @9122 = bare EX_PopExecutionFlow (docs/vehicles/
ATV.md section 15's dead-stub, found on the graph in one read) and the
PhysicsConstraintComponent->BreakConstraint call rows.

WHAT YOU GET, per Blueprint:
  research/bp_reflection/cfg/<name>/<name>.txt   per-function basic blocks + successor
                                                 edges (greppable; the fast answer)
  research/bp_reflection/cfg/<name>/<name>.dot   the whole asset as one graphviz digraph
  with --fn <Function>:  <Function>.dot + .svg   that function's subgraph, rendered
                                                 (cross-function jump targets kept as stubs)

PIPELINE: repak (unpack) -> kismet-analyzer `gen-cfg` -> slice per function ->
graphviz `dot -Tsvg` (portable Graphviz auto-downloaded, SHA-pinned, like the other
two tools). All three land in research/pak_re/tools/.

RULES: dev/RE tool ONLY -- nothing here ships (RULE 3). Read-only on game assets
(RULE 1). Outputs live under research/ (gitignored: copyrighted game content in
derived form). Findings distilled from these graphs go to research/findings/ as usual.

Usage:
  python tools/bp_cfg.py ATV                      # build CFG, list functions
  python tools/bp_cfg.py ATV --fn ejectWheel      # + slice & render one function
  python tools/bp_cfg.py ui_damageIndicator --fn Tick
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bp_reflect as br  # noqa: E402  (shares PAK paths, pinned-download machinery)

CFG_OUT = os.path.join(br.ROOT, "research", "bp_reflection", "cfg")

GV_URL = ("https://gitlab.com/api/v4/projects/4207231/packages/generic/"
          "graphviz-releases/15.1.1/windows_10_cmake_Release_Graphviz-15.1.1-win64.zip")
# Verified 2026-09-02 against upstream's own .sha256 sibling file at the same URL.
br.TOOL_SHA256[GV_URL] = "e8256ef077e601d9f284378d96cd17faa7910832cf6bb85c43005e66ec2f255e"


def ensure_graphviz():
    """dot.exe, portable install under research/pak_re/tools/graphviz (SHA-pinned)."""
    root = os.path.join(br.TOOLS, "graphviz")
    dot = br._find(os.path.join(root, "**", "dot.exe"))
    if not dot:
        print("  downloading graphviz (portable) ...")
        os.makedirs(root, exist_ok=True)
        z = os.path.join(br.TOOLS, "graphviz.zip")
        br._fetch_verified(GV_URL, z)
        br._safe_extract(z, root)
        dot = br._find(os.path.join(root, "**", "dot.exe"))
    return dot  # None is survivable: .dot/.txt still land, only .svg is skipped


def ensure_extracted(repak, asset):
    """Same extraction bp_reflect.disassemble does: unpack the containing dir once."""
    ua = os.path.join(br.EXTRACT, asset.replace("/", os.sep))
    if not os.path.exists(ua):
        d = "/".join(asset.split("/")[:-1])
        subprocess.run([repak, "unpack", "-o", br.EXTRACT, "-i", d, br.PAK],
                       check=True, stdout=subprocess.DEVNULL)
    return ua if os.path.exists(ua) else None


def gen_cfg(ka, ua, name):
    outdir = os.path.join(CFG_OUT, name)
    os.makedirs(outdir, exist_ok=True)
    subprocess.run([ka, "gen-cfg", ua, outdir], check=True)
    stem = os.path.splitext(os.path.basename(ua))[0]
    return (os.path.join(outdir, stem + ".dot"), os.path.join(outdir, stem + ".txt"))


def function_index(txt_path):
    """FunctionExport name -> block count, in file order."""
    idx, cur = [], None
    for line in open(txt_path, encoding="utf-8", errors="replace"):
        if line.startswith("FunctionExport "):
            cur = [line.split(None, 1)[1].strip(), 0]
            idx.append(cur)
        elif cur and line.startswith("=== Block @"):
            cur[1] += 1
    return idx

_NODE = re.compile(r'^"([^"]+)" \[label')
_EDGE = re.compile(r'^"([^"]+?)(?::[ns ew]+)?" -> "([^"]+?)(?::[nsew]+)?"(.*)$')

# Upstream emission bug (kismet-analyzer e8982e9): an EMPTY string operand (e.g. an
# EX_TextConst whose LocalizedNamespace is "") becomes <FONT COLOR="#...."></FONT>,
# and graphviz's HTML-label grammar rejects an empty element ("syntax error in line 1").
# Measured 2026-09-02 on ATV.uasset node ejectWheel__block_898 -- the ORIGINAL
# gen-cfg .dot fails to render, not just our slice. An entity keeps it visually empty.
_EMPTY_FONT = re.compile(r'(<FONT COLOR="#[0-9A-Fa-f]{6}">)(</FONT>)')


def _fix_empty_font(line):
    return _EMPTY_FONT.sub(r"\1&nbsp;\2", line)


def slice_function(dot_path, fn, out_dot):
    """Keep fn's entry node + its __block_* nodes + edges leaving them; foreign jump
    targets (e.g. the ubergraph entry an event stub calls into) stay as dashed stubs."""
    mine = lambda n: n == fn or n.startswith(fn + "__block_")
    nodes, edges, stubs = [], [], []
    for raw in open(dot_path, encoding="utf-8", errors="replace"):
        line = raw.rstrip("\n")
        m = _NODE.match(line)
        if m:
            if mine(m.group(1)):
                nodes.append(_fix_empty_font(line))
            continue
        m = _EDGE.match(line)
        if m and mine(m.group(1)):
            edges.append(line)
            tgt = m.group(2)
            if not mine(tgt):
                stubs.append(f'"{tgt}" [label = "{tgt}"; shape = "box"; '
                             f'style = "dashed"; color = "purple"]')
    if not nodes:
        return 0, 0
    with open(out_dot, "w", encoding="utf-8") as f:
        f.write('digraph\n{\ngraph [fontname = "monospace"]\n'
                'node [fontname = "monospace"]\nedge [fontname = "monospace"]\n')
        f.write("\n".join(nodes + sorted(set(stubs)) + edges))
        f.write("\n}\n")
    return len(nodes), len(edges)


def slice_txt(txt_path, fn, out_txt):
    keep, on = [], False
    for line in open(txt_path, encoding="utf-8", errors="replace"):
        if line.startswith("FunctionExport "):
            on = line.split(None, 1)[1].strip() == fn
        if on:
            keep.append(line)
    if keep:
        open(out_txt, "w", encoding="utf-8").writelines(keep)
    return bool(keep)


def main():
    args = sys.argv[1:]
    fn = None
    if "--fn" in args:
        i = args.index("--fn")
        fn = args[i + 1]
        del args[i:i + 2]
    names = [a for a in args if not a.startswith("--")]
    if not names:
        print(__doc__)
        return
    if not os.path.exists(br.PAK):
        sys.exit(f"FATAL: pak not found at {br.PAK}")
    repak, ka = br.ensure_tools()
    dot_exe = ensure_graphviz()
    entries = br.pak_list(repak)
    for name in names:
        asset = br.find_asset(entries, name)
        if not asset:
            print(f"[{name}] NOT FOUND in pak (try bp_reflect.py --list)")
            continue
        print(f"[{name}] {asset}")
        ua = ensure_extracted(repak, asset)
        if not ua:
            print("  extract FAILED")
            continue
        dot_path, txt_path = gen_cfg(ka, ua, name)
        idx = function_index(txt_path)
        print(f"  -> {os.path.relpath(txt_path, br.ROOT)}  ({len(idx)} functions)")
        if fn is None:
            for f_name, blocks in idx:
                print(f"     {blocks:4d} blocks  {f_name}")
            continue
        outdir = os.path.dirname(dot_path)
        f_dot = os.path.join(outdir, fn + ".dot")
        n, e = slice_function(dot_path, fn, f_dot)
        if not n:
            print(f"  --fn {fn}: no such FunctionExport; this asset has:")
            for f_name, blocks in idx:
                print(f"     {blocks:4d} blocks  {f_name}")
            continue
        slice_txt(txt_path, fn, os.path.join(outdir, fn + ".txt"))
        print(f"  -> {os.path.relpath(f_dot, br.ROOT)}  ({n} blocks, {e} edges)")
        if dot_exe:
            f_svg = os.path.join(outdir, fn + ".svg")
            subprocess.run([dot_exe, "-Tsvg", f_dot, "-o", f_svg], check=True)
            print(f"  -> {os.path.relpath(f_svg, br.ROOT)}")
        else:
            print("  (graphviz unavailable -- .svg skipped, .dot is complete)")


if __name__ == "__main__":
    main()
