#!/usr/bin/env python3
"""status_census -- the /documentize close as a computed set, a recorded number and a script-owned commit.

WHY THIS EXISTS (docs/DOCUMENTIZE_ARC.md, section 2)
-----------------------------------------------------
The skill's Step 0.5 ordered a HAND check of every status marker in the tree -- 11,291 grep
hits per run, most of them not labels -- and Step 1 ordered every doc read (17.4 MB), at ~8
closes per active day. No run ever did it, and 148 of 281 close-commit bodies said
"reconciled" anyway. A mandate nothing observes is satisfied by assertion. The rot that DOES
exist sits in old point-in-time docs the session never touched, in a subordinate fact under a
true label (a line number, a count, "(commit pending)"), and in trees no repository tracks.

WHAT IT DOES (section 3, WP-1)
-------------------------------
  census   compute the READ SET (every *.md main tracks, every *.md under docs/ tracked or
           not, CLAUDE.md, research/findings/, the memory directory), decide each path's
           OWNER (main / the inner research repo / nobody -> the private history), compute
           the RADIUS -- (i) docs touched since the last close per owner, (ii) docs citing a
           SPECIFIC symbol or path of the code diff (cited by <= 5 docs), (iii) the K oldest-
           censused docs -- and scan those docs with the LABEL GRAMMAR into a row table with
           an EMPTY verdict column, written to the private history's working tree.
  (hand)   fill the verdict column: STILL OPEN / ACTUALLY DONE / STALE DONE / PARTIAL /
           STILL TRUE. The judgment is the hand's; its EXISTENCE is checked.
  close    refuse until every row carries one token (and no STILL TRUE / ACTUALLY DONE rests
           on a citation the mechanical column resolved gone); then THREE commits -- the
           private history (snapshot + state + the verdict table), the inner research repo,
           and main -- each from a PRIVATE index so nothing another session staged in the
           shared index is swallowed or discarded, each carrying ONE machine-written
           `Docs-Census:` trailer plus the attribution trailers the caller MUST pass.
  --sweep  the whole read set instead of the radius (a census, not a sample).
  --loose  add the skill's loose vocabulary regex as an extra row source.
  resolved read back the RESOLVED LEDGER: every verdict retired because the line it named was
           FIXED. The verdict columns describe the text being committed, so a corrected claim
           leaves them reading zero; this is where the correction is recorded.

Nothing here is prose the agent asserts: the sets are computed and printed, the numbers go
into commit trailers that tools/docs/docs_census_gate.py checks against each other in CI, and
the trees CI cannot see get their history in ~/.claude/projects/<slug>/history/ (local-only,
no remote -- the research/ pattern).

USAGE
    python tools/docs/status_census.py census [--since DATE] [--sweep] [--loose] [-k N]
    python tools/docs/status_census.py close -m "<subject>" \\
        --trailer "Co-Authored-By: ..." --trailer "Claude-Session: ..." [--new docs/X.md]...
    python tools/docs/status_census.py snapshot          # sync the private history only
    python tools/docs/status_census.py show               # print the pending table + state
    python tools/docs/status_census.py resolved [--since UTC] [--verdict "STALE DONE"]
"""
import argparse
import datetime as _dt
import io
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lessons_gate as LG  # noqa: E402  -- build_corpora / CITE_ROOTS (same directory)
from status_grammar import *  # noqa: E402,F401,F403  -- the label grammar, the Resolver, scan_doc, read_text, sha1
from status_grammar import (read_text, sha1, DATE_RE, CORR_RE, ACCRETION_RE, DATED_NAME_RE,  # noqa: E402
                            drift_cites, line_hashes,  # noqa: E402
                            STATUS_WORDS, COMPLETION_WORDS, OPEN_WORDS, VOCAB_MARKER)  # noqa: E402
from comment_lexer import comment_only  # noqa: E402
import trailer_schema as TS  # noqa: E402  -- the ONE trailer column vocabulary (census + gate)
from census_git import git, gitz  # noqa: E402  -- the git primitive both layers share
from census_history import (utc_now, history_init, snapshot_sync, state_load, state_save,  # noqa: E402
                            resolved_path, resolved_load, resolved_append, resolved_counts,  # noqa: E402
                            retired_verdicts, FLIP_VERDICTS)  # noqa: E402

K_DEFAULT = 40
ROW_BUDGET = 40          # the SWEEP's own row budget: the hand check is paid per ROW, so that is the
                         # unit the sweep amortises over -- the session's own rows are always included
# The sixth token is not a status: it is the hand REJECTING the row. The grammar has a measured false
# positive rate (2026-09-02 sample: 29 % of the old regex's hits were not labels at all) and the five
# status verdicts have no value for "this line is not a claim" -- so without it a false positive must be
# verdicted STILL TRUE, which launders noise into the counts and hides the grammar's precision. With it,
# every close MEASURES that precision (`not-a-label=` in the trailer) instead of asserting it.
VERDICTS = ("STILL OPEN", "ACTUALLY DONE", "STALE DONE", "PARTIAL", "STILL TRUE", "NOT A LABEL")
# The sixth token is not a status: it is the hand REJECTING the row. Without it a false positive
# must be verdicted STILL TRUE, which launders noise into the counts and hides the grammar's
# precision; with it, every close MEASURES that precision instead of asserting it.
#
# AND IT IS ATTRIBUTED BY THE RUNG THAT PRODUCED THE ROW, which is the rule a seventh token was
# briefly mistaken for. `not-a-label=` is declared -- in the gate, in the skill, in this file --
# to be the LABEL GRAMMAR's false-positive rate, but rows arrive from FOUR instruments: the label
# grammar, the citation resolver (`cite`), the symbol content rung (`drift`) and the opt-in loose
# regex. `[V]` 2026-09-03: 16 of one census's 87 rows were `cite` rows, so a single counter was
# already three instruments' errors added together (DIFF pass, round 2 Q4). The HAND still writes
# one token -- asking it to pick the right synonym per rung is bookkeeping the machine can do --
# and the MACHINE splits the count. A separate `DRIFT OK` token shipped hours earlier and is
# retired here whole (RULE 2): for a drift row "the pairing was a coincidence" and "this row is
# not a claim" are the same sentence, so it was a second mechanism for one concept.
LABEL_BUCKET = {"cite": "not-a-cite", "drift": "drift-ok", "loose": "not-loose"}
# The four that assert something about a STATUS. They are meaningless on a row that carries
# no label, which is every kind in LABEL_BUCKET above.
STATUS_VERDICTS = ("STILL OPEN", "ACTUALLY DONE", "STALE DONE", "PARTIAL")


CLOSE_PREFIX = "[docs] close:"
TRAILER_KEY = "Docs-Census"
RATCHET_COLS = TS.RATCHETED
TARGETS = TS.TARGETS
OWN_TOOLING = ("tools/docs/", ".claude/skills/")
ACCRETION_EXCLUDE = ("docs/LESSONS.md", "docs/DOCUMENTIZE_ARC.md", "docs/QF_ARC.md")


# ----------------------------------------------------------------------------- environment
class Env:
    """Every path the script touches, overridable for the drill (never hardcoded twice)."""

    def __init__(self, repo=None, memory=None, history=None):
        self.repo = os.path.abspath(repo or os.environ.get("MULTIVOID_REPO") or
                                    os.path.dirname(os.path.dirname(HERE)))
        slug = re.sub(r"[^A-Za-z0-9]", "-", self.repo)
        base = os.path.join(os.path.expanduser("~"), ".claude", "projects", slug)
        self.memory = os.path.abspath(memory or os.environ.get("MULTIVOID_MEMORY_DIR") or
                                      os.path.join(base, "memory"))
        self.history = os.path.abspath(history or os.environ.get("MULTIVOID_HISTORY_DIR") or
                                       os.path.join(base, "history"))
        self.owned = discover_owned_repos(self.repo)
        r = os.path.join(self.repo, "research")
        self.research = r if r in self.owned else None


