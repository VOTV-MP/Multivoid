#!/usr/bin/env python3
"""reliablekind_gate -- every ReliableKind reaches a receiver, or names why it does not.

WHY THIS EXISTS. Three times a sync lane has shipped WIRE-DEAD: the enum entry, the
payload, the sender and the module wiring all landed, the build was clean, the smoke
passed -- and the message died at `event_feed`'s `default:` because no family switch
claimed it. v70 `DeskLogLine` shipped dead from day one; v71 `SleepState` vanished into
the host's drain; 2026-09-01 `LightGroupState` was published as "live cross-peer" and a
post-ship audit found it had never once reached a receiver.

The written checklist (`[[feedback-reliablekind-router-checklist]]`) did not stop the
third one -- it was in the session's own indexed memory at the time. A memo cannot hold
a wiring invariant; a gate can.

WHAT IT HOLDS -- these FAIL the build:

  1. An enumerator with NO receiver case label, that is not a declared net-thread-
     terminal kind. This is the shipped-dead class. Verified against real history: run
     against 96d14250 (the commit that shipped the defect) it fails and names
     LightGroupState; against 7fed1c1a (the audit fold) it passes.

  2. A net-thread-terminal declaration whose INTERCEPTION is gone. Each exception names
     a required pattern that must still match -- the branch itself, not the kind's name.
     The first version of this check asked only whether the file still MENTIONED the
     kind, and every one of the three real deletions passed it: both files name these
     kinds in unrelated places (lane sizing, the sends), so removing the terminal
     `return;` changed nothing the gate read. An exception is a claim about a BRANCH, so
     the check has to be about the branch.

  3. A family dispatcher that is no longer CHAINED. The 100 case labels in the five
     family files are reachable only because event_feed's `default:` calls each
     Handle*Event in turn; delete one call and ~25 kinds die while every case label is
     still present and the coverage arithmetic still adds up.

  4. One enumerator claimed by two families (event_feed chains them in order, so all but
     the first are dead code that reads as wired), a receiver case label for a name that
     is not in the enum, a second `switch (msg.kind)` in a dispatcher file (which would
     let a sender-side or filter switch count as receiver coverage), an enum row this
     parser cannot read, or any file it cannot open. It fails CLOSED: a gate that cannot
     read its subject must not return PASS.

WHAT IT DELIBERATELY DOES NOT HOLD. `session_lanes.h`'s three switches -- `LaneForKind`,
`IsClientRelayableReliableKind`, `IsPreWorldSendableKind` -- all default safely (Normal /
no-relay / hold-until-world-ready), and whether a given kind SHOULD relay is a design
decision no text scan can make. Absence there is legitimate for every host-authored kind.
So they are reported in `--table`, not gated: this file must not manufacture a failure it
cannot justify, or the next author learns to route around it.

Note that membership in `session_lanes.h` is NOT receiver coverage. That is not a detail:
the 2026-09-01 dead kind WAS in that header (79 case labels), so a gate written around
"is it mentioned anywhere" would have passed the very defect this one exists to catch.

USAGE
    python tools/net/reliablekind_gate.py            # the gate
    python tools/net/reliablekind_gate.py --table    # + the per-kind wiring table
    python tools/net/reliablekind_gate.py --drill    # injected must-fire controls, then the gate
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src" / "votv-coop"

PROTO = "include/coop/net/protocol.h"
LANES = "src/coop/net/session_lanes.h"
SESSION = "src/coop/net/session.cpp"

# The five family dispatchers. Each Handle*Event returns true iff msg.kind is in its
# family, so its own switch IS the membership declaration (SyncRouter consolidation,
# 2026-06-28) -- which is exactly why parsing these case labels is parsing the truth
# and not a parallel list that can drift from it.
FAMILY_FILES = [
    "src/coop/dispatch/event_dispatch_entity.cpp",
    "src/coop/dispatch/event_dispatch_intent.cpp",
    "src/coop/dispatch/event_dispatch_signal.cpp",
    "src/coop/dispatch/event_dispatch_state.cpp",
    "src/coop/dispatch/event_dispatch_world.cpp",
]
# event_feed's own top-level switch: the handful of specials that are disjoint from every
# family (Join / Balance / Teleport / Wisp / Snapshot / the handshake kinds).
FEED_FILE = "src/coop/dispatch/event_feed.cpp"

# Kinds the NET THREAD consumes and returns on, before the inbox -- so they legitimately
# reach no family switch. Each names the pattern that must STILL MATCH for that to be
# true. `must` entries are (file, regex, what the regex is looking at); the regex runs
# over comment-stripped source, so a commented-out interception does not satisfy it.
NET_THREAD_TERMINAL = {
    "SaveTransferBegin": {
        "reason": "W3: diverted to the net thread so the announce lands in the same lane "
                  "order as its chunks. Retired the game-thread case with it (RULE 2), so "
                  "losing this branch drops the announce entirely and joins break.",
        "must": [(SESSION, r"==\s*ReliableKind::SaveTransferBegin\s*\)\s*\{",
                  "the net-thread branch that diverts the announce to saveBeginSink_")],
    },
    "SaveTransferChunk": {
        "reason": "v56: the one BULK kind -- 65000B payloads bypass the 228B inbox. Losing "
                  "this branch makes the chunk fail the payload cap and return SILENTLY, "
                  "with no warning at all.",
        "must": [(SESSION, r"==\s*ReliableKind::SaveTransferChunk\s*\)\s*\{",
                  "the net-thread branch that diverts the chunk to bulkSink_")],
    },
}
# The three admission kinds share ONE interception, so they share its required pattern --
# and each must additionally still be named by the predicate that interception calls.
for _k in ("AuthHello", "AuthChallenge", "AuthProof"):
    NET_THREAD_TERMINAL[_k] = {
        "reason": "v144 admission: answered on the net thread before a slot exists, in both "
                  "directions. Reaching the inbox costs an unknown-kind warning per replay.",
        "must": [(SESSION, r"IsAdmissionKind\(.*\)\s*return\s*;",
                  "the net-thread early return for admission kinds"),
                 (LANES, r"IsAdmissionKind[\s\S]{0,400}?ReliableKind::" + _k + r"\b",
                  "this kind's membership in IsAdmissionKind")],
    }

CASE = re.compile(r"case\s+(?:coop::)?(?:net::)?ReliableKind::(\w+)\s*:")
ENUM_BLOCK = re.compile(r"enum class ReliableKind\s*:\s*uint8_t\s*\{(.*?)\n\};", re.S)
# Name required; value optional (an implicit-increment enumerator is still a KIND, and the
# first version of this regex demanded `= <decimal>` at exactly four spaces -- so a row
# written `NewLane,` was not parsed at all and check 1 never asked it for a receiver. The
# parser failed OPEN for precisely the defect class this gate exists to catch.)
ENUM_ROW = re.compile(r"^\s+([A-Za-z_]\w*)\s*(?:=\s*(0[xX][0-9a-fA-F]+|\d+))?\s*,?\s*$")
MSG_SWITCH = re.compile(r"switch\s*\(\s*msg\.kind\s*\)")
HANDLER_DEF = re.compile(r"\bbool\s+(Handle\w+Event)\s*\(")
# A char literal, and nothing else. `'` also appears as the C++ DIGIT SEPARATOR -- there is
# a live `60'000` in peer_admission.cpp -- and scanning from it to "the next apostrophe"
# ran into an English apostrophe three lines down and blanked the lines between.
CHAR_LIT = re.compile(r"'(?:\\.|[^\\'\n])'")


def strip_comments(text: str) -> str:
    """Blank out // and /* */ and string/char literals, preserving line structure.

    Rewritten to spaces rather than deleted so no two code tokens are glued together and
    every line number is preserved.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == chr(92) else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == "'":
            m = CHAR_LIT.match(text, i)
            if m:
                out.append(" " * (m.end() - i))
                i = m.end()
            else:
                out.append(c)      # a digit separator, or prose -- an ordinary character
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_tree() -> dict:
    """Every file the gate reasons about, as text. One dict so --drill can mutate a copy."""
    want = {PROTO, LANES, SESSION, FEED_FILE} | set(FAMILY_FILES)
    for spec in NET_THREAD_TERMINAL.values():
        want |= {f for f, _rx, _what in spec["must"]}
    tree = {}
    for rel in sorted(want):
        p = SRC / rel
        try:
            tree[rel] = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            tree[rel] = None       # a FAIL below, not a traceback and not a skip
    return tree


