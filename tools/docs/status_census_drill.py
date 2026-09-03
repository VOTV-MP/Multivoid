#!/usr/bin/env python3
"""status_census_drill -- show tools/docs/status_census.py RED before trusting it green.

Three fixtures, each a class the design claims to hold (docs/DOCUMENTIZE_ARC.md WP-1):

  A  the LABEL GRAMMAR on the REAL lines of the 2026-09-02 staleness sample (section 2.3): the six
     stale-open lines must produce a row (four as labels with their sub-state clause, one as a
     heading, one through a DEAD PATH token on a non-label heading), and the eight vocabulary
     false positives must produce none. Recall AND precision, on real text, not a synthetic RED.
  A4 the CITATION CONTENT rung at its two strengths: a QUOTE the file does not carry is DEAD and
     refuses a close, while a SYMBOL found elsewhere in the cited file only raises a row. Five
     false-positive shapes, each measured on the real corpus first, must all stay silent.
  B  the COMMENT-ONLY lexer: a code change behind a quoted `#` or `//` is CODE; a trailing-comment
     rewrite on an unchanged declaration is comment-only (the /qf round 6 Q4 lines).
  E  the RESOLVED LEDGER: acting on a verdict erases the line it named, so the verdict is
     appended to an append-only ledger first; the close's verdict columns still read zero (they
     describe the committed text) while `resolved=`/`flips=` carry the correction. Control: a
     verdict that stops carrying because its DOC left the radius is not recorded.
  F  the reading order's own POINTERS (MEMORY.md's links + date globs, CLAUDE.md's paths) and
     the generated dated index -- with controls for the three shapes that only LOOK dead: a live
     glob over dated files, a directory, and a path written from the source root.
  G  MOVE-THEN-CUT on the reading order, made checkable: a clause that LEFT and is findable
     somewhere moved; one findable nowhere was destroyed and is named. Plus the two readings of
     "coverage" -- clauses, not the symbols both texts happen to mention.
  H  every DECLARED trailer column has a PRODUCER -- the check that would have caught a ratchet
     hardcoded to 0 and a column no close has ever emitted, both of which read as a plausible 0.
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


def drill_drift_token_refusal():
    """The seventh token BINDS, not merely exists. Asserting `DRIFT_VERDICT in VERDICTS` says the
    vocabulary grew; it says nothing about the refusal, and a token nothing enforces leaves the hand
    free to answer drift with `NOT A LABEL` exactly as before -- which is the defect the token was
    added to close. This is the same shape as the `memref-dead` ratchet that was drilled only at 0."""
    root = tempfile.mkdtemp(prefix="scd_")
    repo, mem, hist = (os.path.join(root, n) for n in ("repo", "memory", "history"))
    os.makedirs(os.path.join(repo, "docs"))
    os.makedirs(os.path.join(repo, "src"))
    os.makedirs(mem)
    try:
        w = lambda rel, text: io.open(os.path.join(repo, rel), "w", encoding="utf-8",
                                      newline="\n").write(text)
        git(["init", "-q", "-b", "main", "."], repo)
        git(["config", "--local", "user.name", "drill"], repo)
        git(["config", "--local", "user.email", "drill@example"], repo)
        w(".gitignore", "CLAUDE.md\n")
        w("CLAUDE.md", CLAUDE)
        w("src/thing.cpp", "\n".join(["// head"] + ["// pad {}".format(i) for i in range(2, 60)] +
                                     ["void MovedSymbol() {}"]) + "\n")
        w("docs/x.md", "# X\n\nthe seam is `src/thing.cpp:5` `MovedSymbol` today\n")
        io.open(os.path.join(mem, "MEMORY.md"), "w", encoding="utf-8").write("# i\n- a\n")
        git(["add", "--", ".gitignore", "docs/x.md", "src/thing.cpp"], repo)
        git(["commit", "-q", "-m", "base"], repo)
        E = (repo, mem, hist)
        code, out = run_sc(E, "census", "--since", "2099-01-01")
        pend = os.path.join(hist, "census", "pending.md")
        _, rows = SC.read_table(pend)
        check(len(rows) == 1 and rows[0]["kind"] == "drift",
              "the fixture yields exactly one DRIFT row ({})".format([(r["kind"], r["line"]) for r in rows]))

        def set_all(v):
            t = io.open(pend, encoding="utf-8").read().split("\n")
            for i, l in enumerate(t):
                if l.startswith("| ") and not l.startswith("| # ") and not l.startswith("|---"):
                    c = [x.strip() for x in l.strip().strip("|").split("|")]
                    if len(c) >= 11 and c[0].isdigit():
                        cells = l.rstrip().rstrip("|").split("|")
                        cells[-1] = " " + v + " "
                        t[i] = "|".join(cells) + "|"
            io.open(pend, "w", encoding="utf-8", newline="\n").write("\n".join(t))

        T = ["--trailer", "Co-Authored-By: Drill <d@e>", "--trailer", "Claude-Session: https://example/x"]
        for tok in ("STILL TRUE", "NOT A LABEL"):
            set_all(tok)
            code, out = run_sc(E, "close", "-m", "drift", *T)
            check(code != 0 and "drift rows" in out,
                  "RED: '{}' on a DRIFT row refuses -- the token BINDS, it does not merely exist".format(tok))
        # ...and the other direction: the seventh token on a row that is NOT a drift row
        w("docs/x.md", "# X\n\nthe seam is `src/thing.cpp:5` `MovedSymbol` today\n\n- **DONE** a plain claim\n")
        run_sc(E, "census", "--force", "--since", "2099-01-01")
        set_all(SC.DRIFT_VERDICT)
        code, out = run_sc(E, "close", "-m", "drift", *T)
        check(code != 0 and "drift rows" in out,
              "RED: '{}' on a NON-drift row refuses too -- a one-sided gate is half a gate".format(
                  SC.DRIFT_VERDICT))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def drill_content_rung():
    """A4: the CONTENT rung. A line number is a POSITION and the claim is about CONTENT -- but the
    two ways a doc says WHICH content are not equally certain, so they have different strengths and
    each of the four false-positive shapes measured on the real corpus gets its own arm."""
    print("-- A4. the citation content rung (quote = dead, symbol = advisory)")
    d = tempfile.mkdtemp(prefix="scq_")
    try:
        os.makedirs(os.path.join(d, "src"))
        w = lambda rel, text: io.open(os.path.join(d, rel), "w", encoding="utf-8", newline="\n").write(text)
        # a source file whose cited fact has MOVED, and whose comment WRAPS mid-sentence
        w("src/thing.cpp", "\n".join([
            "// thing.cpp -- the header",                      # 1
            "//",                                              # 2
            "// Gameplay layer. The COLOR AXIS has ONE owner: this",   # 3
            "// module (see the note). Everything else reads it.",     # 4
            "int filler_a() { return 0; }",                    # 5
        ] + ["// pad {}".format(i) for i in range(6, 60)] + [
            "void MovedSymbol() {}",                           # 60
            "void CommonSymbol() {}",                          # 61
            "int CommonSymbol_use() { return 0; }",            # 62
        ]) + "\n")
        env = SC.Env(repo=d, memory=os.path.join(d, "m"), history=os.path.join(d, "h"))
        R = SC.Resolver(env)

        def st(line):
            return [t for t in R.tokens(line) if not t[0].startswith("`")]

        # 1. the QUOTE rung, hard: the words are not in the file at all -> DEAD, refuses a close
        got = st('`src/thing.cpp:3` says "The COLOR AXIS has TWO owners: this module and more"')
        check(got and got[0][1] == "content-gone" and SC.dead_cites(got),
              "quote absent from the file -> content-gone, and it IS a dead citation ({})".format(got))
        # 2. ...and the SAME check must not fire on a quote that is really there but WRAPS, with its
        #    second line carrying its own `//`. This is the shape that produced the corpus's only
        #    quote-rung hit, and it was a false positive.
        got = st('`src/thing.cpp:3` says "The COLOR AXIS has ONE owner: this module"')
        check(got and got[0][1] == "ok",
              "a quote WRAPPING across two comment lines is found, not called dead ({})".format(got))
        # 3. the SYMBOL rung, soft: unique, far away -> drift, naming the repair, NOT dead
        got = st("the seam is `src/thing.cpp:5` `MovedSymbol` today")
        check(got and got[0] == ("src/thing.cpp:5->60", "drift") and not SC.dead_cites(got),
              "a unique symbol 55 lines away -> drift, the true line named, and NOT a dead citation ({})".format(got))
        rows = SC.scan_lines("t.md", ["intro", "the seam is `src/thing.cpp:5` `MovedSymbol` today"], R)
        check(len(rows) == 1 and rows[0]["kind"] == "drift",
              "...and it still reaches the hand: the line becomes a row of its own kind ({})".format(
                  [(r["kind"], r["line"]) for r in rows]))
        # THE SEVENTH TOKEN. A drift row makes no status claim, so none of the five status verdicts
        # fits it -- and the skill first told the hand to answer it NOT A LABEL, which would have put
        # the symbol rung's false positives into the counter the gate declares to be the LABEL
        # GRAMMAR's precision (DIFF pass, round 1 Q1; 31 corpus drift rows). The token is refused in
        # BOTH directions, or the two instruments' errors merge again by default.
        check(SC.DRIFT_VERDICT in SC.VERDICTS and SC.DRIFT_VERDICT == "DRIFT OK",
              "the drift rung has its OWN token, so `not-a-label` stays the label grammar's rate")
        drill_drift_token_refusal()
        # 4. FALSE-POSITIVE CONTROLS, one per shape measured on the real corpus
        got = st("`src/thing.cpp:60` `MovedSymbol` is right here")
        check(got and got[0][1] == "ok", "a symbol AT the cited line is ok ({})".format(got))
        got = st("`src/thing.cpp:55-62` covers `MovedSymbol`")
        check(got and got[0][1] == "ok",
              "a RANGE citation is judged over the whole range, not its first line ({})".format(got))
        got = st("(`src/thing.cpp:5` `filler_a`; `src/thing.cpp:61` and `:62` after `MovedSymbol`)")
        check(all(s == "ok" for _, s in got),
              "a symbol belonging to a LATER citation in the same sentence is not paired with an "
              "earlier one ({})".format(got))
        got = st("`src/thing.cpp:5` mentions `CommonSymbol` somewhere")
        check(got and got[0][1] == "ok",
              "a symbol appearing MORE THAN ONCE gives no unambiguous repair, so no drift ({})".format(got))
        got = st("`src/thing.cpp:5` and `NeverInThisFile` are unrelated")
        check(got and got[0][1] == "ok",
              "a symbol absent from the cited file is no evidence at all ({})".format(got))
        # a REGEX quoted in prose is not a path (found by running the real census, 2026-09-03)
        got = st("round 19 killed it: `git grep -l 'src/thing\\.cpp' -- docs/`")
        check(not got, "a regex escape `\\.` in prose is not a dead path citation ({})".format(got))
        got = st("the real file is `src/thing.cpp` though")
        check(got and got[0][1] == "ok", "...while the same path unescaped still resolves ({})".format(got))
    finally:
        shutil.rmtree(d, ignore_errors=True)


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


def drill_resolved():
    """E. the RESOLVED LEDGER (D0). The defect is not that a verdict is wrong -- it is that acting on
    one ERASES it: the fix rewrites the line the verdict names, the hash changes, the carry drops it,
    and the corrected line returns as a fresh row verdicted STILL TRUE. So the close's own trailer
    reported `stale-done=0` on the real run that corrected two memory topics. Both halves are asserted
    here: the verdict columns still read ZERO (they describe the committed text, which is right), and
    the correction is on the record anyway. Plus the false-positive control: a verdict that stops
    carrying because its DOC left the radius was never acted on and must NOT be recorded."""
    print("-- E. the resolved ledger: a verdict retired by the fix it ordered")
    root = tempfile.mkdtemp(prefix="scr_")
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
        w("docs/c.md", "# C\n\n- **DONE** -- shipped whole, see `zz_gone.cpp:9`\n")
        w("docs/d.md", "# D\n\n- **VERIFIED** -- see `zz_also_gone.cpp:4`\n")
        io.open(os.path.join(mem, "MEMORY.md"), "w", encoding="utf-8").write("# index\n- one\n")
        git(["add", "--", ".gitignore", "docs/c.md", "docs/d.md"], repo)
        git(["commit", "-q", "-m", "base"], repo)
        E = (repo, mem, hist)
        T = ["--trailer", "Co-Authored-By: Drill <d@e>", "--trailer", "Claude-Session: https://example/x"]
        pend = os.path.join(hist, "census", "pending.md")

        # census 1: nothing is touched, so BOTH docs arrive through the sweep, read whole
        code, out = run_sc(E, "census", "--since", "2099-01-01")
        check(code == 0, "census runs ({})".format(out.strip().splitlines()[-1][:60] if out.strip() else "no output"))
        _, rows = SC.read_table(pend)
        keys = sorted(r["key"] for r in rows)
        check(keys == ["docs/c.md", "docs/d.md"], "both docs charged one row each (got {})".format(keys))

        def set_verdict(n, v):
            t = io.open(pend, encoding="utf-8").read().split("\n")
            for i, l in enumerate(t):
                if l.startswith("| {} |".format(n)):
                    cells = l.rstrip().rstrip("|").split("|")
                    cells[-1] = " " + v + " "
                    t[i] = "|".join(cells) + "|"
            io.open(pend, "w", encoding="utf-8", newline="\n").write("\n".join(t))

        nc = [r["n"] for r in rows if r["key"] == "docs/c.md"][0]
        nd = [r["n"] for r in rows if r["key"] == "docs/d.md"][0]
        set_verdict(nc, "STALE DONE")
        set_verdict(nd, "STALE DONE")
        check(SC.resolved_load(SC.Env(repo, mem, hist)) == [], "the ledger is empty before any fix")

        # ACT on c.md only -- the correction the STALE DONE verdict ordered
        w("docs/c.md", "# C\n\n- **BUILT** -- the transport only; the eject lane is its own row\n")
        # `-k 0`: the sweep is off, so d.md LEAVES the radius while c.md stays (it is touched now)
        code, out = run_sc(E, "census", "--force", "--since", "2099-01-01", "-k", "0")
        check(code == 0 and "resolved: 1 verdict(s) retired" in out,
              "the re-census records the retired verdict and says so ({})".format(
                  [l for l in out.splitlines() if l.startswith("resolved")] or out.strip()[-90:]))
        _, rows2 = SC.read_table(pend)
        check(len(rows2) == 1 and rows2[0]["key"] == "docs/c.md" and not rows2[0]["verdict"],
              "the corrected line returns as a FRESH, unverdicted row -- the erasure this ledger answers")
        led = SC.resolved_load(SC.Env(repo, mem, hist))
        check(len(led) == 1 and led[0]["key"] == "docs/c.md" and led[0]["verdict"] == "STALE DONE",
              "the ledger holds exactly the acted-on verdict ({})".format(
                  [(r["key"], r["verdict"]) for r in led]))
        check(len(led) == 1 and all(r["key"] != "docs/d.md" for r in led),
              "CONTROL: d.md's verdict left the RADIUS, was not acted on, and is NOT recorded")
        # CONTROL 2 (the defect the first version shipped, found in the DIFF pass): c.md's SCOPE
        # changed -- whole on census 1, diff-scoped on census 2 -- so its OTHER rows also vanished
        # from the row set. They must NOT be recorded: the line is still in the file.
        check(all(r["line"] == 3 for r in led),
              "CONTROL: only the line actually EDITED is recorded, not every row the scope change "
              "dropped ({})".format([(r["key"], r["line"]) for r in led]))
        check(bool(led) and "DONE" in led[0].get("label", ""),
              "the record keeps the label it retired ({})".format(led[0].get("label") if led else "-"))

        set_verdict(rows2[0]["n"], "STILL TRUE")
        code, out = run_sc(E, "close", "-m", "the correction", *T)
        check(code == 0, "GREEN: close commits ({})".format(out.strip().splitlines()[-1][:70] if out.strip() else out))
        tr = SC.parse_trailer(git(["log", "-1", "--format=%B"], repo))
        check(tr.get("stale-done") == "0" and tr.get("still-true") == "1",
              "the verdict columns describe the COMMITTED text: stale-done=0 ({})".format(tr))
        check(tr.get("resolved") == "1" and tr.get("flips") == "1",
              "...and the correction is on the record anyway: resolved=1 flips=1 ({})".format(
                  {k: tr.get(k) for k in ("resolved", "flips")}))
        code, out = run_sc(E, "resolved")
        check(code == 0 and "docs/c.md" in out and "1 record(s); naming a defect: 1" in out,
              "the ledger reads back (a capability nothing calls is not shipped): {}".format(out.strip()[-90:]))

        # a SECOND close accumulates, and the delta is printed against the previous trailer
        code, out = run_sc(E, "census")
        _, rows3 = SC.read_table(pend)
        nd = [r["n"] for r in rows3 if r["key"] == "docs/d.md"]
        check(len(nd) == 1, "the sweep reaches d.md again after the close ({} rows)".format(len(rows3)))
        for r in rows3:
            set_verdict(r["n"], "ACTUALLY DONE" if r["key"] == "docs/d.md" else "STILL TRUE")
        w("docs/d.md", "# D\n\n- **BUILT** -- landed, no citation\n")
        code, out = run_sc(E, "census", "--force", "-k", "0")
        check(code == 0 and "resolved: 1 verdict(s) retired to the ledger (ACTUALLY DONE 1)" in out,
              "the second fix retires its own verdict ({})".format(
                  [l for l in out.splitlines() if l.startswith("resolved")] or out.strip()[-90:]))
        _, rows4 = SC.read_table(pend)
        for r in rows4:
            set_verdict(r["n"], "STILL TRUE")
        code, out = run_sc(E, "close", "-m", "the second correction", *T)
        check(code == 0 and "resolved this close: 1 verdict(s), 1 of them naming a defect" in out,
              "the close prints ITS OWN delta, not only the running total ({})".format(
                  [l for l in out.splitlines() if l.startswith("resolved this close")] or out.strip()[-120:]))
        tr = SC.parse_trailer(git(["log", "-1", "--format=%B"], repo))
        check(tr.get("resolved") == "2" and tr.get("flips") == "2",
              "the trailer's totals are CUMULATIVE across closes ({})".format(
                  {k: tr.get(k) for k in ("resolved", "flips")}))
        # RED: roll the ledger back and the close refuses -- the monotone half, at the close's own seam
        io.open(SC.resolved_path(SC.Env(repo, mem, hist)), "w", encoding="utf-8", newline="\n").write("")
        w("docs/c.md", "# C\n\n- **BUILT** -- the transport only; one more line\n- **DONE** again\n")
        code, out = run_sc(E, "census", "--force", "-k", "0")
        _, rows5 = SC.read_table(pend)
        for r in rows5:
            set_verdict(r["n"], "STILL TRUE")
        code, out = run_sc(E, "close", "-m", "rolled back", *T)
        check(code != 0 and "monotone" in out and "append-only" in out,
              "RED: a truncated ledger refuses the close (exit {}: {})".format(code, out.strip()[-160:]))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def drill_memory_index():
    """F: the two index files' own POINTERS. `MEMORY.md` is loaded into every session and CLAUDE.md's
    reading order is the first thing a reset session opens, and nothing gated either -- `lessons_gate`
    fixes its ledger to `docs/LESSONS.md` and never looks at them. `[V]` 2026-09-03 that left SIX of
    MEMORY.md's eleven date globs matching zero files, because 0 of 705 lesson files carry a date in
    the NAME while every `project*` file does."""
    print("-- F. the reading order's own pointers, and the generated dated index")
    import memory_index as MI
    root = tempfile.mkdtemp(prefix="scm_")
    repo, mem = os.path.join(root, "repo"), os.path.join(root, "memory")
    os.makedirs(os.path.join(repo, "docs", "events"))
    os.makedirs(os.path.join(repo, "src", "votv-coop", "include", "coop"))
    os.makedirs(mem)
    try:
        w = lambda base, rel, text: io.open(os.path.join(base, rel), "w", encoding="utf-8",
                                            newline="\n").write(text)
        git(["init", "-q", "-b", "main", "."], repo)
        git(["config", "--local", "user.name", "drill"], repo)
        git(["config", "--local", "user.email", "drill@example"], repo)
        w(repo, "docs/kept.md", "# kept\n")
        w(repo, "src/votv-coop/include/coop/thing.h", "// thing\n")
        w(mem, "lesson-no-date-in-my-name.md",
          "---\nname: lesson-no-date-in-my-name\ndescription: \"a lesson\"\nmetadata:\n"
          "  modified: 2026-08-30T10:00:00.000Z\n---\n\nbody\n")
        w(mem, "project_dated_2026-08-29.md", "---\nname: project_dated_2026-08-29\n---\n\nbody 2026-08-29\n")
        env = SC.Env(repo=repo, memory=mem, history=os.path.join(root, "h"))

        # RED: every pointer shape that can be dead, one of each
        w(mem, "MEMORY.md", "\n".join([
            "# index",
            "- greps: `memory/lesson-*2026-08-30*`",                 # dead: no filename dates
            "- greps: `memory/project_*2026-08-29*`",                # LIVE: project files carry them
            "- [a link](lesson-no-date-in-my-name.md)",              # live
            "- [a dead link](lesson-never-written.md)",              # dead
        ]) + "\n")
        w(repo, "CLAUDE.md", "\n".join([
            "# rules", "", "## Reading order after a session reset / new conversation", "",
            "1. `docs/kept.md` and the `docs/events/` subtree",      # file + DIRECTORY, both live
            "2. `include/coop/thing.h`",                             # named from the SOURCE root
            "3. `docs/never_written.md`",                            # dead
        ]) + "\n")
        dead = MI.dead_refs(env)
        kinds = sorted((f, k, p) for f, k, p in dead)
        check(len(dead) == 3, "RED: three dead pointers of three shapes, and only three ({})".format(kinds))
        # THE WIRING, not just the detector. The first version of this arm asserted only
        # `dead_refs() == []` and `ratchet_values()["memref-dead"] == 0` in the GREEN state -- which a
        # never-computed field satisfies by construction, and that is exactly what shipped: the compute
        # block silently did not apply and the ratchet was a hardcoded 0 that could never fire
        # (found in the DIFF pass, 2026-09-03). A gate is only drilled when it is shown NON-zero.
        check(SC.ratchet_values(env)["memref-dead"] == len(dead),
              "RED: the RATCHET reports the dead pointers too -- {} (a gate shown only at 0 is not "
              "shown at all)".format(SC.ratchet_values(env)["memref-dead"]))
        check(any(k == "glob" and "lesson-" in p for _, k, p in dead),
              "a date GLOB matching zero files is dead -- the shipped defect")
        check(not any("project_" in p for _, _, p in dead),
              "CONTROL: the same glob shape over `project_*` files is NOT dead (they carry dates)")
        check(any(k == "link" and "never-written" in p for _, k, p in dead), "a dead markdown link is caught")
        check(any(k == "path" and "never_written" in p for _, k, p in dead), "a dead docs path is caught")
        check(not any(p.endswith("events/") for _, _, p in dead),
              "CONTROL: a DIRECTORY is a resolved pointer, not a dead one")
        check(not any("thing.h" in p for _, _, p in dead),
              "CONTROL: a path written from the SOURCE root resolves")
        # GREEN: fix the three, and the gate goes to zero
        w(mem, "MEMORY.md", "# index\n- `INDEX_BY_DATE.md` -> 08-30\n- [a link](lesson-no-date-in-my-name.md)\n")
        w(repo, "CLAUDE.md", "# rules\n\n## Reading order after a session reset / new conversation\n\n"
                             "1. `docs/kept.md` and the `docs/events/` subtree\n")
        check(MI.dead_refs(env) == [], "GREEN: the repaired pointers leave none")
        check(SC.ratchet_values(env)["memref-dead"] == 0, "...and the ratchet reads it as 0")

        # the index itself: dated by the LADDER, so a file with no date in its name still lands
        path, changed = MI.write(env)
        body = io.open(path, encoding="utf-8").read()
        check(changed and "## 2026-08-30" in body and "lesson-no-date-in-my-name" in body,
              "the index dates a name-less lesson from its frontmatter")
        check("## 2026-08-29" in body and "project_dated_2026-08-29" in body, "...and a dated project file")
        check(body.startswith(SC.VOCAB_MARKER_DOC),
              "the index carries the doc-scope vocabulary marker (it QUOTES descriptions)")
        check(SC.scan_lines("memory/" + MI.INDEX_NAME, body.split("\n"), SC.Resolver(env)) == [],
              "...so the census reads ZERO claims from it, however many status words it reprints")
        check(MI.write(env)[1] is False, "a second run is idempotent -- an unchanged index is not rewritten")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def drill_trailer_producers():
    """H: every declared trailer column has a PRODUCER. The schema's own kinds describe who READS a
    column -- RATCHETED, GATED, REPORTED ("printed and never enforced") -- and none of them describes
    a column nothing WRITES. `[V]` 2026-09-03 the DIFF pass found two at once: `memref-dead`, whose
    compute block silently failed to apply so the ratchet was a hardcoded 0, and `running-totals`,
    declared since the schema was written and emitted by no close in its life. A drill that only
    checks values cannot see either, because both read as a plausible 0."""
    print("-- H. every declared trailer column has a producer")
    import trailer_schema as TS
    src = io.open(os.path.join(HERE, "status_census.py"), encoding="utf-8").read()
    # An ASSIGNMENT anywhere in the module, not only inside `ratchet_values`: `accretion` is computed
    # at the two CALL SITES (`rv["accretion"] = accretion_count(...)`), which is legitimate, and a
    # body-scoped check reports it as unproduced. The property is "something writes this", not "it is
    # written HERE" -- narrowing the scope narrows the invariant into a site list.
    unwritten = [c for c in TS.RATCHETED
                 if 'vals["{}"] ='.format(c) not in src and 'rv["{}"] ='.format(c) not in src]
    check(not unwritten,
          "every RATCHETED column is ASSIGNED somewhere, not merely initialised to 0 ({})".format(
              unwritten or "none missing"))
    # and the close must actually put each one in the trailer it writes
    vals_src = src[src.index("    vals = {\"base\":"):src.index("    # 6. commit 3 first")]
    absent = [c for c in TS.RATCHETED + TS.MONOTONE + TS.GATED
              if '"{}":'.format(c) not in vals_src]
    check(not absent, "every RATCHETED / MONOTONE / GATED column reaches the trailer ({})".format(
        absent or "none missing"))
    # a REPORTED column may legitimately be unwritten ONLY if nothing declares it -- which is a
    # contradiction, so the same rule applies; this is the check that would have caught running-totals
    rep_absent = [c for c in TS.REPORTED if '"{}":'.format(c) not in vals_src]
    check(not rep_absent, "every REPORTED column is produced too -- 'nothing reads it' is a decision, "
                          "'nothing writes it' is a defect ({})".format(rep_absent or "none missing"))


def drill_reading_order():
    """G: MOVE-THEN-CUT, made checkable. A shrink of the reading order is only good news if the facts
    went somewhere, and `[V]` 2026-09-03 that is not the usual case: NO entry's clauses are more than
    11 % present in the doc it points at, so the reading order is the ONLY copy of most of what it
    says and every byte of the ~37 KB still owed is a MOVE. The two must be distinguishable."""
    print("-- G. the reading order: a move and a cut are not the same shrink")
    import reading_order as RO
    root = tempfile.mkdtemp(prefix="scr2_")
    os.makedirs(os.path.join(root, "docs"))
    try:
        w = lambda rel, text: io.open(os.path.join(root, rel), "w", encoding="utf-8",
                                      newline="\n").write(text)
        head = "# rules\n\n## Reading order after a session reset / new conversation\n\n"
        A = "1. `docs/dest.md` -- the first entry, whose long sentence is a claim worth keeping here.\n"
        B = "2. a second entry whose long sentence is about to be deleted outright by a careless hand.\n"
        C = "3. a third entry whose long sentence stays exactly where it has always been, untouched.\n"
        U = ('4. USER 2026-09-03, verbatim: "this line is a record of what I said and never moves"\n')
        prev = head + A + B + C + U
        w("CLAUDE.md", prev)
        w("docs/dest.md", "# dest\n\nnothing here yet.\n")

        # (1) entry A is MOVED: gone from the order, present in its destination
        w("docs/dest.md", "# dest\n\n" + A.split(". ", 1)[1])
        now = head + "1. `docs/dest.md` -- moved, see there.\n" + B + C + U
        w("CLAUDE.md", now)
        moved, cut, lost = RO.moved_and_cut(root, prev, now)
        check(len(moved) == 1 and len(cut) == 0,
              "a MOVE is seen as a move: {} moved, {} cut".format(len(moved), len(cut)))

        # (2) entry B is CUT: gone from the order and findable nowhere
        now2 = head + "1. `docs/dest.md` -- moved, see there.\n" + C + U
        w("CLAUDE.md", now2)
        moved, cut, lost = RO.moved_and_cut(root, prev, now2)
        check(len(moved) == 1 and len(cut) == 1 and "deleted outright" in cut[0][1],
              "a CUT is NOT counted as a move, and the destroyed claim is named ({})".format(
                  [c[1][:40] for c in cut]))
        # (3) the untouched entry is neither
        check(all("has always been" not in raw for _, raw in moved + cut),
              "an untouched clause is neither moved nor cut")
        # (4) the USER-verbatim line is exempt from the coverage report, so a shrink cannot be
        #     justified by "it is covered" on a line that records what the user said
        rows = RO.coverage(root)
        ex = sum(r[5] for r in rows)
        check(ex == 1, "the USER + verbatim line is EXEMPT, and exactly one line is ({})".format(ex))
        # (4b) THE EXEMPTION AT CLOSE TIME. The module's header claimed a USER clause is "never moved
        #      and never cut" while the only consumer of the rule was `coverage()`, which the close
        #      never calls -- so at the one moment it mattered nothing enforced it (round 1 Q4).
        #      Losing one is its own bucket: never counted as a move, never buried among the cuts.
        now3 = head + "1. `docs/dest.md` -- moved, see there.\n" + B + C
        w("CLAUDE.md", now3)
        moved3, cut3, lost3 = RO.moved_and_cut(root, prev, now3)
        check(len(lost3) == 1 and "never moves" in lost3[0][1],
              "RED: an EXEMPT clause that left is reported LOST, on its own ({})".format(
                  [l[1][:40] for l in lost3]))
        check(all("never moves" not in raw for _, raw in moved3 + cut3),
              "...and it is in NEITHER the moved nor the cut bucket")
        # (4c) the quotation may WRAP to the next line -- 3 real entries do this
        wrapped = ["1. `docs/dest.md` -- USER RULE 2026-09-03: the decision that follows is theirs",
                   '   and they put it this way: "the wrapped quotation lands on the second line"']
        cl = RO.clauses(wrapped)
        check(cl and all(c[3] for c in cl),
              "a USER line whose quotation wraps to the NEXT line is still exempt ({})".format(
                  [(c[0][:28], c[3]) for c in cl]))
        # (5) coverage is measured on CLAUSES, not on the symbols both texts happen to mention --
        #     the reading that made the handed-down 94 % and would have licensed cutting 136 claims
        w("docs/dest2.md", "# d2\n\nwe also discuss `SomeSymbol` and `OtherSymbol` at length here.\n")
        w("CLAUDE.md", head + "1. `docs/dest2.md` -- a long sentence about `SomeSymbol` and "
                              "`OtherSymbol` that this destination does not actually contain.\n")
        r = RO.coverage(root)[0]
        check(r[3] >= 1 and r[4] == 0,
              "sharing SYMBOLS with the destination is not coverage: {} clause(s), {} covered".format(
                  r[3], r[4]))
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
    drill_content_rung()
    drill_lexer()
    drill_close()
    drill_resolved()
    drill_memory_index()
    drill_trailer_producers()
    drill_reading_order()
    drill_cross_session()
    print("status_census_drill: {} check(s) failed".format(len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
