#!/usr/bin/env python3
"""census_history -- the PRIVATE HISTORY: version control for the trees no repository tracks.

WHY THIS EXISTS. `CLAUDE.md`, the memory directory and the ignored docs (`docs/security/`) are real
documentation that no repository tracks -- `docs/DOCS_ARC.md` records the decision to keep them
local -- so a close that reconciled them left no evidence anywhere, and a census had nothing to diff
a doc AGAINST. This module is their git repo: it lives beside the project's slug under
`~/.claude/projects/<slug>/history/`, has no remote (the `research/` pattern), and holds four things:

  the SNAPSHOT      a copy of every private path under its census key, synced before each census so
                    the next one can diff. What vanished from the tree is deleted from the snapshot.
  the STATE         `docs_census_state.json` -- when each doc was last censused WHOLE. The sweep's
                    queue order is this file; a diff-scoped doc is deliberately NOT stamped.
  the TABLE         the filed verdict table per close, under `census/`.
  the LEDGER        `census/resolved.jsonl` -- see below. Appended when a verdict is retired by the
                    fix it ordered, because that fix erases the line the verdict names.

Extracted from `status_census.py` on 2026-09-03 (it was 1,244 LOC against a 800-line soft cap, and
D6/D9/D10 were queued to add to it again). Behaviour is unchanged: the functions moved verbatim.
"""
import io
import json
import os
import shutil
import subprocess
import datetime as _dt
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from census_git import git  # noqa: E402  -- sibling module
from status_grammar import read_text  # noqa: E402


def utc_now():
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


# ----------------------------------------------------------------------------- history / state
def history_init(env):
    h = env.history
    os.makedirs(h, exist_ok=True)
    if not os.path.isdir(os.path.join(h, ".git")):
        git(["init", "-q"], h)
    name = git(["config", "--local", "user.name"], env.repo, check=False).strip()
    mail = git(["config", "--local", "user.email"], env.repo, check=False).strip()
    if not name or not mail:
        raise SystemExit("REFUSE: main has no local user.name/user.email to copy into the history repo "
                         "(CLAUDE.md git-identity rule: set the same in any NEW repo)")
    hn = git(["config", "--local", "user.name"], h, check=False).strip()
    hm = git(["config", "--local", "user.email"], h, check=False).strip()
    if (hn, hm) != (name, mail):
        if hn or hm:
            raise SystemExit("REFUSE: history identity {} <{}> != main {} <{}>".format(hn, hm, name, mail))
        git(["config", "--local", "user.name", name], h)
        git(["config", "--local", "user.email", mail], h)
    return h


def snapshot_sync(env, rs):
    """Copy every PRIVATE read-set path (and the whole memory dir) into the history worktree
    under its key; delete what vanished. -> list of changed keys (git status of the worktree)."""
    h = history_init(env)
    wanted = {}
    for key, (owner, ap) in rs.items():
        if owner == "private":
            wanted[key] = ap
    if os.path.isdir(env.memory):
        for f in os.listdir(env.memory):
            ap = os.path.join(env.memory, f)
            if os.path.isfile(ap):
                wanted["memory/" + f] = ap
    for key, ap in wanted.items():
        dst = os.path.join(h, key)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(ap, dst)
    for sub in ("memory", "docs", "research"):
        root = os.path.join(h, sub)
        for dirpath, dirnames, filenames in os.walk(root):
            for f in filenames:
                rel = os.path.relpath(os.path.join(dirpath, f), h).replace("\\", "/")
                if rel not in wanted:
                    os.remove(os.path.join(dirpath, f))
    if os.path.isfile(os.path.join(h, "CLAUDE.md")) and "CLAUDE.md" not in wanted:
        os.remove(os.path.join(h, "CLAUDE.md"))
    if subprocess.run(["git", "rev-parse", "--verify", "-q", "HEAD"], cwd=h, capture_output=True).returncode != 0:
        # FIRST RUN: no history to diff against. The snapshot is committed as the BASELINE and no
        # private path is "changed" -- the sweep reaches them over the cycle; the next close diffs.
        git(["add", "-A"], h)   # the worktree holds only the snapshot at this point (census/ is written after)
        git(["commit", "-q", "--allow-empty", "-m", "[docs] history baseline: first snapshot of the trees no repository tracks"], h)
        n = len(git(["ls-files", "-z"], h).split("\0")) - 1
        print("history baseline committed: {} files in {}".format(n, h))
        return []
    changed = []
    for rec in git(["status", "--porcelain", "-z", "--untracked-files=all"], h).split("\0"):
        p = rec[3:] if len(rec) > 3 else ""
        if p and not p.startswith("census/") and p != "docs_census_state.json":
            changed.append(p)
    return changed


def state_load(env):
    p = os.path.join(env.history, "docs_census_state.json")
    t = read_text(p)
    return json.loads(t) if t else {"docs": {}}


def state_save(env, st):
    p = os.path.join(env.history, "docs_census_state.json")
    io.open(p, "w", encoding="utf-8").write(json.dumps(st, indent=1, sort_keys=True))