def parse_enum(text):
    """Return (name -> value-or-None, [unreadable rows]).

    Every non-blank line of the enum body must be recognised. A row this parser cannot
    read is reported, because silently dropping one is indistinguishable from the kind
    not existing -- and a kind that does not exist is never asked for a receiver.
    """
    m = ENUM_BLOCK.search(strip_comments(text or ""))
    if not m:
        return {}, []
    enums, bad = {}, []
    for line in m.group(1).splitlines():
        if not line.strip():
            continue
        r = ENUM_ROW.match(line)
        if r:
            enums[r.group(1)] = int(r.group(2), 0) if r.group(2) else None
        else:
            bad.append(line.strip()[:80])
    return enums, bad


def check(tree: dict):
    """The whole gate as a pure function over file texts. Returns (fails, enums, fam)."""
    fails = []

    # --- the gate must be able to READ its subject --------------------------------
    for rel, text in sorted(tree.items()):
        if text is None:
            fails.append(f"{rel}: missing or unreadable. The gate cannot verify a tree it "
                         f"cannot read, so this is a FAIL and not a skip.")
    if fails:
        return fails, {}, {}

    enums, unreadable = parse_enum(tree[PROTO])
    for row in unreadable:
        fails.append(f"{PROTO}: cannot parse this ReliableKind row -- '{row}'. An enumerator "
                     f"this gate drops is never asked for a receiver, so an unreadable row is "
                     f"a hole in exactly the check it would have been subject to.")
    if len(enums) < 100:
        fails.append(f"{PROTO}: parsed {len(enums)} ReliableKind enumerators, expected >=100. "
                     f"The enum moved or its shape changed -- fix this parser before trusting "
                     f"any verdict below it.")
    if fails:
        return fails, enums, {}

    fam = {}
    for rel in FAMILY_FILES:
        code = strip_comments(tree[rel])
        got = set(CASE.findall(code))
        if not got:
            fails.append(f"{rel}: zero ReliableKind case labels. A family dispatcher with no "
                         f"membership declaration is either renamed or gutted; either way the "
                         f"coverage below would be silently wrong.")
        n_sw = len(MSG_SWITCH.findall(code))
        if n_sw != 1:
            fails.append(f"{rel}: {n_sw} `switch (msg.kind)` blocks, expected exactly 1. This "
                         f"gate counts case labels per FILE, so a second switch over "
                         f"ReliableKind (a sender-side helper, a relay filter) would count as "
                         f"receiver coverage and re-open the class this gate closes.")
        fam[rel] = got
    feed_code = strip_comments(tree[FEED_FILE])
    feed = set(CASE.findall(feed_code))
    if fails:
        return fails, enums, fam

    # --- every family is still CHAINED --------------------------------------------
    # 100 case labels are reachable only because event_feed's default: calls each
    # Handle*Event in turn. Delete one call and ~25 kinds die with every label intact.
    for rel in FAMILY_FILES:
        handlers = set(HANDLER_DEF.findall(strip_comments(tree[rel])))
        if not handlers:
            fails.append(f"{rel}: no `bool Handle*Event(` definition found -- this gate cannot "
                         f"confirm the family is chained, so it will not pass it.")
            continue
        for h in sorted(handlers):
            if not re.search(r"\b" + h + r"\s*\(", feed_code):
                fails.append(f"{h} is defined in {rel} but never CALLED in {FEED_FILE}. Its "
                             f"{len(fam[rel])} case labels are unreachable: the kinds fall "
                             f"through to `unknown ReliableKind` while every label is still "
                             f"there and the coverage arithmetic still adds up.")

    famall = set()
    for got in fam.values():
        famall |= got
    receivers = famall | feed

    # --- a case label for a name the enum does not have ----------------------------
    for name in sorted(receivers - set(enums)):
        fails.append(f"receiver case label 'ReliableKind::{name}' names a kind that is not in "
                     f"the enum. A renamed or retired kind left a handler behind.")

    # --- check 1: every kind reaches a receiver ------------------------------------
    def idof(n):
        return enums.get(n) if enums.get(n) is not None else "?"
    for name in sorted(set(enums) - receivers - set(NET_THREAD_TERMINAL),
                       key=lambda k: (enums.get(k) is None, enums.get(k) or 0)):
        fails.append(
            f"{name} = {idof(name)} has NO receiver: no case label in any family dispatcher "
            f"nor in event_feed's own switch, and it is not declared net-thread-terminal. On "
            f"the wire it will reach `event_feed: unknown ReliableKind {idof(name)} -- "
            f"dropping` and the lane is DEAD. Add the case to the right "
            f"event_dispatch_<family>.cpp (that case IS the family-membership declaration), "
            f"or -- if the net thread really consumes it before the inbox -- declare it in "
            f"NET_THREAD_TERMINAL here with the pattern that proves it.")

    # --- check 2: the declared exceptions still describe the code -------------------
    for name, spec in sorted(NET_THREAD_TERMINAL.items(),
                             key=lambda kv: (enums.get(kv[0]) is None, enums.get(kv[0]) or 0)):
        if name not in enums:
            fails.append(f"NET_THREAD_TERMINAL lists {name}, which is no longer a ReliableKind. "
                         f"Drop the entry.")
            continue
        if name in receivers:
            fails.append(f"{name} is declared net-thread-terminal but now HAS a receiver case "
                         f"label. One of the two is wrong: either the exception is stale, or a "
                         f"second consumer was added for a message the net thread already ate.")
        for rel, rx, what in spec["must"]:
            if not re.search(rx, strip_comments(tree[rel])):
                fails.append(f"{name} is declared net-thread-terminal, but {rel} no longer "
                             f"contains {what} (/{rx}/). If that interception was removed, this "
                             f"kind now reaches the inbox with no handler at all. Asking only "
                             f"whether the file still MENTIONS the kind is not enough -- both "
                             f"these files name these kinds in unrelated places.")

    # --- check 4: two families claiming one kind -----------------------------------
    for name in sorted(set(enums), key=lambda k: (enums.get(k) is None, enums.get(k) or 0)):
        who = [pathlib.PurePosixPath(rel).name for rel, got in fam.items() if name in got]
        if len(who) > 1:
            fails.append(f"{name} = {idof(name)} is claimed by {len(who)} families "
                         f"({', '.join(who)}). event_feed chains them in order, so all but the "
                         f"first are dead code that reads as wired.")

    return fails, enums, fam


