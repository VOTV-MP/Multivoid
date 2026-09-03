#!/usr/bin/env python3
"""status_census_drill -- show tools/docs/status_census.py RED before trusting it green.

Three fixtures, each a class the design claims to hold (docs/DOCUMENTIZE_ARC.md WP-1):

  A  the LABEL GRAMMAR on the REAL lines of the 2026-09-02 staleness sample (section 2.3): the six
     stale-open lines must produce a row (four as labels with their sub-state clause, one as a
     heading, one through a DEAD PATH token on a non-label heading), and the eight vocabulary
     false positives must produce none. Recall AND precision, on real text, not a synthetic RED.
  B  the COMMENT-ONLY lexer: a code change behind a quoted `#` or `//` is CODE; a trailing-comment
     rewrite on an unchanged declaration is comment-only (the /qf round 6 Q4 lines).
  C  the CLOSE in a scratch environment (repo + memory dir + history dir): a neighbour's whole-file
     staged doc survives and is excluded; a missing verdict REFUSES; STILL TRUE on a dead citation
     REFUSES; the good close carries exactly the session's paths with the trailer; a second close
     whose reading order GREW is refused by the ratchet.

    python tools/docs/status_census_drill.py
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import status_census as SC  # noqa: E402

SCRIPT = os.path.join(HERE, "status_census.py")
FAILS = []


def check(cond, what):
    print(("ok      " if cond else "FAILED  ") + what)
    if not cond:
        FAILS.append(what)


# ------------------------------------------------------------------ A. the grammar on real lines
STALE_OPEN = [
    # (line, next line, expects: label kind, sub-state non-empty)
    ("**Status:** PHASE A+B IMPLEMENTED (uncommitted; build+audit+smoke clean, hands-on-pending). "
     "Enabled by the new BP-disassembly", "", "field", True),                                   # 2.3 #1
    ("- **Status (take 4): still NOT hands-on** -- the current build is b125; the label was never folded.",
     "", "lead", True),                                                                          # 2.3 #6
    ("> - **L1 (level-pile client->host) + L2 (proxy interaction window/ERHHH/offset): the open functional bugs.**",
     "", "lead", False),                                                                         # 2.3 #9
    ("## 8. Open questions", "", "heading", False),                                              # 2.3 #12
    ("> - **A2 — DONE** (commit pending). `git rm`'d the two empty `.gitkeep` placeholders; the dead",
     "", "lead", True),                                                                          # 2.3 #20
]
DEAD_PATH_HEADING = ("## Pile-reconcile core (P2 catalog, P3 doom, P5 pending-remove, P4 clock) — in "
                     "zz_prop_adoption_gone.cpp")                                               # 2.3 #11
FALSE_POSITIVES = [
    "| safety | exact 1cm key + chipType + `>50%` valve (denominator = ALL live piles) | own-key != pending-key -> never steal | exact 1cm key |",  # #2
    "| Power panel | 5 breaker bools (mask) | U | code | CO | `power_sync::ApplyMask` | snapshot + pending |",  # #3
    "  radius is uncensused (what live HUD state does the player lose -- open windows, radar,",   # #4
    "  `hud::Render()` on `!PauseMenuOpen()` above it. ESC closes the chat AND opens the native pause menu on",  # #5
    "The morph product's **Init-POST observer does NOT fire** for a BP-deferred clump/pile",     # #10
    DEAD_PATH_HEADING,                                                                            # #11 (not a LABEL)
    "`OpenLevel` when the local pawn has `dead == true` (fail CLOSED), and write the revive in the game's",  # #18
    "a second mechanism for the already-covered no-RTSS case is not load-bearing). Fail-CLOSED on a stale AOB",  # #21
]
PROVENANCE = ["- `[V]` the runtime key is `ATV`, uppercase.", "| B4 | `[RD]` measured on a dead pawn | [A] agent |"]


def drill_grammar():
    print("-- A. label grammar on the section 2.3 lines")
    for line, nxt, kind, sub in STALE_OPEN:
        lab = SC.label_of(line)
        check(lab is not None and lab[0] == kind, "recall  {:<8} {!r}".format(kind, line[:60]))
        if sub:
            check(bool(SC.substate_of(line, nxt)), "sub-state caught        {!r}".format(line[:60]))
    for line in FALSE_POSITIVES:
        check(SC.label_of(line) is None, "precision (no label)    {!r}".format(line[:60]))
    for line in PROVENANCE:
        check(SC.label_of(line) is None, "provenance tag not label {!r}".format(line[:60]))
    # the dead-path heading must still yield a row through the mechanical column
    d = tempfile.mkdtemp(prefix="scg_")
    try:
        p = os.path.join(d, "x.md")
        io.open(p, "w", encoding="utf-8").write("intro\n" + DEAD_PATH_HEADING + "\nafter\n")
        env = SC.Env(repo=d, memory=os.path.join(d, "m"), history=os.path.join(d, "h"))
        rows = SC.scan_doc("x.md", p, SC.Resolver(env))
        check(len(rows) == 1 and rows[0]["kind"] == "cite" and any(s == "gone" for _, s in rows[0]["tokens"]),
              "dead path token on a non-label heading -> a 'cite' row (2.3 #11)")
    finally:
        shutil.rmtree(d, ignore_errors=True)


# ------------------------------------------------------------------ B. the comment-only lexer
def drill_negated_labels():
    """A negated status word must not be captured as its own opposite (post-ship audit, 2026-09-03)."""
    print("-- A2. negated status words")
    cases = [
        ("**Status (superseded, kept for the record): ROOT MEASURED, NOT FIXED.**", "NOT FIXED"),
        ("- **NOT VERIFIED** by any hands-on run.", "NOT VERIFIED"),
        ("| A2 | NOT SHIPPED | the lane is parked |", "NOT SHIPPED"),
        ("- **DONE** and shipped.", "DONE"),
    ]
    for line, want in cases:
        lab = SC.label_of(line)
        check(lab is not None and lab[1] == want,
              "label {!r} (not {!r}) for {!r}".format(want, (lab or ("", "?"))[1], line[:52]))


def drill_vocab_markers():
    """A3: a doc that QUOTES the vocabulary is not making the claims it prints -- and the two scopes
    must behave DIFFERENTLY, because the corpus holds both shapes. `[V]` 2026-09-03: marking only the
    SECTION of each skill's token table still left 7 and 6 rows (an instruction text ABOUT status
    labels quotes them in prose too), while doc-scoping `DOCUMENTIZE_ARC.md` would have thrown away
    38 real claims to suppress 3 quotes."""
    print("-- A3. the two vocabulary-quote scopes")
    import status_grammar as G
    R = SC.Resolver(SC.Env())
    R.index(); R.corpora()
    body = ["# Real", "", "**Status:** OPEN", "", "## Legend", G.VOCAB_MARKER,
            "| DONE | it shipped |", "| STALE | it did not |", "", "## After", "",
            "**Status:** BUILT"]
    rows = G.scan_lines("t.md", body, R)
    got = sorted(r["label"] for r in rows)
    check(got == ["BUILT", "OPEN"],
          "SECTION scope: the legend is suppressed, the claims before AND after it survive "
          "(got {})".format(got))
    doc = [G.VOCAB_MARKER_DOC] + body
    check(G.scan_lines("t.md", doc, R) == [],
          "DOC scope: a doc that quotes the vocabulary throughout yields NO rows")
    mentioned = ["# X", "", "the marker is `" + G.VOCAB_MARKER + "` in prose", "", "**Status:** OPEN"]
    check(len(G.scan_lines("t.md", mentioned, R)) == 1,
          "a doc that MENTIONS the marker in prose does not silently opt out")
    # A `[corr]` stamp NAMES what was wrong, dead paths included -- it is the remedy, not a claim.
    corr = ["# X", "", "> **[corr 2026-09-03: was DONE; `tools/zz_gone.ps1` is GONE; measured]**", "",
            "**Status:** OPEN"]
    rows = G.scan_lines("t.md", corr, R)
    check(len(rows) == 1 and rows[0]["line"] == 5,
          "a [corr] stamp yields NO row -- the census does not flag its own remedy (got {})".format(
              [(r["line"], r["label"]) for r in rows]))


def drill_lexer():
    print("-- B. comment-only lexer")
    a = 'x = re.compile(r"(<FONT COLOR=#[0-9A-Fa-f]{6}>)")\n'
    b = 'x = re.compile(r"(<FONT COLOR=#[0-9A-Fa-f]{8}>)")\n'
    check(not SC.comment_only(a, b, ".py"), "python: a change behind a quoted # is CODE")
    a = 'const size_t scheme = s.find("://");\n'
    b = 'const size_t scheme = s.find("://x");\n'
    check(not SC.comment_only(a, b, ".cpp"), "cpp: a change behind a quoted // is CODE")
    a = "int x = 1; // old claim\n"
    b = "int x = 1; // new, corrected claim\n"
    check(SC.comment_only(a, b, ".cpp"), "cpp: a trailing-comment rewrite is comment-only")
    a = "# old\nfoo = 1\n"
    b = "# new\nfoo = 1\n"
    check(SC.comment_only(a, b, ".py"), "python: a comment-line rewrite is comment-only")
    check(not SC.comment_only('{"a": 1}', '{"a": 2}', ".json"), "json: no grammar -> code")
    check(not SC.comment_only(None, "x", ".cpp"), "a new file is code")


# ------------------------------------------------------------------ C. the close, in a scratch world
def git(args, cwd, input_text=None, env=None):
    r = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", input=input_text, env=env)
    if r.returncode != 0:
        raise RuntimeError("git {}: {}".format(" ".join(args), (r.stderr or r.stdout)[:300]))
    return r.stdout


def run_sc(env_dirs, *args):
    repo, mem, hist = env_dirs
    r = subprocess.run([sys.executable, SCRIPT, "--repo", repo, "--memory-dir", mem, "--history-dir", hist] + list(args),
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, r.stdout + r.stderr


CLAUDE = ("# rules\n\n## Reading order after a session reset / new conversation\n\n"
          "1. first entry\n   more\n2. second\n")


def drill_close():
    print("-- C. the close in a scratch environment")
    root = tempfile.mkdtemp(prefix="scc_")
    repo, mem, hist = (os.path.join(root, n) for n in ("repo", "memory", "history"))
    os.makedirs(os.path.join(repo, "docs"))
    os.makedirs(mem)
    try:
        git(["init", "-q", "-b", "main", "."], repo)
        git(["config", "--local", "user.name", "drill"], repo)
        git(["config", "--local", "user.email", "drill@example"], repo)
        w = lambda rel, text: io.open(os.path.join(repo, rel), "w", encoding="utf-8", newline="\n").write(text)
        w(".gitignore", "CLAUDE.md\n")
        w("CLAUDE.md", CLAUDE)
        w("docs/a.md", "# A\n\n**Status:** OPEN (commit pending)\n")
        w("docs/b.md", "# B\n\nplain prose, no labels\n")
        io.open(os.path.join(mem, "MEMORY.md"), "w", encoding="utf-8").write("# index\n- one\n")
        git(["add", "--", ".gitignore", "docs/a.md", "docs/b.md"], repo)
        git(["commit", "-q", "-m", "base"], repo)
        # the neighbour: edits b.md and stages it WHOLE in the shared index
        w("docs/b.md", "# B\n\nthe neighbour's edit\n")
        git(["add", "--", "docs/b.md"], repo)
        # this session: edits a.md in the worktree only
        # the session ADDS two labelled lines; the pre-existing `**Status:** OPEN` line at a.md:3 is not
        # this session's row and must be left to the sweep (diff scoping, 2026-09-03)
        w("docs/a.md", "# A\n\n**Status:** OPEN (commit pending)\n\n- **PARTIAL** rework, uncommitted\n"
                       "- **DONE** -- see `zz_gone_file.cpp:12` for the site\n")
        E = (repo, mem, hist)
        # `-k 0` switches the SWEEP off so DIFF-SCOPING is drilled in isolation. Since 2026-09-03 a
        # touched doc is a normal sweep candidate (it is the only way `MEMORY.md` / `LESSONS.md` /
        # `CLAUDE.md`, touched by every close, are ever read whole), and this corpus is two docs, so
        # with the sweep on the same run would offer a.md:3 immediately. Both properties are drilled:
        # here that the DIFF charges only the session's lines, below that the SWEEP still reaches the
        # rest of a touched doc.
        code, out = run_sc(E, "census", "--since", "2099-01-01", "-k", "0")
        check(code == 0, "census runs ({})".format(out.strip().splitlines()[-1][:60] if out.strip() else "no output"))
        meta, rows = SC.read_table(os.path.join(hist, "census", "pending.md"))
        check(len(rows) == 2 and rows[1]["kind"] == "lead" and any(s == "gone" for _, s in rows[1]["tokens"]),
              "census: the TWO rows this session ADDED to a.md, the second with a dead citation, got {}".format(len(rows)))
        check(all(r["line"] != 3 for r in rows),
              "the pre-existing `**Status:** OPEN` line (a.md:3) is NOT charged to this session")
        check("docs/b.md" in meta.get("touched", []) and "docs/a.md" in meta.get("touched", []), "census: both docs in radius (i)")
        T = ["--trailer", "Co-Authored-By: Drill <d@e>", "--trailer", "Claude-Session: https://example/x"]
        code, out = run_sc(E, "close", "-m", "drill", *T)
        check(code != 0 and "no verdict token" in out, "RED: close refuses an unverdicted row")
        # fill verdicts: the dead-cite row as STILL TRUE -> contradiction
        pend = os.path.join(hist, "census", "pending.md")
        t = io.open(pend, encoding="utf-8").read()
        lines = t.split("\n")
        def set_verdict(n, v):
            for i, l in enumerate(lines):
                if l.startswith("| {} |".format(n)):
                    cells = l.rstrip().rstrip("|").split("|")
                    cells[-1] = " " + v + " "
                    lines[i] = "|".join(cells) + "|"
        set_verdict(1, "STILL OPEN")
        set_verdict(2, "STILL TRUE")
        io.open(pend, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
        code, out = run_sc(E, "close", "-m", "drill", *T)
        check(code != 0 and "resolved dead" in out, "RED: STILL TRUE on a dead citation refuses")
        set_verdict(2, "STALE DONE")
        io.open(pend, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
        code, out = run_sc(E, "close", "-m", "drill")
        check(code != 0 and "Co-Authored-By" in out, "RED: close refuses without the attribution trailers")
        code, out = run_sc(E, "close", "-m", "drill", *T)
        check(code == 0, "GREEN: close commits ({})".format(out.strip().splitlines()[-1][:70] if out.strip() else out))
        names = git(["show", "--format=", "--name-only", "HEAD"], repo).split()
        check(names == ["docs/a.md"], "main close carries exactly docs/a.md (got {})".format(names))
        body = git(["log", "-1", "--format=%B"], repo)
        tr = SC.parse_trailer(body)
        check(tr is not None and tr.get("rows") == "2" and tr.get("still-open") == "1" and tr.get("stale-done") == "1"
              and tr.get("foreign") == "1", "trailer: rows=2 still-open=1 stale-done=1 foreign=1 ({})".format(tr))
        check("Co-Authored-By: Drill" in body and "Claude-Session:" in body, "attribution trailers on the close")
        check(git(["log", "-1", "--format=%s"], repo).startswith("[docs] close: drill"), "subject carries the registration prefix")
        staged = git(["diff", "--cached", "--name-only"], repo).split()
        check(staged == ["docs/b.md"], "the neighbour's staged b.md is still staged, untouched (got {})".format(staged))
        check(os.path.isdir(os.path.join(hist, ".git")) and git(["log", "--format=%s", "-1"], hist).startswith("[docs] close:"),
              "history repo committed the snapshot + table")
        check(git(["config", "--local", "user.name"], hist).strip() == "drill", "history repo copied main's identity")
        check(not os.path.exists(pend) and any(f.endswith(".md") for f in os.listdir(os.path.join(hist, "census"))),
              "the verdict table is filed under census/<utc>-<base>.md")
        # The SWEEP reads a.md whole -- INCLUDING while it is touched -- so the line the previous
        # close's diff scoping left alone (a.md:3) is offered. This is the round-17 fix: the old
        # `c not in touched` filter dropped a doc from the candidate list BEFORE any ordering, so a
        # doc touched every close could never be censused whole.
        code, out = run_sc(E, "census")
        check(code == 0 and "base (main)" in out and "[trailer]" in out, "second census bases on the trailer commit")
        _, rows2 = SC.read_table(pend)
        check(rows2 and all(not r["verdict"] for r in rows2) and any(r["line"] == 3 for r in rows2),
              "the sweep later reads the whole doc: a.md:3 is offered ({} rows, lines {})".format(
                  len(rows2), sorted(r["line"] for r in rows2)))
        # the ratchet: grow the reading order and add ONE labelled line, census, verdict, close -> REFUSE
        w("CLAUDE.md", CLAUDE + "3. a third entry that grows the reading order\n")
        w("docs/a.md", "# A\n\n**Status:** OPEN (commit pending)\n\n"
                       "- **PARTIAL** rework, uncommitted\n"
                       "- **DONE** -- see `zz_gone_file.cpp:12` for the site\n"
                       "- **BUILT** one more claim\n")
        code, out = run_sc(E, "census", "--force", "-k", "0")
        check(code == 0, "a re-census after an edit re-pins ({})".format(out.strip()[-60:]))
        t = io.open(pend, encoding="utf-8").read()
        lines = t.split(chr(10))
        _, rows3 = SC.read_table(pend)
        check(len(rows3) == 1 and rows3[0]["label"] == "BUILT",
              "only the ONE label this edit added is charged ({} rows: {})".format(
                  len(rows3), [(r["line"], r["label"]) for r in rows3]))
        set_verdict(1, "STILL TRUE")
        io.open(pend, "w", encoding="utf-8", newline=chr(10)).write(chr(10).join(lines))
        code, out = run_sc(E, "close", "-m", "drill two", *T)
        check(code != 0 and "ratchet" in out and "ro-bytes" in out,
              "RED: the ratchet refuses a grown reading order (exit {}: {})".format(code, out.strip()[-240:]))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def drill_cross_session(root=None):
    """The three cross-session holes a post-ship audit found on 2026-09-03, each shown RED."""
    print("-- D. cross-session and content-pin refusals")
    root = tempfile.mkdtemp(prefix="scx_")
    repo, mem, hist = (os.path.join(root, n) for n in ("repo", "memory", "history"))
    os.makedirs(os.path.join(repo, "docs"))
    os.makedirs(mem)
    try:
        git(["init", "-q", "-b", "main", "."], repo)
        git(["config", "--local", "user.name", "drill"], repo)
        git(["config", "--local", "user.email", "drill@example"], repo)
        w = lambda rel, text: io.open(os.path.join(repo, rel), "w", encoding="utf-8", newline="\n").write(text)
        w(".gitignore", "CLAUDE.md\n")
        w("docs/a.md", "# A\n\n**Status:** OPEN (commit pending)\n")
        io.open(os.path.join(mem, "MEMORY.md"), "w", encoding="utf-8").write("# index\n")
        git(["add", "--", ".gitignore", "docs/a.md"], repo)
        git(["commit", "-q", "-m", "base"], repo)
        E = (repo, mem, hist)
        T = ["--trailer", "Co-Authored-By: D <d@e>", "--trailer", "Claude-Session: https://e/x"]
        w("docs/a.md", "# A\n\n**Status:** OPEN (commit pending)\n\n- **DONE** mine, added this session\n")
        # the neighbour marks a brand-new doc intent-to-add: invisible to the index guard AND to
        # `ls-files --others`, so without the fix it is owned as a tracked doc and published by us
        w("docs/neighbour_new.md", "# theirs\n\nin progress\n")
        git(["add", "-N", "--", "docs/neighbour_new.md"], repo)
        code, out = run_sc(E, "census", "--since", "2099-01-01", "-k", "0")
        check(code == 0, "census runs with an intent-to-add file present")
        pend = os.path.join(hist, "census", "pending.md")
        t = io.open(pend, encoding="utf-8").read().split("\n")
        for i, l in enumerate(t):
            if l.startswith("| 1 |"):
                cells = l.rstrip().rstrip("|").split("|")
                cells[-1] = " STILL OPEN "
                t[i] = "|".join(cells) + "|"
        io.open(pend, "w", encoding="utf-8", newline="\n").write("\n".join(t))
        code, out = run_sc(E, "close", "-m", "drill", *T)
        check(code != 0 and "neighbour_new.md" in out and "--new" in out,
              "RED: an intent-to-add doc is treated as NEW and refuses the close ({})".format(out.strip()[-90:]))
        git(["reset", "-q", "--", "docs/neighbour_new.md"], repo)
        os.remove(os.path.join(repo, "docs", "neighbour_new.md"))
        # a second census must not silently discard the first hand's verdicts
        code, out = run_sc(E, "census", "--since", "2099-01-01", "-k", "0")
        check(code != 0 and "hand verdict" in out,
              "RED: a second census refuses to overwrite held verdicts ({})".format(out.strip()[-80:]))
        # content pinned at census time: an edit after it (anyone's) refuses the close
        w("docs/a.md", "# A\n\n**Status:** OPEN (commit pending)\n\nedited after the census\n")
        code, out = run_sc(E, "close", "-m", "drill", *T)
        check(code != 0 and "changed since the census" in out,
              "RED: a doc edited after the census refuses the close")
        code, out = run_sc(E, "census", "--since", "2099-01-01", "--force", "-k", "0")
        check(code == 0, "--force re-censuses and re-pins ({})".format(out.strip()[-80:]))
        t = io.open(pend, encoding="utf-8").read().split("\n")
        for i, l in enumerate(t):
            if l.startswith("| ") and not l.startswith("| # ") and not l.startswith("|---"):
                cells = l.rstrip().rstrip("|").split("|")
                if cells[-1].strip() == "":
                    cells[-1] = " STILL OPEN "
                    t[i] = "|".join(cells) + "|"
        io.open(pend, "w", encoding="utf-8", newline="\n").write("\n".join(t))
        code, out = run_sc(E, "close", "-m", "drill", *T)
        check(code == 0, "GREEN: the close commits the pinned bytes ({})".format(out.strip()[-70:]))
        hidx = os.path.join(hist, ".git", "docs_census.index")
        check(not os.path.exists(hidx), "the history repo's private index is removed after the close")
        check(git(["status", "--porcelain"], hist).strip() == "",
              "the history repo's shared index is left clean")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main():
    drill_grammar()
    drill_negated_labels()
    drill_vocab_markers()
    drill_lexer()
    drill_close()
    drill_cross_session()
    print("status_census_drill: {} check(s) failed".format(len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
