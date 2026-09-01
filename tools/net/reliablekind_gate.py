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

  2. A net-thread-terminal declaration that no longer matches the code -- either its
     named interception file stopped mentioning it (the interception was deleted and
     the kind now silently reaches the inbox with no handler), or it GREW a receiver
     case label (the exception is stale and is now hiding a real second consumer).

  3. One enumerator claimed by two families. The families are chained in order, so the
     later one is dead code that reads as wired. Currently zero; this keeps it there.

  4. A receiver case label for a name that is not in the enum, and any parse that comes
     back implausible (no enum, an empty family file). It fails CLOSED: a gate that
     cannot read its subject must not return PASS.

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

# Kinds that never reach event_feed at all because the NET THREAD consumes them and
# returns before the inbox. Each entry names the file that must still do so: delete the
# interception and check 2 fires, rather than the kind quietly becoming an unknown-kind
# warning in the field.
NET_THREAD_TERMINAL = {
    "SaveTransferBegin": ("src/coop/net/session.cpp",
                          "W3: diverted to the net thread so the announce lands in the same "
                          "lane order as its chunks. session.cpp:643 returns."),
    "SaveTransferChunk": ("src/coop/net/session.cpp",
                          "v56: the one BULK kind -- 65000B payloads bypass the 228B inbox. "
                          "session.cpp:583 returns."),
    "AuthHello": ("src/coop/net/peer_admission.cpp",
                  "v144 admission: answered before a slot exists, so there is no session "
                  "for event_feed to act on."),
    "AuthChallenge": ("src/coop/net/peer_admission.cpp",
                      "v144 admission: net-thread-terminal in both directions."),
    "AuthProof": ("src/coop/net/peer_admission.cpp",
                  "v144 admission: net-thread-terminal in both directions."),
}

CASE = re.compile(r"case\s+(?:coop::)?(?:net::)?ReliableKind::(\w+)\s*:")
ENUM_BLOCK = re.compile(r"enum class ReliableKind\s*:\s*uint8_t\s*\{(.*?)\n\};", re.S)
ENUM_ROW = re.compile(r"^\s{4}([A-Za-z_]\w*)\s*=\s*(\d+)", re.M)