def discover_owned_repos(repo, depth=4):
    """-> [abs path] of the inner repositories that are OURS, newest-first by nothing in particular.

    OWNERSHIP IS THE LOCAL GIT IDENTITY, not the absence of a remote (round 15 of the /qf pass).
    `[V]` 2026-09-03: eleven inner `.git` dirs exist to depth 4 and the local `user.email` separates
    them perfectly -- `research` and `site` carry `pelmentr@gmail.com` like main, and all eight
    vendored trees (mtasa-blue, RE-UE4SS, Relay, baritone, VoiceChatMC/simple-voice-chat, SourceIO,
    unrealpak, imgui1928) carry none. That is the CLAUDE.md git-identity rule, which `history_repo`
    already enforces when it mints the snapshot store -- so the census reads the same invariant rather
    than a second heuristic. "No remote" was the first test tried and it inverts the day `site/` is
    pushed, which it is meant to be."""
    mail = git(["config", "--local", "user.email"], repo, check=False).strip()
    if not mail:
        return []
    out = []
    for dirpath, dirnames, _ in os.walk(repo):
        rel = os.path.relpath(dirpath, repo)
        if rel != "." and rel.count(os.sep) + 1 > depth:
            dirnames[:] = []
            continue
        if ".git" in dirnames and dirpath != repo:
            m = git(["config", "--local", "user.email"], dirpath, check=False).strip()
            if m == mail:
                out.append(dirpath)
            dirnames.remove(".git")
    return out



# ----------------------------------------------------------------------------- the read set
def read_set(env):
    """-> {key: (owner, abspath)}; owner is `main`, an OWNED inner repo's basename, or `private`.

    Ownership is LOCATION + IGNORE RULES, not `git add` state (round 6, Q2): a path inside a
    repository's tree that its .gitignore does not exclude belongs to that repository, tracked
    or not. What no repository can hold (ignored, or outside every tree) is private. WHICH inner
    repos count is `discover_owned_repos`' local-git-identity test (round 15).

    `_archive/` is NOT in the read set: an archived doc's labels are historical BY DEFINITION -- that
    is what archiving MEANS -- and leaving them in made 26 docs / 185 rows permanent sweep candidates
    while an archive MOVE re-keyed a row with nothing updating (round 7, Q3)."""
    out = {}
    repo = env.repo
    pending_add = intent_to_add(repo)
    for p in git(["ls-files", "-z", "--", "*.md"], repo).split("\0"):
        if p and p not in pending_add:      # an intent-to-add entry is a NEW file, not a tracked one
            out[p] = ("main", os.path.join(repo, p))
    docs_dir = os.path.join(repo, "docs")
    all_docs = []
    for dirpath, dirnames, filenames in os.walk(docs_dir):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for f in filenames:
            if f.endswith(".md"):
                all_docs.append(os.path.relpath(os.path.join(dirpath, f), repo).replace("\\", "/"))
    for p in check_ignore(repo, [d for d in all_docs if d not in out]):
        out[p] = ("private", os.path.join(repo, p))
    claude = os.path.join(repo, "CLAUDE.md")
    if os.path.isfile(claude) and "CLAUDE.md" not in out:
        out["CLAUDE.md"] = ("private", claude)
    # Every OWNED inner repo, WHOLE -- not `research/findings` alone. `[V]` 2026-09-03: the old walk
    # left 77 non-archive tracked `research/*.md` (almost all `handson_runbook_*`, which the skill's
    # own Step 2 orders you to author, and which 17 docs cite by name) permanently outside the read
    # set, and `site/` -- which holds the PUBLIC website copy -- entirely invisible.
    for owned in env.owned:
        name = os.path.basename(owned)
        rel = []
        for dirpath, dirnames, filenames in os.walk(owned):
            dirnames[:] = [d for d in dirnames if d != ".git"]
            for f in filenames:
                if f.endswith(".md"):
                    rel.append(os.path.relpath(os.path.join(dirpath, f), owned).replace("\\", "/"))
        # An IGNORED .md in an owned repo is NOT that repo's document -- it is a generated artifact.
        # `[V]` 2026-09-03: research holds 1,251 `.md` of which **869 are git-ignored and ZERO of
        # those are under `findings/`** (pak dumps, bp_reflection output), so the old findings-only
        # walk never classified one as private, while a whole-tree walk that kept them tripled the
        # read set. Tracked-or-untracked-but-not-ignored is the same "location + ignore rules" test
        # the rest of the read set uses.
        ignored = set(check_ignore(owned, rel))
        for p in rel:
            if p not in ignored:
                out[name + "/" + p] = (name, os.path.join(owned, p))
    if os.path.isdir(env.memory):
        for f in sorted(os.listdir(env.memory)):
            if f.endswith(".md"):
                out["memory/" + f] = ("private", os.path.join(env.memory, f))
    for k in [k for k in out if "_archive/" in k]:
        del out[k]
    return out


def check_ignore(repo, paths):
    if not paths:
        return []
    r = subprocess.run(["git", "check-ignore", "-z", "--stdin"], cwd=repo, capture_output=True,
                       text=True, encoding="utf-8", errors="replace", input="\0".join(paths) + "\0")
    if r.returncode not in (0, 1):
        raise RuntimeError("git check-ignore failed: " + r.stderr.strip()[:200])
    return [p for p in r.stdout.split("\0") if p]


# ----------------------------------------------------------------------------- the diff's symbols
KEYWORDS = {"const", "static", "return", "struct", "class", "namespace", "template", "inline", "void",
            "bool", "float", "double", "auto", "public", "private", "override", "virtual", "unsigned",
            "import", "from", "def", "self", "print", "param", "function", "string", "while", "else"}


def diff_symbols(env, base):
    """Touched code paths as PATH citations (basename with extension) + hunk-header identifiers."""
    syms = {}
    out = git(["diff", "-U0", base, "--", "src", "include", "tools", "reference"], env.repo, check=False)
    for line in out.splitlines():
        if line.startswith("+++ b/") or line.startswith("--- a/"):
            p = line[6:].strip()
            if p != "dev/null":
                syms[os.path.basename(p)] = "path"
        elif line.startswith("@@"):
            ctx = line.split("@@")[-1]
            for t in re.findall(r"[A-Za-z_][A-Za-z0-9_]{4,}", ctx):
                if t not in KEYWORDS and t.upper() != t:
                    syms.setdefault(t, "hunk")
    for h in git(["log", "--format=%h", base + "..HEAD"], env.repo, check=False).split():
        syms[h] = "commit"
    return syms




# ----------------------------------------------------------------------------- trailers
def parse_trailer(body):
    for line in body.splitlines():
        if line.startswith(TRAILER_KEY + ":"):
            kv = {}
            for tok in line.split(":", 1)[1].split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    kv[k] = v
            return kv
    return None


def last_close(repo):
    """-> (sha, trailer dict) of the newest commit carrying the trailer, or (None, None)."""
    out = git(["log", "--format=%H%x00%B%x01", "--grep=^" + TRAILER_KEY + ":"], repo, check=False)
    for rec in out.split("\x01"):
        if "\x00" in rec:
            sha, body = rec.strip().split("\x00", 1)
            t = parse_trailer(body)
            if t:
                return sha, t
    return None, None


def base_for(repo, since, first_run_head=False):
    """`first_run_head`: an OWNED repo joining the census for the first time has no trailer to tile
    onto, and its whole history is not "this session's work" -- so its base is HEAD and nothing in it
    is TOUCHED. Its docs enter through the sweep like any other never-censused doc. Only MAIN refuses
    without a base, because main's base is what the gate tiles the close chain on."""
    sha, _ = last_close(repo)
    if sha:
        return sha, "trailer"
    if first_run_head:
        return git(["rev-parse", "HEAD"], repo).strip(), "first-run HEAD"
    if not since:
        raise SystemExit("REFUSE: no previous Docs-Census trailer in {} -- pass --since DATE for the first run"
                         .format(repo))
    sha = git(["rev-list", "-1", "--before=" + since + " 00:00", "HEAD"], repo).strip()
    if not sha:
        raise SystemExit("REFUSE: no commit before " + since)
    return sha, "since"


def format_trailer(v):
    """The column ORDER and every column's KIND live in `trailer_schema`, imported by this script AND
    by the gate -- one definition, so the four hand-written lists cannot drift apart again."""
    undeclared = [k for k in v if k not in TS.KIND]
    if undeclared:
        raise SystemExit("REFUSE: trailer column(s) with no declared kind: {} -- add them to "
                         "tools/docs/trailer_schema.py with a KIND (round 20, Q4)".format(
                             ", ".join(sorted(undeclared))))
    return TRAILER_KEY + ": " + " ".join("{}={}".format(k, v[k]) for k in TS.ORDER if k in v)


