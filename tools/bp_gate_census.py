#!/usr/bin/env python3
"""bp_gate_census.py -- is THIS Blueprint verb gateable by an in-memory bytecode prologue?

WHY: the reserved gate seam (dispatch-ladder tier 4) would prepend a prologue --
`EX_JumpIfNot` + a call to our own engine-resident marker -- to a target SCRIPT function's
in-memory bytecode, buying observe + full args + CANCEL on EVERY dispatch route, including
the `EX_Local*` routes that are invisible to both our ProcessEvent detour and the Func seam.

Its entry gate is a census, and the census asks exactly three questions per verb:

  1. Does the verb HAVE a real body, or is it an ubergraph stub?  A stub's body is a call to
     `ExecuteUbergraph_<BP>` with a CONSTANT entry offset; the ubergraph re-enters via
     `EX_ComputedJump` off a runtime value, so a point INSIDE an ubergraph is not reachable.
     `[V]` measured here 2026-09-04: all 274 ubergraphs in the corpus carry exactly one
     EX_ComputedJump, no exceptions.  A verb's ENTRY is still reachable -- see verdict().
  2. How many statements does the body carry?  (Size of the thing we are patching.)
  3. How many ABSOLUTE code offsets does it carry?  Every one of them must be rewritten by
     +len(prologue) when bytes are prepended.  Zero offsets == the mechanism is exercised
     alone, which is what makes a verb a good FIRST DRILL target rather than a good first
     VALUE target.

THE RELOCATION FAMILY IS MEASURED HERE, NOT ASSUMED (2026-09-04, over 307 dumps).  Six int
fields carry an ABSOLUTE address into the function's own Script array -- their values reach
118,790 in the largest ubergraph:

    EX_Jump.CodeOffset                 0x06     3,813 sites
    EX_JumpIfNot.CodeOffset            0x07     4,393
    EX_PushExecutionFlow.PushingAddress 0x4C    3,611
    EX_SkipOffsetConst.Value           0x5B       601
    EX_SwitchValue.EndGotoOffset       0x69       749
    FKismetSwitchCase.NextOffset       (inside 0x69)  2,737   <-- see below

...and TWO look-alikes are RELATIVE skip sizes that must NOT be touched:

    EX_Context.Offset                  0x19    21,018 sites, min 9 / median 16 / max 3,777
    EX_ClassContext.Offset             0x12        13 sites, all 9

The discrimination is measured, not asserted: EX_Context appears throughout the same large
ubergraphs where EX_Jump reaches 118,790, and never once exceeds 3,777.  It is the size of the
context expression it skips over, so it is position-independent.  Relocating those 21,031
sites would corrupt every one of them.

WHY `FKismetSwitchCase` GETS ITS OWN ROW: the charted opcode family {0x06,0x07,0x4C,0x5B,0x69}
is correct as a list of OPCODES and misleading as a list of OFFSETS.  A switch carries one
`EndGotoOffset` on the EX_SwitchValue node plus one `NextOffset` per case -- corpus-wide, 749
switches carry 3,486 offsets between them (4.7 each).  An implementer who relocates "the five
opcodes" by walking top-level nodes only will miss 2,737 offsets that live one level down.

RULES: dev/RE tool ONLY -- nothing here ships (RULE 3).  Read-only on game assets (RULE 1);
it reads the JSON that bp_reflect.py already produced and never touches the pak.

Usage:
  python tools/bp_gate_census.py serverBox                     # every function in the BP
  python tools/bp_gate_census.py ui_playerInventory --fn em_   # prefix filter
  python tools/bp_gate_census.py laptop --fn startup --verbose  # + the offsets themselves
  python tools/bp_gate_census.py --markdown serverBox ui_playerInventory
"""
import argparse
import glob
import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUMPS = os.path.join(ROOT, "research", "bp_reflection")

# field -> opcode, for the report. Measured 2026-09-04; see the module docstring.
ABSOLUTE = {
    ("EX_Jump", "CodeOffset"): "0x06",
    ("EX_JumpIfNot", "CodeOffset"): "0x07",
    ("EX_PushExecutionFlow", "PushingAddress"): "0x4C",
    ("EX_SkipOffsetConst", "Value"): "0x5B",
    ("EX_SwitchValue", "EndGotoOffset"): "0x69",
    ("FKismetSwitchCase", "NextOffset"): "0x69 case",
}
# Relative skip sizes that LOOK like offsets. Counted so a verb's report can say
# "and N look-alikes you must leave alone" rather than leaving it to memory.
RELATIVE = {
    ("EX_Context", "Offset"),
    ("EX_ClassContext", "Offset"),
}