# --- the advisory table --------------------------------------------------------------

def parse_lane_switches(text: str):
    """LaneForKind / IsClientRelayableReliableKind / IsPreWorldSendableKind.

    Reported, never gated: all three default safely and "should this relay" is a design
    call. The table exists so a new kind's whole wiring is visible in one place, which is
    what the checklist's step 5 asked a human to do by grep.
    """
    src = strip_comments(text)
    out = {"lane": {}, "relay": set(), "preworld": set()}

    def body(sig):
        i = src.find(sig)
        if i < 0:
            return ""
        i = src.find("{", i)
        depth, j = 0, i
        while j < len(src):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    return src[i:j]
            j += 1
        return ""

    pending = []
    for line in body("Lane LaneForKind").splitlines():
        m = CASE.search(line)
        if m:
            pending.append(m.group(1))
        r = re.search(r"return\s+Lane::(\w+)", line)
        if r:
            for k in pending:
                out["lane"][k] = r.group(1)
            pending = []

    for key, sig in (("relay", "bool IsClientRelayableReliableKind"),
                     ("preworld", "bool IsPreWorldSendableKind")):
        pending = []
        for line in body(sig).splitlines():
            m = CASE.search(line)
            if m:
                pending.append(m.group(1))
            if re.search(r"return\s+true", line):
                out[key] |= set(pending)
                pending = []
            elif re.search(r"\bdefault\s*:", line):
                pending = []
    return out