# ----------------------------------------------------------------------------- the resolved ledger
# WHY (D0). A verdict is a MEASUREMENT taken at a moment, and the close records only the LAST one.
# The action a verdict ORDERS is an edit to the line it names -- so the moment a defect is FIXED, the
# row's text changes, its hash changes, the carry-forward drops the verdict, and the corrected line
# comes back as a fresh row verdicted STILL TRUE. `[V]` 2026-09-03, the first real close of the
# rebuilt census: two memory topics were verdicted STALE DONE and stamped, and the trailer it wrote
# read `actually-done=0 stale-done=0` -- a run that corrected two claims, reporting that it corrected
# none. The verdict columns are not wrong; they describe the text being committed, which is what the
# content pin exists to guarantee. What was missing is a record of the verdict that MOTIVATED the fix.
#
# So: when a verdict leaves the live table because its line was acted on, it is appended HERE first.
# Two consequences worth stating, because both were design choices:
#   - The record is written at RE-CENSUS time, not at close time -- that is when the verdict is lost,
#     and a record written later would have to reconstruct it.
#   - The record does NOT carry a close sha. It cannot: the sha does not exist yet. It does not need
#     to either -- the ledger is committed by the history commit, so the commit that FIRST contains a
#     record IS the close that published it, recoverable with `git log --oneline -S`.
# The trailer carries the CUMULATIVE totals (`resolved=`, `flips=`) because CI never sees this file;
# a running total is the only property it can check, and it checks the one that matters: a close may
# not un-record what an earlier close recorded (kind MONOTONE, trailer_schema).
def resolved_path(env):
    return os.path.join(env.history, "census", "resolved.jsonl")


def resolved_load(env):
    out = []
    for line in (read_text(resolved_path(env)) or "").splitlines():
        line = line.strip()
        if line:
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def resolved_append(env, recs):
    if not recs:
        return
    p = resolved_path(env)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with io.open(p, "a", encoding="utf-8", newline="\n") as f:
        for r in recs:
            f.write(json.dumps(r, sort_keys=True) + "\n")


# A verdict that named something WRONG. STILL TRUE and NOT A LABEL are verdicts too and are recorded
# with the rest, but a close whose every retired verdict was one of those corrected nothing.
FLIP_VERDICTS = ("STILL OPEN", "ACTUALLY DONE", "STALE DONE", "PARTIAL")


def resolved_counts(env):
    recs = resolved_load(env)
    return len(recs), sum(1 for r in recs if r.get("verdict") in FLIP_VERDICTS)


def lost_unverdicted(prev_rows, rows, radius, whole_hashes=None):
    """Rows that left the table while their verdict was still EMPTY.

    `retired_verdicts` skips these one line before it consults the witness, and that skip is exactly
    the hole the 2026-09-04 close fell into: the operator stamped a doc BEFORE verdicting its rows,
    which made the doc `touched`, which made the re-census read it diff-scoped -- so 21 ageing rows
    left the table and `ageing-corr` reported 0 for a run that corrected 21 rows. Nothing anywhere
    printed a non-zero number (`grep -c unverdicted tools/docs/*.py` found no counter at all).

    The skill's rule -- verdict the whole table, THEN act, THEN re-census -- is prose addressed to
    the hand, and this project's own ledger says a mandate nothing observes is satisfied by
    assertion. This is the observation. It is REPORTED rather than refusing, because the honest
    remedy (restore the doc, verdict, re-edit) is not always available and a refusal with no escape
    is a worse instrument than a loud number; what it must never do is let D8 read a silent zero.
    """
    live = {(r["key"], r["hash"][:12]) for r in rows}
    out = []
    for r in prev_rows:
        if r["verdict"]:
            continue
        ident = (r["key"], r["hash"][:12])
        if ident in live or r["key"] not in radius:
            continue                        # still on the table, or the doc left the radius honestly
        if whole_hashes is not None:
            hs = whole_hashes(r["key"])
            if hs is not None and r["hash"][:12] in hs:
                continue                    # the line is still in the file: the SCOPE changed, not the text
        out.append(r)
    return out


def retired_verdicts(prev_rows, rows, radius, utc, base, whole_hashes=None):
    """Prior verdicts that do NOT carry into this census -> ledger records.

    The RADIUS decides whether the question can be asked at all: a prior row whose doc is not being
    scanned this time simply left the frame -- nothing was acted on, and recording it would inflate
    the ledger with every change of sweep queue.

    THE DOC ITSELF decides the answer, and the first version got this wrong by asking the wrong
    witness. It compared against THIS census's row set, which is not the same view of the file: a doc
    is read WHOLE by the sweep and DIFF-SCOPED when the session touched it. `[V]` 2026-09-03,
    reproduced in a scratch repo -- census 1 sweeps a doc whole, its three rows are verdicted, an edit
    adds ONE line, and the re-census reads the now-touched doc diff-scoped, so all three whole-scan
    rows vanish while the doc is still in the radius: "resolved: 3 verdict(s) retired", none of them
    acted on. The original argued that a touched doc's diff only GROWS, which is true and beside the
    point: it holds only when the PRIOR rows also came from the diff.

    Suppressing every scope change would also suppress the GENUINE case, which has the same shape.
    So the row is asked of the FILE: `whole_hashes(key)` returns every row hash the doc yields when
    read whole, at any scope. Present means the line is still there untouched; absent means it was
    edited or deleted, which is the action the verdict ordered.

    This number feeds `flips=`, the INPUT to D8's falsifier -- and a false positive there biases the
    measurement toward KEEPING the hand phase, the direction that lets a useless step survive its own
    test."""
    live = {(r["key"], r["hash"][:12]) for r in rows}
    out = []
    for r in prev_rows:
        if not r["verdict"]:
            continue
        ident = (r["key"], r["hash"][:12])
        if ident in live or r["key"] not in radius:
            continue
        if whole_hashes is not None:
            hs = whole_hashes(r["key"])
            if hs is not None and r["hash"][:12] in hs:
                continue                   # the line is still in the file, untouched
        out.append({"utc": utc, "base": base[:12], "key": r["key"], "line": r["line"],
                    "hash": r["hash"][:12], "kind": r["kind"], "lane": r.get("lane", ""),
                    "label": r["label"], "substate": " ".join(r.get("substate") or []),
                    "verdict": r["verdict"]})
    return out