# ----------------------------------------------------------------------------- the ratchet + accretion
def ratchet_values(env):
    vals = {"ro-bytes": 0, "ro-longest": 0, "mem-over200": 0, "memref-dead": 0,
            "running-totals": 0,
            "wikilinks-dead": 0, "pairing-unref": 0, "pairing-dead": 0}
    ledger = read_text(os.path.join(env.repo, "docs", "LESSONS.md"))
    if ledger:
        # WP-4: the ledger gate's checks C / D, which CI cannot run (no memory corpus there) --
        # their numbers travel in the trailer and are ratcheted here and in docs_census_gate.
        dead_links = LG.check_wikilinks(ledger, env.memory)
        unref, dead_refs = LG.check_pairing(ledger, env.memory)
        if dead_links is not None:
            vals["wikilinks-dead"] = len(dead_links)
            vals["pairing-unref"] = len(unref)
            vals["pairing-dead"] = len(dead_refs)
    t = read_text(os.path.join(env.repo, "CLAUDE.md"))
    if t:
        lines = t.split("\n")
        start = next((i for i, l in enumerate(lines) if l.startswith("## Reading order after a session reset")), None)
        if start is not None:
            sect = lines[start:]
            vals["ro-bytes"] = len("\n".join(sect).encode("utf-8"))
            longest, n = 0, 0
            for l in sect:
                if re.match(r"^[0-9]+[a-z-]*\. ", l):
                    longest = max(longest, n)
                    n = 0
                n += 1
            vals["ro-longest"] = max(longest, n)
    m = read_text(os.path.join(env.memory, "MEMORY.md"))
    if m:
        vals["mem-over200"] = sum(1 for l in m.split("\n") if len(l) > 200)
    # A ledger row carrying a running total is a count that rots by construction (the ledger's own
    # lesson). `lessons_gate` has WARNED on these since WP-4, and the trailer DECLARED the column --
    # but nothing ever produced it, so no close has emitted `running-totals=` in its life. A column
    # with no writer is not what the REPORTED kind ("printed and never enforced") describes; that
    # phrase covers "nothing READS it". Found 2026-09-03 by censusing the schema against the code.
    if ledger:
        vals["running-totals"] = len(LG.check_running_totals(ledger))
    # The pointers the two index files MAKE. `wikilinks-dead` already covers `[[name]]` in the ledger;
    # this covers the rest of the reading order -- MEMORY.md's markdown links and its date GLOBS, and
    # CLAUDE.md's backticked repo paths. Nothing gated either file before 2026-09-03, and six of
    # MEMORY.md's eleven globs matched zero files.
    import memory_index          # local: memory_index imports THIS module, so the dependency is
                                 # one-way and there is no cycle to resolve
    vals["memref-dead"] = len(memory_index.dead_refs(env))
    return vals


def accretion_count(env, rs):
    """Correction vocabulary NOT in the [corr YYYY-MM-DD: ...] form, in the LIVING SCOPE (WP-2).

    The scope, stated because the ratchet is only as meaningful as its denominator: `CLAUDE.md` and
    `memory/MEMORY.md` are read WHOLE; any other `docs/*.md` is read only OUTSIDE its dated sections,
    and a doc whose FILENAME is dated is skipped entirely, as is `_archive/` and `research/`. The rule
    is that a dated record is allowed to carry old correction prose -- it is a record of what was
    believed then -- while a living doc must fold the correction into the claim.

    SO A MOVE CAN SATISFY THIS RATCHET, and that is worth knowing before reading a drop as work.
    `[V]` 2026-09-03: relocating 271 lines out of CLAUDE.md into `docs/signals/HISTORY.md`, whose only
    heading is dated, took `accretion` 275 -> 274 -- the one hit inside the moved text simply left the
    scope (DIFF pass, round 1 Q3). Here the drop is honest, because that text was always a dated build
    log and was in scope only by living inside CLAUDE.md. But nothing in the NUMBER distinguishes that
    from folding a correction, so a close whose accretion falls should say which of the two it did."""
    n = 0
    for key, (owner, ap) in rs.items():
        if key in ACCRETION_EXCLUDE or "_archive/" in key or key.startswith("research/"):
            continue
        whole = key in ("CLAUDE.md", "memory/MEMORY.md")
        if not whole:
            if not key.startswith("docs/") or DATED_NAME_RE.search(os.path.basename(key)):
                continue
        t = read_text(ap)
        if not t:
            continue
        dated_section = False
        for l in t.split("\n"):
            if l.startswith("#"):
                dated_section = bool(DATE_RE.search(l))
            if not whole and dated_section:
                continue
            if ACCRETION_RE.search(l) and not CORR_RE.search(l):
                n += 1
    return n


# ----------------------------------------------------------------------------- the two lanes
def asks_authoring(row):
    """Does this freshly-written row owe the AUTHORING question?

    THE VERDICT IS TWO QUESTIONS, NOT ONE (the round-7 reframe, corrected in round 8). A row the
    session just wrote cannot have AGED -- `[V]` the one real close verdicted 93 of 93 such rows
    STILL TRUE, because it was asking "is this still true?" of lines written minutes earlier. What a
    fresh row CAN carry is the other defect: false optimism at AUTHORING time, the "PROVEN from a
    smoke" class the skill's own preamble names. So the authoring lane asks only rows that ASSERT
    something falsifiable -- a completion label, or a sub-state this very close may already have
    falsified ("uncommitted", "commit pending", "hands-on pending"). A freshly written OPEN owes
    nothing: it will be asked the ageing question when the sweep reaches it.

    `[V]` on the one real close: 47 of 108 touched rows would be asked, 61 dropped."""
    lab = (row.get("label") or "").strip().upper().lstrip("[").rstrip("]")
    if lab in {w.upper() for w in COMPLETION_WORDS}:
        return True
    return bool(row.get("substate"))


def authoring_lane(row):
    row["lane"] = "authoring"
    return row


# ----------------------------------------------------------------------------- the age clock
_AGE_CACHE = {}
_FM_MODIFIED = re.compile(r"^\s+modified:\s*(\d{4}-\d{2}-\d{2})", re.M)
_BODY_DATE = re.compile(r"\b(20\d\d-\d\d-\d\d)\b")


def _repo_commit_dates(repo):
    """path -> unix time of its newest commit, in ONE git pass (a `git log` per doc is minutes)."""
    if repo in _AGE_CACHE:
        return _AGE_CACHE[repo]
    out, cur = {}, None
    try:
        txt = git(["log", "--format=%ct", "--name-only", "-z"], repo, check=False)
    except (OSError, RuntimeError):
        txt = ""
    for chunk in txt.split("\0"):
        for piece in chunk.split(chr(10)):
            piece = piece.strip()
            if not piece:
                continue
            if piece.isdigit() and len(piece) >= 9:
                cur = int(piece)
            elif cur and piece not in out:
                out[piece] = cur
    _AGE_CACHE[repo] = out
    return out


def doc_age(env, key, rs, want_rung=False):
    """-> a unix time for the doc's CLAIM age, oldest-first sortable; with want_rung, (time, rung).

    A LADDER, because no single clock covers the corpus (round 12 / round 14):
      1 the AUTHORING repo's newest commit for the path -- main or an owned inner repo. NEVER the
        private history: `[V]` it holds 2 commits both dated 2026-09-03, so it would stamp all 1,067
        memory files with the SNAPSHOT date and rank the whole private corpus as the freshest thing
        in the read set -- the exact opposite of where the stale-open density is.
      2 the frontmatter `modified:` -- 645 of the 1,067 memory files carry one.
      3 a date in the BODY -- 291 more.
      4 a date in the FILENAME -- 124 more.
      5 mtime -- SEVEN files, all named in the arc; invalid alone (228 memory files share the
        2026-07-28 compaction day), which is why it is last and never first.
    """
    owner, ap = rs.get(key, (None, None))
    if not ap:
        return (0, "none") if want_rung else 0
    if owner == "main":
        t = _repo_commit_dates(env.repo).get(key)
        if t:
            return (t, "commit") if want_rung else t
    elif owner and owner != "private":
        d = owned_dir(env, owner)
        if d:
            t = _repo_commit_dates(d).get(key[len(owner) + 1:])
            if t:
                return (t, "commit") if want_rung else t
    txt = read_text(ap) or ""
    m = _FM_MODIFIED.search(txt)
    rung = None
    if m:
        stamp, rung = m.group(1), "frontmatter"
    else:
        b = _BODY_DATE.findall(txt)
        if b:
            stamp, rung = max(b), "body"
        else:
            f = _BODY_DATE.findall(os.path.basename(key))
            stamp, rung = (max(f), "filename") if f else (None, "mtime")
    if stamp:
        try:
            t = _dt.datetime.strptime(stamp, "%Y-%m-%d").replace(tzinfo=_dt.timezone.utc).timestamp()
            return (t, rung) if want_rung else t
        except ValueError:
            pass
    try:
        t = os.path.getmtime(ap)
    except OSError:
        t = 0
    return (t, "mtime") if want_rung else t