def print_table(tree, enums, fam):
    lanes = parse_lane_switches(tree[LANES])
    feed = set(CASE.findall(strip_comments(tree[FEED_FILE])))
    print("")
    print("  id  kind                       receiver                 lane    relay  preworld")
    print("  --- -------------------------- ------------------------ ------- -----  --------")
    for name in sorted(enums, key=lambda k: (enums.get(k) is None, enums.get(k) or 0)):
        where = "-"
        for rel, got in fam.items():
            if name in got:
                where = pathlib.PurePosixPath(rel).name.replace("event_dispatch_", "").replace(".cpp", "")
        if name in feed:
            where = "event_feed (special)"
        if name in NET_THREAD_TERMINAL:
            where = "net thread (terminal)"
        print("  %3s %-26s %-24s %-7s %-6s %s"
              % (enums[name] if enums[name] is not None else "?", name, where,
                 lanes["lane"].get(name, "Normal*"),
                 "yes" if name in lanes["relay"] else "-",
                 "yes" if name in lanes["preworld"] else "-"))
    print("  (* = falls to the switch default. All three session_lanes.h switches default")
    print("   safely -- Normal / no-relay / hold-until-world-ready -- so absence there is")
    print("   legitimate for every host-authored kind and is reported, not gated.)")


# --- the injected must-fire controls -------------------------------------------------