def short_type(node):
    t = node.get("$type", "") if isinstance(node, dict) else ""
    return t.split(".")[-1].split(",")[0] if t else ""


def walk(node, fn):
    """Depth-first over the decoded bytecode tree, calling fn(dict) on every node."""
    if isinstance(node, dict):
        fn(node)
        for v in node.values():
            walk(v, fn)
    elif isinstance(node, list):
        for v in node:
            walk(v, fn)


def scan_body(body):
    """-> (absolute_offsets, relative_offsets, type_histogram)."""
    absolute, relative, hist = [], [], {}

    def visit(n):
        t = short_type(n)
        if t:
            hist[t] = hist.get(t, 0) + 1
        for k, v in n.items():
            if not isinstance(v, int) or k == "$type":
                continue
            if (t, k) in ABSOLUTE:
                absolute.append((t, k, v, ABSOLUTE[(t, k)]))
            elif (t, k) in RELATIVE:
                relative.append((t, k, v))

    walk(body, visit)
    return absolute, relative, hist


def classify(name, body, by_index):
    """Is this a real gateable body, an ubergraph stub, the ubergraph, or a signature?

    An ubergraph STUB is the compiler's event entry. Its shape is
        N x EX_LetValueOnPersistentFrame   (marshal this event's params into the
                                            ubergraph's persistent frame)
        1 x EX_LocalFinalFunction -> ExecuteUbergraph_<BP>(<const entry offset>)
        EX_Return / EX_EndOfScript
    A stub carries no logic of its own -- the logic lives in the shared ubergraph -- but
    CANCELLING it still cancels the event, because the cancel jumps past the ExecuteUbergraph
    call.  See verdict() for the measurement that makes that sound.  What no patch reaches is
    a point INSIDE the ubergraph: every stub embeds its entry offset as a constant and the
    ubergraph is re-entered by EX_ComputedJump off a runtime value.

    CLASSIFY BY THE CALLEE, NOT BY STATEMENT COUNT.  Counting "one meaningful statement"
    reads `serverBox::insertFloppy` as a real body, because its param marshalling is a
    second statement -- it is a stub whose event takes an argument.  That misclassification
    put two wrong rows into the census table on 2026-09-04, both of them saying
    "yes, real body" about a verb whose entire logic is in the ubergraph.  The callee is
    the fact; the statement count is a symptom.
    """
    # A delegate signature / empty event still serialises an EX_Return + EX_EndOfScript, so
    # "no body" is about MEANINGFUL statements, not about the list being empty. Without this a
    # signature reports "gateable, NIL relocation" -- true and useless, and it pads the census.
    meaningful = [x for x in body
                  if short_type(x) not in ("EX_Return", "EX_Nothing", "EX_EndOfScript")]
    if not meaningful:
        return "no body (signature/delegate)", None
    if name.startswith("ExecuteUbergraph"):
        return "THE UBERGRAPH (not gateable: EX_ComputedJump re-entry)", None

    uber_entry = []

    def visit(n):
        if short_type(n) in ("EX_LocalFinalFunction", "EX_FinalFunction"):
            nm = by_index.get(n.get("StackNode"))
            if nm and nm.startswith("ExecuteUbergraph"):
                for p in n.get("Parameters") or []:
                    if short_type(p) == "EX_IntConst":
                        uber_entry.append(p.get("Value"))

    walk(body, visit)
    if not uber_entry:
        return "real body", None

    # It enters the ubergraph. Does it ALSO carry logic of its own? Anything at top level
    # that is not param marshalling, the ubergraph call, or the epilogue.
    skip = ("EX_Return", "EX_Nothing", "EX_EndOfScript", "EX_LetValueOnPersistentFrame")
    own = [s for s in body
           if short_type(s) not in skip
           and not (short_type(s) in ("EX_LocalFinalFunction", "EX_FinalFunction")
                    and (by_index.get(s.get("StackNode")) or "").startswith("ExecuteUbergraph"))]
    if own:
        return ("HYBRID: %d own statement(s) + ubergraph entry" % len(own)), uber_entry[0]
    return "ubergraph stub (logic is in the shared ubergraph)", uber_entry[0]


def load(bp):
    path = bp if os.path.isfile(bp) else os.path.join(DUMPS, bp + ".json")
    if not os.path.isfile(path):
        cand = glob.glob(os.path.join(DUMPS, "*" + bp + "*.json"))
        if len(cand) == 1:
            path = cand[0]
        else:
            return None, path, cand
    return json.load(io.open(path, encoding="utf-8")), path, []


