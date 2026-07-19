#!/usr/bin/env python3
"""
sdk_diff.py -- Diff two UE4SS CXXHeaderDump trees.

Usage:
    python tools/sdk_diff.py <old_dump_dir> <new_dump_dir> [--out report.md]

What it reports:
    - Added UClasses (new in <new>).
    - Removed UClasses (gone in <new>).
    - Renamed UFunctions (same class, same param count, similar name --
      heuristic: edit distance <= max(2, len/4)).
    - Changed UProperty offsets per class (a property exists in both
      dumps under the same name but at a different offset).
    - K2Node ordinal shifts (functions whose name ends in `_K2Node_*_N`
      where the trailing digit changed but the prefix is identical).

The output feeds directly into authoring a new sdk_profile_<hash>.json
override or patching src/votv-coop/include/ue_wrap/sdk_profile.h. Each
line is annotated with the corresponding sdk_profile.h constant name
where one exists.

Designed to be re-runnable: pure-Python (no deps), reads only .hpp files,
emits Markdown to stdout (or --out file).

Adaptation workflow:
    1. After a VOTV patch, run UE4SS against the new install + dump headers.
    2. Run this script against the new dump vs the dump that the current
       sdk_profile.h targets.
    3. Pipe the output into the GitHub issue with the boot
       `multivoid-compat-report.txt`.
    4. Authoring follows: each renamed UFunction => update one constant,
       each shifted property offset => update one constant, etc.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path


# ---------------------------------------------------------------- parser

_CLASS_RE = re.compile(r"^\s*class\s+([A-Za-z_][\w]*)\s*(?::\s*public\s+[A-Za-z_][\w:<>,\s]*)?\s*\{?\s*$")
_PROP_RE = re.compile(r"^\s*([A-Za-z_][\w:<>\s\*,]*?)\s+([A-Za-z_]\w*);\s*//\s*0x([0-9A-Fa-f]+)")
_FN_RE = re.compile(r"^\s*(?:virtual\s+)?(?:static\s+)?[A-Za-z_][\w:<>\s\*,&]*\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(?:const\s*)?;?\s*$")


@dataclass
class Property:
    name: str
    offset: int
    type_str: str


@dataclass
class Function:
    name: str
    param_count: int


@dataclass
class Klass:
    name: str
    props: dict[str, Property] = field(default_factory=dict)
    fns: dict[str, Function] = field(default_factory=dict)
    source_file: str = ""


def parse_dump(root: Path) -> dict[str, Klass]:
    """Walk every .hpp under root; return {className -> Klass}."""
    classes: dict[str, Klass] = {}
    if not root.exists():
        sys.exit(f"sdk_diff: dump directory not found: {root}")
    for hpp in sorted(root.rglob("*.hpp")):
        try:
            lines = hpp.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as e:
            print(f"sdk_diff: warn: cannot read {hpp}: {e}", file=sys.stderr)
            continue
        cur: Klass | None = None
        for line in lines:
            m = _CLASS_RE.match(line)
            if m:
                name = m.group(1)
                cur = classes.setdefault(name, Klass(name=name, source_file=str(hpp.relative_to(root))))
                continue
            if cur is None:
                continue
            m = _PROP_RE.match(line)
            if m:
                t, n, off_hex = m.group(1).strip(), m.group(2), m.group(3)
                cur.props[n] = Property(name=n, offset=int(off_hex, 16), type_str=t)
                continue
            m = _FN_RE.match(line)
            if m:
                n = m.group(1)
                params = m.group(2).strip()
                pc = 0 if not params else len([p for p in params.split(",") if p.strip()])
                # Many UFunctions are declared multiple times across overrides;
                # keep the first (we'd track them all in a fuller tool).
                if n not in cur.fns:
                    cur.fns[n] = Function(name=n, param_count=pc)
    return classes


# ---------------------------------------------------------------- diffing

def edit_distance(a: str, b: str) -> int:
    """Tiny Levenshtein -- enough for "GrabComponentAtLoc" vs "GrabAtLoc"."""
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i] + [0] * len(b)
        for j, cb in enumerate(b, 1):
            cost = 0 if ca == cb else 1
            cur[j] = min(cur[j - 1] + 1, prev[j] + 1, prev[j - 1] + cost)
        prev = cur
    return prev[-1]


def strip_k2node_ordinal(name: str) -> tuple[str, str | None]:
    """`InpActEvt_use_K2Node_InputActionEvent_41` -> (`InpActEvt_use_K2Node_InputActionEvent_`, `41`).

    Returns (prefix, ordinal) where ordinal is None for non-K2Node names.
    """
    m = re.match(r"^(.*K2Node[^_]*_)(\d+)$", name)
    return (m.group(1), m.group(2)) if m else (name, None)


def diff_classes(old: dict[str, Klass], new: dict[str, Klass]) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {
        "added_classes": [],
        "removed_classes": [],
        "renamed_functions": [],
        "shifted_offsets": [],
        "k2node_renumbered": [],
        "added_functions": [],
        "removed_functions": [],
    }

    old_names = set(old.keys())
    new_names = set(new.keys())

    for n in sorted(new_names - old_names):
        out["added_classes"].append(f"+ class **{n}** (source: `{new[n].source_file}`)")
    for n in sorted(old_names - new_names):
        out["removed_classes"].append(f"- class **{n}** (was: `{old[n].source_file}`)")

    # Per-class diffs for classes present in both.
    for n in sorted(old_names & new_names):
        oc, nc = old[n], new[n]

        # Property offsets that shifted.
        for pn, op in oc.props.items():
            np_ = nc.props.get(pn)
            if np_ is None:
                continue
            if np_.offset != op.offset:
                out["shifted_offsets"].append(
                    f"~ **{n}::{pn}**: 0x{op.offset:X} -> 0x{np_.offset:X} (Δ {np_.offset - op.offset:+d})"
                )

        # K2Node ordinal shifts: same prefix, ordinal changed.
        old_k2 = {}
        for fn_name in oc.fns:
            pfx, ordinal = strip_k2node_ordinal(fn_name)
            if ordinal is not None:
                old_k2[pfx] = (fn_name, ordinal)
        for fn_name in nc.fns:
            pfx, ordinal = strip_k2node_ordinal(fn_name)
            if ordinal is not None and pfx in old_k2:
                old_name, old_ord = old_k2[pfx]
                if old_ord != ordinal and fn_name != old_name:
                    out["k2node_renumbered"].append(
                        f"~ **{n}::{old_name}** -> **{fn_name}** (K2Node ordinal {old_ord} -> {ordinal})"
                    )

        # Functions: added / removed / renamed-heuristic.
        old_fn_names = set(oc.fns.keys())
        new_fn_names = set(nc.fns.keys())
        added = new_fn_names - old_fn_names
        removed = old_fn_names - new_fn_names

        # Skip K2Node renumbered pairs from added/removed (we already reported them).
        k2_old_done = {old_name for old_name, _ in old_k2.values()}
        k2_new_done = {fn for fn in new_fn_names
                       if strip_k2node_ordinal(fn)[1] is not None and strip_k2node_ordinal(fn)[0] in old_k2}
        added -= k2_new_done
        removed -= k2_old_done

        # Heuristic rename: pair removed fn with added fn of same param count + small edit dist.
        # Track BOTH sides of the pairing so dedup in the listing loop is
        # set-membership (O(1)) and immune to false negatives from substring
        # scans (audit fix 2026-05-25: a function named e.g. `Init` would
        # incorrectly dedup against any rename line containing "InitComponent").
        matched_added: set[str] = set()
        matched_removed: set[str] = set()
        for r in sorted(removed):
            r_pc = oc.fns[r].param_count
            best: tuple[int, str] | None = None
            for a in added:
                if a in matched_added:
                    continue
                if nc.fns[a].param_count != r_pc:
                    continue
                d = edit_distance(r, a)
                threshold = max(2, len(r) // 4)
                if d <= threshold:
                    if best is None or d < best[0]:
                        best = (d, a)
            if best is not None:
                out["renamed_functions"].append(
                    f"~ **{n}::{r}** -> **{n}::{best[1]}** (edit-distance {best[0]}, params={r_pc})"
                )
                matched_added.add(best[1])
                matched_removed.add(r)

        for r in sorted(removed):
            if r in matched_removed:
                continue  # already paired in renamed_functions
            out["removed_functions"].append(f"- **{n}::{r}**")
        for a in sorted(added):
            if a in matched_added:
                continue
            out["added_functions"].append(f"+ **{n}::{a}**")

    return out


def render(diff: dict[str, list[str]]) -> str:
    sections = [
        ("Removed classes (HIGH IMPACT -- sdk_profile.h constant needs replacing)", "removed_classes"),
        ("Added classes (informational; new content)", "added_classes"),
        ("Shifted property offsets (HIGH IMPACT -- hardcoded `+ 0xNNN` in code may break)", "shifted_offsets"),
        ("K2Node ordinal renumbered (UFunction name constants in sdk_profile.h need updating)", "k2node_renumbered"),
        ("Likely renamed functions (heuristic match -- verify each)", "renamed_functions"),
        ("Removed functions (a UFunction lookup will return null)", "removed_functions"),
        ("Added functions (informational)", "added_functions"),
    ]
    lines = ["# SDK diff report", ""]
    for title, key in sections:
        entries = diff[key]
        lines.append(f"## {title} ({len(entries)})")
        if entries:
            lines.extend(entries)
        else:
            lines.append("_(none)_")
        lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("old", type=Path, help="path to the OLD CXXHeaderDump directory")
    ap.add_argument("new", type=Path, help="path to the NEW CXXHeaderDump directory")
    ap.add_argument("--out", type=Path, default=None, help="write report to this file (default: stdout)")
    args = ap.parse_args()

    print(f"sdk_diff: parsing OLD dump at {args.old}", file=sys.stderr)
    old = parse_dump(args.old)
    print(f"sdk_diff: parsed {len(old)} classes from OLD", file=sys.stderr)

    print(f"sdk_diff: parsing NEW dump at {args.new}", file=sys.stderr)
    new = parse_dump(args.new)
    print(f"sdk_diff: parsed {len(new)} classes from NEW", file=sys.stderr)

    diff = diff_classes(old, new)
    report = render(diff)

    if args.out:
        args.out.write_text(report, encoding="utf-8")
        print(f"sdk_diff: wrote report to {args.out}", file=sys.stderr)
    else:
        print(report)

    # Exit code reflects "anything meaningful changed?"
    impact = (
        len(diff["removed_classes"])
        + len(diff["shifted_offsets"])
        + len(diff["k2node_renumbered"])
        + len(diff["removed_functions"])
        + len(diff["renamed_functions"])
    )
    return 0 if impact == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