def drill() -> int:
    """Each control must make the gate FAIL, and for the stated reason.

    A gate nobody has seen refuse is a gate nobody has tested. These mutate a COPY of the
    tree in memory; nothing on disk is touched. Every expected string NAMES ITS VICTIM, so
    a control cannot be satisfied by some unrelated failure the mutation happened to cause.
    """
    base = read_tree()
    clean, enums, _fam = check(base)
    if clean:
        print("reliablekind_gate: DRILL ABORTED -- the unmutated tree already fails; fix that "
              "first, the controls below cannot be read against a red baseline.")
        for f in clean[:5]:
            print("    " + f)
        return 1

    controls = []

    # 1. the shipped-dead class: a kind loses its receiver case label.
    victim = "LightGroupState" if "LightGroupState" in enums else sorted(enums)[-1]
    t = dict(base)
    for rel in FAMILY_FILES + [FEED_FILE]:
        t[rel] = re.sub(r"case\s+(?:coop::)?(?:net::)?ReliableKind::" + victim + r"\s*:",
                        "case net::ReliableKind::__removed_by_drill:", t[rel])
    controls.append(("a kind with no receiver", t, victim + " = 129 has NO receiver"))

    # 2. THE REAL DELETION, not a global rename. Remove only the terminal `return;` --
    #    every mention of the kind stays exactly where it was, which is what defeated the
    #    first version of check 2.
    t = dict(base)
    t[SESSION] = t[SESSION].replace(
        "if (IsAdmissionKind(static_cast<ReliableKind>(rh.kind))) return;", "", 1)
    controls.append(("the admission interception deleted (mentions intact)", t,
                     "AuthProof is declared net-thread-terminal"))

    # 2b. the same for a SaveTransfer divert: drop only the branch's opening.
    t = dict(base)
    t[SESSION] = t[SESSION].replace(
        "if (static_cast<ReliableKind>(rh.kind) == ReliableKind::SaveTransferBegin) {",
        "if (false) {", 1)
    controls.append(("the SaveTransferBegin divert deleted (mentions intact)", t,
                     "SaveTransferBegin is declared net-thread-terminal"))

    # 3. a family stops being chained: every case label survives, the call does not.
    t = dict(base)
    t[FEED_FILE] = t[FEED_FILE].replace("if (HandleWorldEvent(session, msg)) break;", "", 1)
    controls.append(("a family dispatcher no longer chained", t,
                     "HandleWorldEvent is defined in"))

    # 4. two families claiming one kind.
    t = dict(base)
    t[FAMILY_FILES[0]] = t[FAMILY_FILES[0]].replace(
        "switch (msg.kind) {",
        "switch (msg.kind) {\n    case net::ReliableKind::DoorState: break;", 1)
    controls.append(("one kind claimed twice", t, "DoorState = 9 is claimed by"))

    # 5. an enum row this parser cannot read must not pass silently.
    t = dict(base)
    t[PROTO] = t[PROTO].replace("    LightGroupState = 129,",
                                "    LightGroupState 129 %% unreadable", 1)
    controls.append(("an unreadable enum row", t, "cannot parse this ReliableKind row"))

    # 6. an unreadable subject must not pass.
    t = dict(base)
    t[PROTO] = "// the enum is gone"
    controls.append(("an unparseable enum", t, "expected >=100"))

    bad = 0
    for label, mutated, expect in controls:
        got, _e, _f = check(mutated)
        hit = any(expect in f for f in got)
        print("reliablekind_gate: DRILL %-48s %s"
              % (label, "fired" if hit else "*** DID NOT FIRE ***"))
        if not hit:
            bad += 1
            print("    expected a failure containing: " + expect)
            for f in got[:3]:
                print("    (got instead) " + f)
    if bad:
        print(f"reliablekind_gate: DRILL FAILED -- {bad} control(s) did not fire. The gate is "
              f"not proven to refuse anything; do not trust its PASS.")
        return 1
    print(f"reliablekind_gate: DRILL PASS -- all {len(controls)} controls refused.")
    return 0


def main() -> int:
    argv = sys.argv[1:]
    if "--drill" in argv:
        if drill() != 0:
            return 1

    tree = read_tree()
    fails, enums, fam = check(tree)
    for f in fails:
        print("reliablekind_gate: FAIL: " + f)

    if not fails:
        famall = set()
        for got in fam.values():
            famall |= got
        feed = set(CASE.findall(strip_comments(tree[FEED_FILE])))
        print("reliablekind_gate: PASS -- %d kinds; %d routed by a family (all 5 chained), %d "
              "by event_feed's own switch, %d net-thread-terminal with their interceptions "
              "verified; 0 claimed twice."
              % (len(enums), len(famall), len(feed - famall), len(NET_THREAD_TERMINAL)))
        if "--table" in argv:
            print_table(tree, enums, fam)
    else:
        print("reliablekind_gate: %d FAIL over %d kinds" % (len(fails), len(enums)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