# ----------------------------------------------------------------------------- owned paths + the private-index commit
def owned_dir(env, owner):
    """-> the abs path of the OWNED inner repo whose basename is `owner`, or None."""
    for p in env.owned:
        if os.path.basename(p) == owner:
            return p
    return None


def baseline_text(env, key, owner, base, rbase):
    """The doc's content at its owner's census base, or None when it has none (a new file) -- in which
    case every row in it is new and all of them are owed a verdict. `rbase` maps owner -> that repo's
    base (one entry per owned repo since round 15 widened ownership past `research`)."""
    if owner == "main":
        r = subprocess.run(["git", "show", "{}:{}".format(base, key)], cwd=env.repo,
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    elif owned_dir(env, owner) and (rbase or {}).get(owner):
        r = subprocess.run(["git", "show", "{}:{}".format(rbase[owner], key[len(owner) + 1:])],
                           cwd=owned_dir(env, owner),
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    else:                                  # private: the history repo's HEAD holds the previous snapshot
        if not os.path.isdir(os.path.join(env.history, ".git")):
            return None
        r = subprocess.run(["git", "show", "HEAD:{}".format(key)], cwd=env.history,
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.stdout if r.returncode == 0 else None


def intent_to_add(repo):
    """Paths another session marked `git add -N`. They are INVISIBLE to both guards otherwise:
    `git diff --cached --name-only` does not list them (nothing is staged yet) while
    `git ls-files -- '*.md'` DOES (an index entry exists) and `git ls-files --others` does not --
    so such a file would be owned as a tracked doc, never reach the `--new` refusal, and be
    published by a stranger's close, erasing the neighbour's marker. Measured 2026-09-03 by a
    post-ship audit. `git status --porcelain=v2` marks them `1 .A ` (index unchanged, worktree added)."""
    out = set()
    for rec in git(["status", "--porcelain=v2", "-z", "--untracked-files=no"], repo).split("\0"):
        if rec.startswith("1 .A "):
            out.add(rec.split(" ", 8)[-1])
    return out


def staged_entries(repo):
    """{path: 'same'|'partial'} for every entry staged in the SHARED index (index != HEAD)."""
    out = {}
    for p in gitz(["diff", "-z", "--cached", "--name-only"], repo):
        wt = git(["diff", "--name-only", "--", p], repo).strip()   # index vs worktree
        out[p] = "partial" if wt else "same"
    return out


def owned(repo, path, tracked_md):
    """(1) tracked *.md anywhere; (2) the close's own instruments; (3) comment-only change."""
    if path in tracked_md:
        return "md"
    if path.startswith(OWN_TOOLING):
        return "own"
    ext = os.path.splitext(path)[1]
    old = git(["show", "HEAD:" + path], repo, check=False) if git(["ls-files", "--", path], repo).strip() else None
    new = read_text(os.path.join(repo, path))
    if old is not None and new is not None and comment_only(old, new, ext):
        return "comment-only"
    return None


def compose(repo, subject, trailers):
    """subject + blank line + the trailers, appended by git itself (interpret-trailers), in the order given."""
    args = ["interpret-trailers", "--if-exists", "addIfDifferent"]
    for t in trailers:
        args += ["--trailer", t]
    return git(args, repo, input_text=subject + "\n")


def private_commit(repo, paths, subject, trailers):
    """Commit exactly `paths` (worktree content) from a temporary index; align the shared index for
    those paths afterwards (LESSONS.md:145 / :6982 -- two axes; docs/DOCUMENTIZE_ARC.md round 6 Q1)."""
    tmp = os.path.join(repo, ".git", "docs_census.index")
    env = dict(os.environ, GIT_INDEX_FILE=tmp)
    try:
        if os.path.exists(tmp):
            os.remove(tmp)
        git(["read-tree", "HEAD"], repo, env=env)
        git(["add", "--"] + paths, repo, env=env)
        git(["commit", "-q", "-F", "-"], repo, env=env, input_text=compose(repo, subject, trailers))
        sha = git(["rev-parse", "HEAD"], repo).strip()
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    git(["reset", "-q", "--"] + paths, repo)
    return sha


# ----------------------------------------------------------------------------- the census
def pending_path(env):
    return os.path.join(env.history, "census", "pending.md")


def write_table(path, meta, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Docs census -- {} \n\n".format(meta["utc"]))
        f.write("```json\n" + json.dumps(meta, indent=1, sort_keys=True) + "\n```\n\n")
        f.write("Fill VERDICT with exactly one of: {}.\n\n".format(" / ".join(VERDICTS)))
        # THE ROW CARRIES ITS OWN HASH. It used to ride a positional `<!-- rowhash: ... -->` sidecar
        # re-attached by the row's PRINTED NUMBER with no validation -- the shape `docs/LESSONS.md:6548`
        # records this project retiring once already (commit f74d05dc): a renumber shifts every later
        # hash silently, and a hand-inserted row reads "". Self-binding rows survive a deletion, a
        # renumber and an insertion.
        f.write("| # | lane | path:line | kind | label | sub-state | tokens | date | total | id | VERDICT |\n")
        f.write("|---|---|---|---|---|---|---|---|---|---|---|\n")
        for i, r in enumerate(rows, 1):
            toks = " ".join("{}={}".format(t, s) for t, s in r["tokens"]) or "-"
            f.write("| {} | {} | {}:{} | {} | {} | {} | {} | {} | {} | {} | {} |\n".format(
                i, r.get("lane", "ageing"), r["key"], r["line"], r["kind"],
                r["label"].replace("|", "/") or "-",
                ",".join(r["substate"]) or "-", toks.replace("|", "/"), r["date"] or "-",
                "yes" if r["total"] else "-", r["hash"][:12], r["verdict"] or ""))


def read_table(path):
    t = read_text(path)
    if not t:
        return None, []
    m = re.search(r"```json\n(.*?)\n```", t, re.S)
    meta = json.loads(m.group(1)) if m else {}
    rows = []
    for l in t.split("\n"):
        if not l.startswith("| ") or l.startswith("| # ") or l.startswith("|---"):
            continue
        cells = [c.strip() for c in l.strip().strip("|").split("|")]
        if len(cells) < 11 or not cells[0].isdigit():
            continue
        key, _, line = cells[2].rpartition(":")
        toks = []
        for tok in cells[6].split():
            if "=" in tok:
                a, b = tok.rsplit("=", 1)
                toks.append((a, b))
        rows.append({"n": int(cells[0]), "lane": cells[1], "key": key,
                     "line": int(line) if line.isdigit() else 0,
                     "kind": cells[3], "label": cells[4],
                     "substate": cells[5].split(",") if cells[5] != "-" else [],
                     "tokens": toks, "date": cells[7], "total": cells[8] == "yes",
                     "hash": cells[9], "verdict": cells[10].strip().upper()})
    return meta, rows


def run_census(env, args):
    t0 = time.time()
    # BEFORE anything expensive: the pending table is ONE fixed path in a directory two sessions on this
    # box share, so a second census would silently destroy a first session's hand verdicts (a post-ship
    # audit, 2026-09-03). Overwriting a table that already carries verdicts needs --force.
    prev_meta, prev_rows = read_table(pending_path(env))
    if prev_rows and any(r["verdict"] for r in prev_rows) and not args.force:
        held = sum(1 for r in prev_rows if r["verdict"])
        raise SystemExit("REFUSE: the pending table already holds {} hand verdict(s), written {} -- "
                         "close it, or re-run with --force, which RE-CENSUSES and carries every "
                         "verdict whose line is unchanged; a line you edited returns as a new row "
                         "needing its own verdict ({})".format(
                             held, (prev_meta or {}).get("utc", "?"), pending_path(env)))
    # The dated index is generated from the memory directory, so it is refreshed BEFORE the
    # read set is taken -- that way the census reads and PINS the current bytes, and the
    # close never has to write a path it has already checked.
    import memory_index
    ipath, ichanged = memory_index.write(env)
    if ichanged:
        print("memory index regenerated: " + ipath)
    rs = read_set(env)
    by_owner = {}
    for k, (o, _) in rs.items():
        by_owner[o] = by_owner.get(o, 0) + 1
    print("read set: {} paths  ({})".format(
        len(rs), " / ".join("{} {}".format(o, by_owner[o]) for o in sorted(by_owner))))
    base, how = base_for(env.repo, args.since)
    print("base (main): {} [{}]".format(base[:10], how))
    rbase = {}
    for owned in env.owned:
        name = os.path.basename(owned)
        rbase[name], rhow = base_for(owned, args.since, first_run_head=True)
        print("base ({}): {} [{}]".format(name, rbase[name][:10], rhow))
    # radius (i): touched, per owner
    touched, new_paths = set(), []
    for p in gitz(["diff", "-z", "--name-only", base, "--", "*.md"], env.repo):
        if p in rs:
            touched.add(p)
    for p in gitz(["ls-files", "-z", "--others", "--exclude-standard", "--", "docs/*.md", "*.md"], env.repo):
        if p.endswith(".md"):
            new_paths.append(p)
            rs.setdefault(p, ("main", os.path.join(env.repo, p)))
            touched.add(p)
    for owned in env.owned:
        name = os.path.basename(owned)
        for args_ in (["diff", "-z", "--name-only", rbase[name], "--", "*.md"],
                      ["ls-files", "-z", "--others", "--exclude-standard", "--", "*.md"]):
            for p in gitz(args_, owned):
                if not p.endswith(".md"):
                    continue
                key = name + "/" + p
                touched.add(key)
                rs.setdefault(key, (name, os.path.join(owned, p)))
    changed_private = snapshot_sync(env, rs)
    for p in changed_private:
        if p in rs:
            touched.add(p)
    print("radius (i) touched: {} docs  (main-new {} / private-changed {})".format(
        len(touched), len(new_paths), len(changed_private)))
    # radius (ii): docs citing a specific symbol of the code diff
    syms = diff_symbols(env, base)
    cited, dropped = set(), {}
    if syms:
        # ONE pass over the read set: each doc is tokenised once and intersected with the symbol set
        # (a regex per symbol over 17 MB was minutes -- measured 2026-09-03)
        symset = set(syms)
        word = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_./\\-]*")
        hits = {}
        for key, (_, ap) in rs.items():
            t = read_text(ap) or ""
            toks = set()
            for m in word.finditer(t):
                w = m.group(0)
                toks.add(w)
                if "/" in w or "\\" in w:
                    toks.add(w.replace("\\", "/").rsplit("/", 1)[-1])
            for s in symset & toks:
                hits.setdefault(s, []).append(key)
        for s, docs in hits.items():
            if len(docs) <= 5:
                cited.update(docs)
            else:
                dropped[s] = len(docs)
    print("radius (ii) cited: {} docs from {} diff symbols; dropped as generic: {}".format(
        len(cited), len(syms), ", ".join("{}({})".format(s, n) for s, n in sorted(dropped.items(), key=lambda x: -x[1])[:12]) or "-"))
    print("  t={:.1f}s".format(time.time() - t0))
    # radius (iii): the amortised sweep, paid in ROWS
    st = state_load(env)
    # THE SWEEP'S ORDER IS THE DOC'S AGE, OLDEST FIRST -- not the alphabet. `[V]` 2026-09-03: `never`
    # held 1,521 of 1,552 docs and was `sorted()`, so the utc ordering could not take effect for ~150
    # closes and the one real run swept README / SECURITY / BUILDING / CONTRIBUTORS and ZERO research
    # findings, against the 185 dated 2026-06/07 where the arc's own section 2.3 measured the rot.
    # AND A TOUCHED DOC KEEPS ITS PLACE IN THE QUEUE: the old filter dropped `touched` BEFORE any
    # ordering, so `MEMORY.md`, `LESSONS.md` and `CLAUDE.md` -- touched by steps 3/3.5/4 of EVERY close
    # -- could never be scanned whole (they hold 194 label rows between them; one close surfaced 17,
    # and the state file had never stamped CLAUDE.md once). When the sweep picks a touched doc it is
    # read WHOLE and its rows merge with the diff rows, deduped by hash.
    never = sorted((k for k in rs if k not in st["docs"]), key=lambda k: (doc_age(env, k, rs), k))
    oldest = sorted((k for k in rs if k in st["docs"]), key=lambda k: st["docs"][k]["utc"])
    k = args.k if args.k is not None else K_DEFAULT
    budget = args.rows if args.rows is not None else ROW_BUDGET
    candidates = [c for c in never + oldest if c not in cited]

    # the scan
    resolver = Resolver(env)
    resolver.index()
    resolver.corpora()
    print("  resolver ready (index {} basenames, corpora {} identifiers) t={:.1f}s".format(
        len(resolver.index()), len(resolver.corpora()), time.time() - t0))

    def scan(key, diff_scoped):
        """A TOUCHED doc contributes only the rows this session INTRODUCED OR CHANGED, not every status
        line it contains. Measured 2026-09-03 on the first real close: 29 touched docs produced 431 rows
        because `docs/LESSONS.md` alone carries 85 and `CLAUDE.md` 71 -- so editing one line of a doc
        owed a hand verdict on all of them, which is the 11,291-hit mandate again one order of magnitude
        down. A row's identity is its line's hash, so the diff is exact, and the rest of the doc reaches
        a verdict through the sweep like any other doc."""
        out = scan_doc(key, rs[key][1], resolver, loose=args.loose)
        if diff_scoped:
            old = baseline_text(env, key, rs[key][0], base, rbase)
            if old is not None:
                seen = {r["hash"] for r in scan_text(key, old, resolver, loose=args.loose)}
                scanned_whole.discard(key)
                return [authoring_lane(r) for r in out if r["hash"] not in seen
                        if asks_authoring(r)]
            # NO BASELINE -- the doc is being PUBLISHED for the first time. `[V]` 2026-09-03: 14 of 33
            # touched docs were in this state and contributed 41 of 108 rows; they are not "lines this
            # session wrote", they are docs WE authored days ago and are publishing now. Same lane.
            scanned_whole.add(key)
            return [authoring_lane(r) for r in out if asks_authoring(r)]
        scanned_whole.add(key)      # a sweep doc: every row was offered
        for r in out:
            r["lane"] = "ageing"
        return out

    rows, swept, scanned_whole = [], [], set()
    if args.sweep:                                    # the whole read set, on request: no budget
        radius = set(rs)
        for n, key in enumerate(sorted(radius), 1):
            rows.extend(scan(key, False))
            if n % 100 == 0:
                print("  scanned {}/{} docs, {} rows, t={:.1f}s".format(n, len(radius), len(rows), time.time() - t0))
        swept = sorted(radius)
    else:
        for key in sorted(touched | cited):
            rows.extend(scan(key, key in touched))
        owed = len(rows)
        # The sweep's budget is ITS OWN, never "what is left of a shared one": the session's rows are
        # not negotiable (you changed those lines, you verdict them), so a shared budget is eaten by a
        # busy session and the queue stops moving -- measured 2026-09-03 on the first real close, where
        # 119 own rows left the sweep 1 doc of 1,521 candidates, i.e. ~1,521 closes to reach the tree.
        # It takes whole docs (per-doc census state stays meaningful); a doc larger than the budget is
        # still taken when nothing else has been, so the queue always advances by at least one.
        seen_hashes = {(r["key"], r["hash"]) for r in rows}
        for key in candidates[:k]:
            if len(rows) - owed >= budget and swept:
                break
            # A doc the sweep picks is read WHOLE even when this session touched it -- that is the
            # only way `MEMORY.md` / `LESSONS.md` / `CLAUDE.md` are ever fully censused -- and its
            # whole-scan rows merge with the diff rows already collected, deduped by (key, hash).
            r = [x for x in scan(key, False) if (x["key"], x["hash"]) not in seen_hashes]
            seen_hashes.update((x["key"], x["hash"]) for x in r)
            rows.extend(r)
            swept.append(key)
        print("radius (i)+(ii): {} docs -> {} rows (touched docs are diff-scoped)".format(
            len(touched | cited), owed))
        print("radius (iii) sweep: {} of {} candidate docs -> {} rows (K={} cap, sweep row budget {}, "
              "never-censused {})".format(len(swept), len(candidates), len(rows) - owed, k, budget, len(never)))
        radius = (touched | cited | set(swept))
    cycle = -(-len(candidates) // max(1, len(swept))) if swept else 0
    print("radius total: {} docs, {} rows{}".format(
        len(radius), len(rows), " (--sweep: the whole read set)" if args.sweep else
        " (~{} closes to reach every doc at this rate)".format(cycle)))
    # carry verdicts forward from a pending table by (key, hash)
    # The table stores the hash TRUNCATED to 12 (it is a column a human reads past), so the carry
    # compares the same prefix on both sides -- never the full digest against a short one.
    carried = {(r["key"], r["hash"][:12]): r["verdict"] for r in prev_rows if r["verdict"]}
    for r in rows:
        r["verdict"] = carried.get((r["key"], r["hash"][:12]), "")
    # A verdict that does NOT carry, on a doc still in the radius, is one whose line was acted on --
    # the fix the verdict ordered. Record it before it is lost (D0; see the resolved ledger above).
    _whole_cache = {}

    def whole_hashes(key):
        """Every row hash the doc yields READ WHOLE -- the witness `retired_verdicts` asks, so a
        prior verdict is retired on what the FILE says and not on which scope this census used."""
        if key not in _whole_cache:
            # EVERY LINE's hash, not every ROW's: a row can vanish for reasons that have nothing
            # to do with its line (a `cite` row exists only while its citation resolves dead).
            lines = resolver.lines_of(rs[key][1]) if key in rs else None
            _whole_cache[key] = set(line_hashes(key, lines)) if lines is not None else None
        return _whole_cache[key]

    retired = retired_verdicts(prev_rows, rows, radius, utc_now(), base, whole_hashes)
    resolved_append(env, retired)
    if retired:
        by_v = {}
        for r in retired:
            by_v[r["verdict"]] = by_v.get(r["verdict"], 0) + 1
        print("resolved: {} verdict(s) retired to the ledger ({})".format(
            len(retired), " ".join("{} {}".format(v, n) for v, n in sorted(by_v.items()))))
    labels = sum(1 for r in rows if r["kind"] not in ("cite", "loose", "drift"))
    dead = sum(1 for r in rows if dead_cites(r["tokens"]))
    drift = sum(1 for r in rows if drift_cites(r["tokens"]))
    # The bytes the census READ, per doc. The close refuses to commit anything else (a post-ship audit,
    # 2026-09-03: `staged_entries` reads the INDEX, so another session's plain unstaged edit to a doc in
    # our radius was invisible and rode into our close). Escape: re-run `census`, which re-reads and
    # re-pins -- and a line the hand edited then arrives as a NEW row needing its own verdict, which is
    # the point: the trailer's counts must describe the text being committed, not the text before the fix.
    content = {key: sha1(read_text(rs[key][1]) or "") for key in sorted(radius)}
    meta = {"utc": utc_now(), "base": base, "research_base": rbase, "touched": sorted(touched),
            "content": content,
            "new": new_paths, "radius": len(radius), "rows": len(rows), "labels": labels, "cited_dead": dead, "cite_drift": drift,
            "sweep": swept, "scanned_whole": sorted(scanned_whole), "k": k, "budget": budget, "cycle": cycle, "read_set": len(rs), "loose": bool(args.loose),
            "full_sweep": bool(args.sweep), "dropped": dropped}
    write_table(pending_path(env), meta, rows)
    print("rows: {} (labels {}, dead citations {}, symbol drift {}); verdicts carried forward: {}"
          .format(len(rows), labels, dead, drift, sum(1 for r in rows if r["verdict"])))
    print("table: " + pending_path(env))
    rv = ratchet_values(env)
    rv["accretion"] = accretion_count(env, rs)
    print("ratchet now: " + " ".join("{}={} (target {})".format(c, rv.get(c, 0), TARGETS.get(c, 0))
                                     for c in RATCHET_COLS))
    print("accretion now: {}".format(accretion_count(env, rs)))
    return 0


# ----------------------------------------------------------------------------- the close
def run_close(env, args):
    meta, rows = read_table(pending_path(env))
    if not rows and not meta:
        raise SystemExit("REFUSE: no pending census -- run `census` first")
    if args.subject.startswith("["):
        raise SystemExit("REFUSE: the subject must not carry its own prefix; the script writes '{}'".format(CLOSE_PREFIX))
    trailers = list(args.trailer or [])
    if not any(t.startswith("Co-Authored-By:") for t in trailers) or not any(t.startswith("Claude-Session:") for t in trailers):
        raise SystemExit("REFUSE: --trailer 'Co-Authored-By: ...' and --trailer 'Claude-Session: ...' are required "
                         "(the trailer STAYS -- CLAUDE.md git-identity rule; the script cannot mint either)")
    # 1. every row carries exactly one verdict token; no STILL TRUE / ACTUALLY DONE on a dead citation
    bad = [r for r in rows if r["verdict"] not in VERDICTS]
    if bad:
        raise SystemExit("REFUSE: {} of {} rows carry no verdict token (first: #{} {}:{} -> '{}')".format(
            len(bad), len(rows), bad[0]["n"], bad[0]["key"], bad[0]["line"], bad[0]["verdict"]))
    # THE ACCEPTANCE SIDE of the same rule. A row of kind `cite`, `drift` or `loose` makes no status
    # claim -- it exists because a citation resolved dead, a symbol moved, or the loose regex fired --
    # so the four STATUS verdicts are category errors on it, exactly as a status verdict on a drift row
    # was. `[V]` 2026-09-03: round 2 retired the seventh token and deleted the two-way refusal WITH it,
    # so a drift row verdicted STALE DONE closed cleanly and landed in `stale-done=` -- which is D8's
    # falsifier input, the number that decides whether the hand phase survives (DIFF pass, round 3 Q2).
    # Fixing the rejection side while leaving the acceptance side open is a one-sided gate.
    # STILL TRUE (nothing to do) and NOT A LABEL (this rung mis-fired) remain valid on every row.
    miscast = [r for r in rows if r["verdict"] in STATUS_VERDICTS and r["kind"] in LABEL_BUCKET]
    if miscast:
        r = miscast[0]
        raise SystemExit(
            "REFUSE: #{} {}:{} is a '{}' row verdicted {} -- that row states no STATUS, so only "
            "STILL TRUE or NOT A LABEL can answer it. A status verdict here would count into "
            "`{}=`, which measures the LABEL grammar's rows.".format(
                r["n"], r["key"], r["line"], r["kind"], r["verdict"],
                r["verdict"].lower().replace(" ", "-")))
    # On a LABEL row a dead pointer under a live status is the measured rot class (section 2.3 #5,
    # #21, #27). On a plain prose line (kind `cite`) STILL TRUE means "dead on purpose" -- a doc
    # naming a file that was retracted or moved away -- and only the hand can tell; it is recorded,
    # not refused. `drift` is excluded too: its tokens are advisory by construction.
    contra = [r for r in rows if r["verdict"] in ("STILL TRUE", "ACTUALLY DONE")
              and r["kind"] not in ("cite", "drift") and dead_cites(r["tokens"])]
    if contra:
        r = contra[0]
        raise SystemExit("REFUSE: #{} {}:{} is verdicted {} but its citation resolved dead ({})".format(
            r["n"], r["key"], r["line"], r["verdict"],
            " ".join("{}={}".format(t, s) for t, s in r["tokens"])))
    counts = {v: sum(1 for r in rows if r["verdict"] == v) for v in VERDICTS}
    # NOT A LABEL, split by the instrument whose row it rejects.
    buckets = {c: 0 for c in LABEL_BUCKET.values()}
    for r in rows:
        if r["verdict"] == "NOT A LABEL" and r["kind"] in LABEL_BUCKET:
            buckets[LABEL_BUCKET[r["kind"]]] += 1
            counts["NOT A LABEL"] -= 1
    rs = read_set(env)
    # 2. the census must not be stale: every doc touched NOW was in the census's touched set
    base = meta["base"]
    now_touched = set()
    for p in gitz(["diff", "-z", "--name-only", base, "--", "*.md"], env.repo):
        now_touched.add(p)
    stale = sorted(p for p in now_touched if p in rs and p not in set(meta.get("touched", [])))
    if stale:
        raise SystemExit("REFUSE: docs changed since the census and have no rows: {} -- re-run `census` "
                         "(verdicts carry forward)".format(", ".join(stale[:8])))
    # 3. main's paths: touched main docs + --new, minus anything staged in the SHARED index
    pending_add = intent_to_add(env.repo)
    tracked_md = set(git(["ls-files", "-z", "--", "*.md"], env.repo).split("\0")) - pending_add
    main_paths = [p for p in meta.get("touched", []) if p in rs and rs[p][0] == "main" and p in tracked_md]
    untracked = gitz(["ls-files", "-z", "--others", "--exclude-standard", "--", "docs/*.md", "*.md"], env.repo)
    untracked += sorted(p for p in pending_add if p.endswith(".md"))   # `git add -N` = new, not tracked
    new = list(args.new or [])
    for p in new:
        if p not in untracked:
            raise SystemExit("REFUSE: --new {} is not an untracked, unignored file".format(p))
    undecided = [p for p in untracked if p not in new]
    if undecided:
        raise SystemExit("REFUSE: new unignored *.md in the read set, neither --new nor gitignored: {}"
                         .format(", ".join(undecided)))
    staged = staged_entries(env.repo)
    foreign = 0
    for p in list(main_paths):
        if p in staged:
            if staged[p] == "same":
                main_paths.remove(p)
                foreign += 1
                print("foreign (staged whole by another session, excluded): " + p)
            else:
                raise SystemExit("REFUSE: {} is staged PARTIALLY in the shared index -- a same-file collision; "
                                 "resolve per docs/CROSS_SESSION.md".format(p))
    main_paths += new
    # 4. non-doc paths the close may carry: only the close's own tooling and comment-only changes,
    #    and only when the caller names them (a close carries CLAIMS)
    for p in list(args.also_comment or []):
        kind = owned(env.repo, p, tracked_md)
        if kind is None:
            raise SystemExit("REFUSE: {} is not a claim (not md, not tools/docs|.claude/skills, not comment-only) -- "
                             "commit it first on its own".format(p))
        main_paths.append(p)
    if not main_paths:
        raise SystemExit("REFUSE: nothing to commit in main (no touched docs)")
    # 4b. EVERY path ANY of the three commits carries must hold the bytes the census read -- an
    # INVARIANT over the whole commit set, not a main-only site. The audit fix that added this pin
    # (2026-09-03) landed on `main_paths` alone, so the owned-repo commit and the history snapshot
    # committed today's bytes under yesterday's verdicts -- exactly what the pin exists to prevent
    # (round 16, Q2). The check runs ONCE, before any commit.
    # AND THE SET IS THE WHOLE PIN, not the paths this close names. The version above reconstructed a
    # `committed` set from `main_paths` plus TOUCHED private paths -- but the history commit is
    # `git add -A` over the whole snapshot, so a doc the SWEEP read and verdicted, then edited before
    # the close, was committed under those verdicts with nothing checking it (DIFF pass, round 3 Q4;
    # reproduced on a swept `memory/` topic). `pinned` already IS the set the census read; iterating it
    # is both correct and shorter than deriving a subset of it.
    pinned = meta.get("content", {})
    drifted = []
    for p in sorted(pinned):
        ap = rs[p][1] if p in rs else os.path.join(env.repo, p)
        if sha1(read_text(ap) or "") != pinned[p]:
            drifted.append(p)
    if drifted:
        raise SystemExit("REFUSE: {} doc(s) changed since the census that produced these verdicts: {} -- "
                         "re-run `census` (it re-reads them; an edited line arrives as a new row and is "
                         "verdicted on the text you are about to commit)".format(len(drifted), ", ".join(drifted[:8])))
    # 4c. the table the hand returns must be the table the census wrote. `meta["rows"]` had ZERO readers
    # while the trailer's `rows=` came from re-parsing the hand-edited markdown, so a row deleted by
    # hand shrank the count silently and "rows = sum of verdicts" described the PRUNED table, not the
    # census's finding (round 10, Q4).
    if meta.get("rows") is not None and meta["rows"] != len(rows):
        raise SystemExit("REFUSE: the census wrote {} rows and the table now holds {} -- rows were "
                         "added or deleted by hand. Restore them, or re-run `census` (verdicts on "
                         "unchanged lines carry forward).".format(meta["rows"], len(rows)))
    # 5. numbers
    rv = ratchet_values(env)
    rv["accretion"] = accretion_count(env, rs)
    prev_sha, prev = last_close(env.repo)
    if prev:
        grew = [c for c in RATCHET_COLS if c in prev and prev[c].isdigit() and rv[c] > int(prev[c])]
        if grew:
            raise SystemExit("REFUSE (ratchet): {} grew vs the previous close {}: {}".format(
                ", ".join(grew), prev_sha[:10], " ".join("{} {}->{}".format(c, prev[c], rv[c]) for c in grew)))
    st = state_load(env)
    utc = utc_now()
    # Stamp ONLY the docs that were read WHOLE. A touched doc is diff-scoped -- the hand verdicted the
    # rows this session changed, not the doc -- so stamping it "censused" would be the same false claim
    # this arc exists to delete, and would push it to the back of the sweep queue unread.
    for key in meta.get("scanned_whole", []):
        st["docs"][key] = {"utc": utc, "base": base}
    cursor = sum(1 for k in rs if k in st["docs"])
    # The reading order: a SHRINK is only good news if the facts went somewhere. The previous
    # CLAUDE.md lives in the private history (nothing else versions it), so the comparison is
    # against that -- and a clause that left with no destination is PRINTED, because a claim
    # being destroyed is not a number.
    import reading_order
    ro_moved = ro_cut = 0
    prev_cl = baseline_text(env, "CLAUDE.md", "private", base, {})   # private: the history repo's HEAD
    if prev_cl is not None:
        now_cl = read_text(os.path.join(env.repo, "CLAUDE.md")) or ""
        moved, cut, lost = reading_order.moved_and_cut(env.repo, prev_cl, now_cl)
        ro_moved, ro_cut = len(moved), len(cut)
        if moved or cut or lost:
            print("reading order: {} clause(s) moved to a destination, {} CUT, {} EXEMPT-LOST"
                  .format(len(moved), len(cut), len(lost)))
        for _, raw in cut[:12]:
            print("  CUT (found in no doc): " + raw[:140])
        # An exempt line is a record of what the USER said. PRINTING it is a post-mortem -- the
        # ledger's own row says a detector where it cannot prevent what it names is not a guard
        # -- and `ro-bytes` is RATCHETED, so a close could otherwise EARN the ratchet by
        # deleting the user's own words (DIFF pass, round 2 Q2). So this one REFUSES.
        if lost:
            raise SystemExit(
                "REFUSE: {} line(s) recording what the USER said left the reading order:{}{}{}"
                .format(len(lost), chr(10),
                        chr(10).join("  " + raw[:200] for _, raw in lost),
                        chr(10) + "  -- restore them, or move them to a doc that keeps them "
                        "verbatim. `ro-bytes` may not be earned this way."))
    # The cumulative ledger totals. The DELTA is this close's own correction count -- the number the
    # verdict columns cannot carry, because a corrected line is committed in its corrected form.
    n_res, n_flip = resolved_counts(env)
    if prev:
        for col, now in (("resolved", n_res), ("flips", n_flip)):
            was = prev.get(col)
            if was is not None and was.isdigit() and now < int(was):
                raise SystemExit("REFUSE (monotone): {} went {}->{} vs the previous close {} -- the "
                                 "resolved ledger is append-only, so this means the private history "
                                 "was replaced or rolled back ({})".format(
                                     col, was, now, prev_sha[:10], resolved_path(env)))
        d_res = n_res - int(prev.get("resolved", "0") or 0)
        d_flip = n_flip - int(prev.get("flips", "0") or 0)
        print("resolved this close: {} verdict(s), {} of them naming a defect (cumulative {}/{})"
              .format(d_res, d_flip, n_res, n_flip))
    # D8's falsifier is stated over AGEING-lane rows -- "over the first 300, if actually-done +
    # stale-done + partial totals fewer than 5, the hand phase is deleted" -- and until 2026-09-03 no
    # trailer separated the lanes, so the bar could not be evaluated from the record it is evaluated
    # from (DIFF pass, round 4). Its denominator and numerator now ride the trailer, and only the
    # AGEING lane counts: an authoring row cannot have aged, so it is not evidence about rot.
    ageing = [r for r in rows if r.get("lane") == "ageing"]
    ageing_corr = sum(1 for r in ageing
                      if r["verdict"] in ("ACTUALLY DONE", "STALE DONE", "PARTIAL"))
    vals = {"base": base[:12], "rows": len(rows), "labels": meta.get("labels", 0),
            "ageing-rows": len(ageing), "ageing-corr": ageing_corr,
            "resolved": n_res, "flips": n_flip, "ro-moved": ro_moved,
            "ro-cut": ro_cut, "ro-lost": 0,   # a close with ro-lost > 0 cannot exist: it refuses
            "still-open": counts["STILL OPEN"], "actually-done": counts["ACTUALLY DONE"],
            "stale-done": counts["STALE DONE"], "partial": counts["PARTIAL"], "still-true": counts["STILL TRUE"],
            "not-a-label": counts["NOT A LABEL"],
            "not-a-cite": buckets["not-a-cite"], "drift-ok": buckets["drift-ok"],
            "not-loose": buckets["not-loose"],
            "cited-dead": meta.get("cited_dead", 0), "cite-drift": meta.get("cite_drift", 0),
            "accretion": rv["accretion"],
            "ro-bytes": rv["ro-bytes"], "ro-longest": rv["ro-longest"], "mem-over200": rv["mem-over200"],
            "memref-dead": rv["memref-dead"], "running-totals": rv["running-totals"],
            "wikilinks-dead": rv["wikilinks-dead"], "pairing-unref": rv["pairing-unref"],
            "pairing-dead": rv["pairing-dead"], "sweep-cursor": cursor, "sweep-cycle": meta.get("cycle", 0), "new": len(new), "foreign": foreign}
    # 6. commit 3 first: the private history (snapshot + state + the verdict table) -> census=
    # The dated index is refreshed by the CENSUS, not here: regenerating at close time would
    # rewrite a path the content pin has already checked, which is the one thing the pin
    # exists to forbid. Here it is only CHECKED, like any other doc the census read.
    import memory_index
    if memory_index.stale(env, rs):
        raise SystemExit("REFUSE: the memory directory changed since the census, so "
                         "memory/INDEX_BY_DATE.md is stale -- re-run `census --force`, which "
                         "regenerates the index and re-pins it (verdicts carry forward)")
    snapshot_sync(env, rs)
    state_save(env, st)
    final = os.path.join(env.history, "census", "{}-{}.md".format(utc, base[:10]))
    os.replace(pending_path(env), final)
    subject = CLOSE_PREFIX + " " + args.subject
    hist_trailer = format_trailer(dict(vals, census="pending"))
    # From a PRIVATE index here too: this repository's path is a pure function of the main repo's, so two
    # sessions on this box share it exactly as they share main's index -- and `git add -A` on a shared
    # index is the cross-session side effect docs/LESSONS.md records twice (a post-ship audit, 2026-09-03,
    # found this one commit bypassing the protection the other two use).
    hidx = os.path.join(env.history, ".git", "docs_census.index")
    henv = dict(os.environ, GIT_INDEX_FILE=hidx)
    try:
        if os.path.exists(hidx):
            os.remove(hidx)
        git(["read-tree", "HEAD"], env.history, env=henv)
        git(["add", "-A"], env.history, env=henv)
        git(["commit", "-q", "-F", "-"], env.history, env=henv,
            input_text=compose(env.history, subject, [hist_trailer] + trailers))
    finally:
        if os.path.exists(hidx):
            os.remove(hidx)
    git(["reset", "-q"], env.history)
    hsha = git(["rev-parse", "HEAD"], env.history).strip()
    vals["census"] = hsha[:12]
    # 7. commit 2: EVERY owned inner repo, not `research` alone (round 15: ownership is the local git
    # identity, and `site/` -- the public website copy -- was invisible to the census entirely).
    owned_shas = []
    rbases = meta.get("research_base") or {}
    if isinstance(rbases, str):                      # a table written before the map (one owner)
        rbases = {"research": rbases}
    for owned in env.owned:
        name = os.path.basename(owned)
        pre = name + "/"
        opaths = sorted(p[len(pre):] for p in meta.get("touched", [])
                        if p.startswith(pre) and rs.get(p, ("",))[0] == name)
        if not opaths:
            continue
        otrailer = format_trailer(dict(vals, base=(rbases.get(name) or "-")[:12]))
        osha = private_commit(owned, opaths, subject, [otrailer] + trailers)
        owned_shas.append("{}:{}".format(name, osha[:10]))
        print("{} close: {} ({} paths)".format(name, osha[:10], len(opaths)))
    vals["research-base"] = ",".join(owned_shas) if owned_shas else "-"
    # 8. commit 1: main
    trailer = format_trailer(vals)
    msha = private_commit(env.repo, sorted(set(main_paths)), subject, [trailer] + trailers)
    print("main close: {} ({} paths)".format(msha[:10], len(set(main_paths))))
    print("history close: {}".format(hsha[:10]))
    print(trailer)
    return 0


def run_show(env, args):
    meta, rows = read_table(pending_path(env))
    print(json.dumps(meta, indent=1, sort_keys=True)[:2000])
    for r in rows:
        print("#{} {}:{} [{}] {} {} -> {}".format(r["n"], r["key"], r["line"], r["kind"], r["label"],
                                                 ",".join(r["substate"]), r["verdict"] or "?"))
    return 0


def run_resolved(env, args):
    """Read the ledger back. It exists so the record is not write-only: a capability nothing calls is
    not shipped (docs/DEAD_CAPABILITY_REGISTER.md), and D8's falsifier -- 300 ageing-lane rows, fewer
    than 5 corrections and the hand phase is deleted -- is counted from exactly these records."""
    recs = resolved_load(env)
    if args.since:
        recs = [r for r in recs if r.get("utc", "") >= args.since]
    if args.verdict:
        recs = [r for r in recs if r.get("verdict") == args.verdict]
    for r in recs:
        print("{}  {:<12}  {}:{}  [{}/{}]  {}".format(
            r.get("utc", "?"), r.get("verdict", "?"), r.get("key", "?"), r.get("line", "?"),
            r.get("kind", "?"), r.get("lane") or "-", (r.get("label") or "").replace("\n", " ")[:90]))
    by_lane, by_v = {}, {}
    for r in recs:
        by_lane[r.get("lane") or "-"] = by_lane.get(r.get("lane") or "-", 0) + 1
        by_v[r.get("verdict", "?")] = by_v.get(r.get("verdict", "?"), 0) + 1
    print("{} record(s); naming a defect: {}".format(
        len(recs), sum(1 for r in recs if r.get("verdict") in FLIP_VERDICTS)))
    print("  by verdict: " + (" ".join("{} {}".format(k, v) for k, v in sorted(by_v.items())) or "-"))
    print("  by lane:    " + (" ".join("{} {}".format(k, v) for k, v in sorted(by_lane.items())) or "-"))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo")
    ap.add_argument("--memory-dir")
    ap.add_argument("--history-dir")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("census")
    c.add_argument("--since")
    c.add_argument("--sweep", action="store_true")
    c.add_argument("--loose", action="store_true")
    c.add_argument("--force", action="store_true",
                   help="re-census over a pending table that already holds hand verdicts: every "
                        "verdict whose line is UNCHANGED is carried forward; a line you edited "
                        "returns as a new row needing its own verdict")
    c.add_argument("-k", type=int, help="cap the sweep at N docs (default 40)")
    c.add_argument("--rows", type=int,
                   help="the SWEEP's own row budget, on top of the session's own rows (default 40)")
    cl = sub.add_parser("close")
    cl.add_argument("-m", "--subject", required=True)
    cl.add_argument("--trailer", action="append")
    cl.add_argument("--new", action="append")
    cl.add_argument("--also-comment", action="append", help="a non-doc path whose change is comment-only or the close's own tooling")
    sub.add_parser("snapshot")
    sub.add_parser("show")
    rv = sub.add_parser("resolved", help="read the resolved ledger: verdicts retired because the line they named was fixed")
    rv.add_argument("--since", help="only records at or after this utc stamp (the census's own format)")
    rv.add_argument("--verdict", choices=VERDICTS)
    args = ap.parse_args(argv)
    env = Env(args.repo, args.memory_dir, args.history_dir)
    if args.cmd == "census":
        return run_census(env, args)
    if args.cmd == "close":
        return run_close(env, args)
    if args.cmd == "snapshot":
        changed = snapshot_sync(env, read_set(env))
        print("snapshot: {} changed paths in {}".format(len(changed), env.history))
        return 0
    if args.cmd == "show":
        return run_show(env, args)
    if args.cmd == "resolved":
        return run_resolved(env, args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