def strip_comments(text: str) -> str:
    """Blank out // and /* */ so a case label quoted in prose is never read as wiring.

    Rewritten to spaces rather than deleted so no two code tokens are glued together.
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
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == chr(92) else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_tree() -> dict:
    """Every file the gate reasons about, as text. One dict so --drill can mutate a copy."""
    want = [PROTO, LANES, FEED_FILE] + FAMILY_FILES + \
           sorted({f for f, _ in NET_THREAD_TERMINAL.values()})
    tree = {}
    for rel in want:
        p = SRC / rel
        if not p.is_file():
            tree[rel] = None          # check 4 turns this into a FAIL, not a crash
        else:
            tree[rel] = p.read_text(encoding="utf-8", errors="replace")
    return tree


def parse_enum(text):
    m = ENUM_BLOCK.search(text or "")
    if not m:
        return {}
    return {r.group(1): int(r.group(2)) for r in ENUM_ROW.finditer(m.group(1))}


def check(tree: dict):
    """The whole gate as a pure function over file texts. Returns a list of failures."""
    fails = []

    # --- check 4 first: the gate must be able to READ its subject ------------------
    for rel, text in tree.items():
        if text is None:
            fails.append(f"{rel}: missing. The gate cannot verify a tree it cannot read, "
                         f"so this is a FAIL and not a skip.")
    if fails:
        return fails, {}, {}

    enums = parse_enum(tree[PROTO])
    if len(enums) < 100:
        fails.append(f"{PROTO}: parsed {len(enums)} ReliableKind enumerators, expected >=100. "
                     f"The enum moved or its shape changed -- fix this parser before trusting "
                     f"any verdict below it.")
        return fails, enums, {}

    fam = {}
    for rel in FAMILY_FILES:
        got = set(CASE.findall(strip_comments(tree[rel])))
        if not got:
            fails.append(f"{rel}: zero ReliableKind case labels. A family dispatcher with no "
                         f"membership declaration is either renamed or gutted; either way the "
                         f"coverage below would be silently wrong.")
        fam[rel] = got
    feed = set(CASE.findall(strip_comments(tree[FEED_FILE])))
    if fails:
        return fails, enums, fam

    famall = set()
    for got in fam.values():
        famall |= got
    receivers = famall | feed

    # --- check 4b: a case label for a name the enum does not have -------------------
    for name in sorted(receivers - set(enums)):
        fails.append(f"receiver case label 'ReliableKind::{name}' names a kind that is not in "
                     f"the enum. A renamed or retired kind left a handler behind.")

    # --- check 1: every kind reaches a receiver ------------------------------------
    for name in sorted(set(enums) - receivers - set(NET_THREAD_TERMINAL),
                       key=lambda k: enums[k]):
        fails.append(
            f"{name} = {enums[name]} has NO receiver: no case label in any family dispatcher "
            f"nor in event_feed's own switch, and it is not declared net-thread-terminal. On "
            f"the wire it will reach `event_feed: unknown ReliableKind {enums[name]} -- "
            f"dropping` and the lane is DEAD. Add the case to the right "
            f"event_dispatch_<family>.cpp (that case IS the family-membership declaration), "
            f"or -- if the net thread really consumes it before the inbox -- declare it in "
            f"NET_THREAD_TERMINAL here with the file that does so.")

    # --- check 2: the declared exceptions still describe the code ------------------
    for name, (rel, _why) in sorted(NET_THREAD_TERMINAL.items(),
                                    key=lambda kv: enums.get(kv[0], 0)):
        if name not in enums:
            fails.append(f"NET_THREAD_TERMINAL lists {name}, which is no longer a ReliableKind. "
                         f"Drop the entry.")
            continue
        if name in receivers:
            fails.append(f"{name} is declared net-thread-terminal but now HAS a receiver case "
                         f"label. One of the two is wrong: either the exception is stale, or a "
                         f"second consumer was added for a message the net thread already ate.")
        if not re.search(r"ReliableKind::" + name + r"\b", strip_comments(tree[rel])):
            fails.append(f"{name} is declared net-thread-terminal in {rel}, but that file no "
                         f"longer mentions it. If the interception was removed, this kind now "
                         f"reaches the inbox with no handler at all.")

    # --- check 3: two families claiming one kind -----------------------------------
    for name in sorted(set(enums), key=lambda k: enums[k]):
        who = [pathlib.PurePosixPath(rel).name for rel, got in fam.items() if name in got]
        if len(who) > 1:
            fails.append(f"{name} = {enums[name]} is claimed by {len(who)} families ({', '.join(who)}). "
                         f"event_feed chains them in order, so all but the first are dead code "
                         f"that reads as wired.")

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
    for name in sorted(enums, key=lambda k: enums[k]):
        where = "-"
        for rel, got in fam.items():
            if name in got:
                where = pathlib.PurePosixPath(rel).name.replace("event_dispatch_", "").replace(".cpp", "")
        if name in feed:
            where = "event_feed (special)"
        if name in NET_THREAD_TERMINAL:
            where = "net thread (terminal)"
        print("  %3d %-26s %-24s %-7s %-6s %s"
              % (enums[name], name, where, lanes["lane"].get(name, "Normal*"),
                 "yes" if name in lanes["relay"] else "-",
                 "yes" if name in lanes["preworld"] else "-"))
    print("  (* = falls to the switch default. All three session_lanes.h switches default")
    print("   safely -- Normal / no-relay / hold-until-world-ready -- so absence there is")
    print("   legitimate for every host-authored kind and is reported, not gated.)")


# --- the injected must-fire controls -------------------------------------------------

def drill() -> int:
    """Each control must make the gate FAIL, and for the stated reason.

    A gate nobody has seen refuse is a gate nobody has tested. These mutate a COPY of the
    tree in memory; nothing on disk is touched.
    """
    base = read_tree()
    clean, enums, _fam = check(base)
    if clean:
        print("reliablekind_gate: DRILL ABORTED -- the unmutated tree already fails; fix that "
              "first, the controls below cannot be read against a red baseline.")
        return 1

    controls = []

    # 1. the shipped-dead class: a kind loses its receiver case label.
    victim = "LightGroupState" if "LightGroupState" in enums else sorted(enums)[-1]
    t = dict(base)
    for rel in FAMILY_FILES + [FEED_FILE]:
        t[rel] = re.sub(r"case\s+(?:coop::)?(?:net::)?ReliableKind::" + victim + r"\s*:",
                        "case net::ReliableKind::__removed_by_drill:", t[rel])
    controls.append(("a kind with no receiver", t, "has NO receiver"))

    # 2. a net-thread-terminal declaration whose interception was deleted.
    t = dict(base)
    rel = NET_THREAD_TERMINAL["AuthProof"][0]
    t[rel] = t[rel].replace("ReliableKind::AuthProof", "ReliableKind::__gone_by_drill")
    controls.append(("an exception whose interception vanished", t, "no longer mentions it"))

    # 3. two families claiming one kind.
    t = dict(base)
    t[FAMILY_FILES[0]] = t[FAMILY_FILES[0]].replace(
        "switch (msg.kind) {",
        "switch (msg.kind) {\n    case net::ReliableKind::DoorState: break;", 1)
    controls.append(("one kind claimed twice", t, "is claimed by"))

    # 4. an unreadable subject must not pass.
    t = dict(base)
    t[PROTO] = "// the enum is gone"
    controls.append(("an unparseable enum", t, "expected >=100"))

    bad = 0
    for label, mutated, expect in controls:
        got, _e, _f = check(mutated)
        hit = any(expect in f for f in got)
        print("reliablekind_gate: DRILL %-42s %s"
              % (label, "fired" if hit else "*** DID NOT FIRE ***"))
        if not hit:
            bad += 1
            for f in got[:3]:
                print("    (got instead) " + f)
    if bad:
        print(f"reliablekind_gate: DRILL FAILED -- {bad} control(s) did not fire. The gate is "
              f"not proven to refuse anything; do not trust its PASS.")
        return 1
    print("reliablekind_gate: DRILL PASS -- all 4 controls refused.")
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
        print("reliablekind_gate: PASS -- %d kinds; %d routed by a family, %d by event_feed's "
              "own switch, %d net-thread-terminal by declaration; 0 claimed twice."
              % (len(enums), len(famall), len(feed - famall), len(NET_THREAD_TERMINAL)))
        if "--table" in argv:
            print_table(tree, enums, fam)
    else:
        print("reliablekind_gate: %d FAIL over %d kinds" % (len(fails), len(enums)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