def census(bp, fn_filter=None, verbose=False):
    d, path, cand = load(bp)
    if d is None:
        print("bp_gate_census: no dump for %r" % bp)
        if cand:
            print("  did you mean: %s" % ", ".join(os.path.basename(c)[:-5] for c in cand[:8]))
        else:
            print("  run:  python tools/bp_reflect.py %s" % bp)
        return None

    exports = d.get("Exports", [])
    # UAsset export indices are 1-based in StackNode references.
    by_index = {i + 1: e.get("ObjectName") for i, e in enumerate(exports)}

    rows = []
    for e in exports:
        if "FunctionExport" not in str(e.get("$type", "")):
            continue
        name = e.get("ObjectName") or "?"
        if fn_filter and not any(name.lower().startswith(f.lower()) or f.lower() == name.lower()
                                 for f in fn_filter):
            continue
        body = e.get("ScriptBytecode") or []
        kind, uber_entry = classify(name, body, by_index)
        absolute, relative, hist = scan_body(body)
        rows.append({
            "bp": os.path.basename(path)[:-5],
            "fn": name,
            "kind": kind,
            "uber": uber_entry,
            "statements": len(body),
            "absolute": absolute,
            "relative": relative,
            "hist": hist,
        })
    return rows


def verdict(row):
    """Gateable? -- and at what relocation cost.

    A STUB IS GATEABLE, and calling it "entry only" understates it (corrected 2026-09-04).
    Cancel means jumping past the body to its EX_Return; for a stub that skips the
    ExecuteUbergraph call, so the whole event does not run.  `[V]` measured over all 4,458
    stubs in the 307-dump corpus: NOTHING meaningful follows the ubergraph call in a single
    one of them, and every statement before it is EX_LetValueOnPersistentFrame param
    marshalling.  The args are the stub's own parameters and are in the frame at entry.

    What stays out of reach is a point INSIDE the ubergraph: its entry offsets are embedded
    as constants in all 4,458 stubs and it is re-entered by EX_ComputedJump, so prepending
    bytes to the ubergraph itself invalidates every one of them.  That is the real content
    of "ubergraph bodies are not gateable" -- per-EVENT gating is available, per-BRANCH is
    not.  The stub's `EX_IntConst <entry>` needs no relocation when the STUB is patched: it
    addresses the ubergraph's script, not this function's.
    """
    n = len(row["absolute"])
    cost = "NIL" if n == 0 else "%d offset%s" % (n, "" if n == 1 else "s")
    if row["kind"].startswith("real body"):
        return "YES", cost
    if row["kind"].startswith("ubergraph stub"):
        return "YES (event)", "%s; cancels ubergraph @%s whole" % (cost, row["uber"])
    if row["kind"].startswith("HYBRID"):
        return "YES (event)", "%s; own stmts + ubergraph @%s" % (cost, row["uber"])
    if row["kind"].startswith("THE UBERGRAPH"):
        return "NO", "EX_ComputedJump re-entry; entry offsets const in every stub"
    return "N/A", "no body"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bp", nargs="+", help="Blueprint name(s), as dumped by bp_reflect.py")
    ap.add_argument("--fn", action="append", default=None,
                    help="only functions whose name starts with this (repeatable)")
    ap.add_argument("--verbose", action="store_true", help="list the offsets themselves")
    ap.add_argument("--markdown", action="store_true", help="emit the census table")
    args = ap.parse_args()

    allrows = []
    for bp in args.bp:
        rows = census(bp, args.fn, args.verbose)
        if rows is None:
            return 1
        if not rows:
            print("bp_gate_census: %s -- no function matched %s" % (bp, args.fn))
        allrows += rows

    if args.markdown:
        print("| verb | statements | abs. offsets | gateable | relocation burden |")
        print("|---|---|---|---|---|")
        for r in allrows:
            g, burden = verdict(r)
            print("| `%s::%s` | %d | **%d** | %s | %s |" % (
                r["bp"], r["fn"], r["statements"], len(r["absolute"]), g, burden))
        return 0

    print("%-26s %-30s %5s %5s %5s  %s" % (
        "BP", "function", "stmts", "abs", "rel", "verdict"))
    print("-" * 100)
    for r in allrows:
        g, burden = verdict(r)
        print("%-26s %-30s %5d %5d %5d  %s -- %s" % (
            r["bp"], r["fn"][:30], r["statements"], len(r["absolute"]),
            len(r["relative"]), g, burden))
        if args.verbose and r["absolute"]:
            for t, k, v, op in r["absolute"]:
                print("      reloc  %-22s %-16s = %-8d (%s)" % (t, k, v, op))
        if args.verbose and r["relative"]:
            print("      LEAVE ALONE: %d relative skip size(s) -- %s" % (
                len(r["relative"]),
                ", ".join("%s.%s=%d" % (t, k, v) for t, k, v in r["relative"][:6])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
