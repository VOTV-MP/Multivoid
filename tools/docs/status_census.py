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
"""
import argparse
import datetime as _dt
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lessons_gate as LG  # noqa: E402  -- build_corpora / CITE_ROOTS (same directory)
from status_grammar import *  # noqa: E402,F401,F403  -- the label grammar, the Resolver, scan_doc, read_text, sha1
from status_grammar import read_text, sha1, DATE_RE, CORR_RE, ACCRETION_RE, DATED_NAME_RE, STATUS_WORDS  # noqa: E402
from comment_lexer import comment_only  # noqa: E402

K_DEFAULT = 40
ROW_BUDGET = 40          # the SWEEP's own row budget: the hand check is paid per ROW, so that is the
                         # unit the sweep amortises over -- the session's own rows are always included
# The sixth token is not a status: it is the hand REJECTING the row. The grammar has a measured false
# positive rate (2026-09-02 sample: 29 % of the old regex's hits were not labels at all) and the five
# status verdicts have no value for "this line is not a claim" -- so without it a false positive must be
# verdicted STILL TRUE, which launders noise into the counts and hides the grammar's precision. With it,
# every close MEASURES that precision (`not-a-label=` in the trailer) instead of asserting it.
VERDICTS = ("STILL OPEN", "ACTUALLY DONE", "STALE DONE", "PARTIAL", "STILL TRUE", "NOT A LABEL")
CLOSE_PREFIX = "[docs] close:"
TRAILER_KEY = "Docs-Census"
RATCHET_COLS = ("ro-bytes", "ro-longest", "mem-over200", "wikilinks-dead", "pairing-unref", "pairing-dead")
TARGETS = {"ro-bytes": 58 * 1024, "ro-longest": 15, "mem-over200": 0}
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
        r = os.path.join(self.repo, "research")
        self.research = r if os.path.isdir(os.path.join(r, ".git")) else None


def git(args, cwd, check=True, env=None, input_text=None):
    r = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env, input=input_text)
    if check and r.returncode != 0:
        raise RuntimeError("git {} failed ({}): {}".format(" ".join(args), r.returncode,
                                                            (r.stderr or r.stdout).strip()[:400]))
    return r.stdout


def gitz(args, cwd):
    """A NUL-separated path list: git QUOTES non-ASCII paths in line mode (core.quotePath), -z never does."""
    return [p for p in git(list(args), cwd).split("\0") if p]


def utc_now():
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


# ----------------------------------------------------------------------------- the read set
def read_set(env):
    """-> {key: (owner, abspath)}; owner in main | research | private.

    Ownership is LOCATION + IGNORE RULES, not `git add` state (round 6, Q2): a path inside a
    repository's tree that its .gitignore does not exclude belongs to that repository, tracked
    or not. What no repository can hold (ignored, or outside every tree) is private."""
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
    if env.research:
        fdir = os.path.join(env.research, "findings")
        rel = []
        for dirpath, dirnames, filenames in os.walk(fdir):
            dirnames[:] = [d for d in dirnames if d != ".git"]
            for f in filenames:
                if f.endswith(".md"):
                    rel.append(os.path.relpath(os.path.join(dirpath, f), env.research).replace("\\", "/"))
        ignored = set(check_ignore(env.research, rel))
        for p in rel:
            out["research/" + p] = ("private" if p in ignored else "research",
                                    os.path.join(env.research, p))
    if os.path.isdir(env.memory):
        for f in sorted(os.listdir(env.memory)):
            if f.endswith(".md"):
                out["memory/" + f] = ("private", os.path.join(env.memory, f))
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


def base_for(repo, since):
    sha, _ = last_close(repo)
    if sha:
        return sha, "trailer"
    if not since:
        raise SystemExit("REFUSE: no previous Docs-Census trailer in {} -- pass --since DATE for the first run"
                         .format(repo))
    sha = git(["rev-list", "-1", "--before=" + since + " 00:00", "HEAD"], repo).strip()
    if not sha:
        raise SystemExit("REFUSE: no commit before " + since)
    return sha, "since"


def format_trailer(v):
    order = ("base", "rows", "labels", "still-open", "actually-done", "stale-done", "partial", "still-true",
             "not-a-label",
             "cited-dead", "accretion", "ro-bytes", "ro-longest", "mem-over200", "wikilinks-dead",
             "pairing-unref", "pairing-dead", "sweep-cursor", "sweep-cycle", "census", "research-base",
             "new", "foreign")
    return TRAILER_KEY + ": " + " ".join("{}={}".format(k, v[k]) for k in order if k in v)


# ----------------------------------------------------------------------------- the ratchet + accretion
def ratchet_values(env):
    vals = {"ro-bytes": 0, "ro-longest": 0, "mem-over200": 0,
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
    return vals


def accretion_count(env, rs):
    """Correction vocabulary NOT in the [corr YYYY-MM-DD: ...] form, in the living scope (WP-2)."""
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


# ----------------------------------------------------------------------------- owned paths + the private-index commit
def baseline_text(env, key, owner, base, rbase):
    """The doc's content at its owner's census base, or None when it has none (a new file) -- in which
    case every row in it is new and all of them are owed a verdict."""
    if owner == "main":
        r = subprocess.run(["git", "show", "{}:{}".format(base, key)], cwd=env.repo,
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    elif owner == "research" and env.research:
        r = subprocess.run(["git", "show", "{}:{}".format(rbase, key[len("research/"):])], cwd=env.research,
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
        f.write("| # | path:line | kind | label | sub-state | tokens | date | total | VERDICT |\n")
        f.write("|---|---|---|---|---|---|---|---|---|\n")
        for i, r in enumerate(rows, 1):
            toks = " ".join("{}={}".format(t, s) for t, s in r["tokens"]) or "-"
            f.write("| {} | {}:{} | {} | {} | {} | {} | {} | {} | {} |\n".format(
                i, r["key"], r["line"], r["kind"], r["label"].replace("|", "/") or "-",
                ",".join(r["substate"]) or "-", toks.replace("|", "/"), r["date"] or "-",
                "yes" if r["total"] else "-", r["verdict"] or ""))
        f.write("\n<!-- rowhash: {} -->\n".format(" ".join(r["hash"] for r in rows)))


def read_table(path):
    t = read_text(path)
    if not t:
        return None, []
    m = re.search(r"```json\n(.*?)\n```", t, re.S)
    meta = json.loads(m.group(1)) if m else {}
    hm = re.search(r"<!-- rowhash: (.*?) -->", t)
    hashes = hm.group(1).split() if hm else []
    rows = []
    for l in t.split("\n"):
        if not l.startswith("| ") or l.startswith("| # ") or l.startswith("|---"):
            continue
        cells = [c.strip() for c in l.strip().strip("|").split("|")]
        if len(cells) < 9 or not cells[0].isdigit():
            continue
        key, _, line = cells[1].rpartition(":")
        toks = []
        for tok in cells[5].split():
            if "=" in tok:
                a, b = tok.rsplit("=", 1)
                toks.append((a, b))
        rows.append({"n": int(cells[0]), "key": key, "line": int(line) if line.isdigit() else 0,
                     "kind": cells[2], "label": cells[3], "substate": cells[4].split(",") if cells[4] != "-" else [],
                     "tokens": toks, "date": cells[6], "total": cells[7] == "yes",
                     "verdict": cells[8].strip().upper(), "hash": hashes[int(cells[0]) - 1] if len(hashes) >= int(cells[0]) else ""})
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
                         "close it, or re-run with --force to discard them ({})".format(
                             held, (prev_meta or {}).get("utc", "?"), pending_path(env)))
    rs = read_set(env)
    by_owner = {}
    for k, (o, _) in rs.items():
        by_owner[o] = by_owner.get(o, 0) + 1
    print("read set: {} paths  (main {} / research {} / private {})".format(
        len(rs), by_owner.get("main", 0), by_owner.get("research", 0), by_owner.get("private", 0)))
    base, how = base_for(env.repo, args.since)
    print("base (main): {} [{}]".format(base[:10], how))
    rbase = None
    if env.research:
        rbase, rhow = base_for(env.research, args.since)
        print("base (research): {} [{}]".format(rbase[:10], rhow))
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
    if env.research:
        for p in gitz(["diff", "-z", "--name-only", rbase, "--", "findings/*.md"], env.research):
            touched.add("research/" + p)
            rs.setdefault("research/" + p, ("research", os.path.join(env.research, p)))
        for p in gitz(["ls-files", "-z", "--others", "--exclude-standard", "--", "findings/*.md"], env.research):
            touched.add("research/" + p)
            rs.setdefault("research/" + p, ("research", os.path.join(env.research, p)))
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
    never = sorted(k for k in rs if k not in st["docs"])
    oldest = sorted((k for k in rs if k in st["docs"]), key=lambda k: st["docs"][k]["utc"])
    k = args.k if args.k is not None else K_DEFAULT
    budget = args.rows if args.rows is not None else ROW_BUDGET
    candidates = [c for c in never + oldest if c not in touched and c not in cited]

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
                return [r for r in out if r["hash"] not in seen]
        scanned_whole.add(key)      # no baseline (a new doc) or a sweep doc: every row was offered
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
        for key in candidates[:k]:
            if len(rows) - owed >= budget and swept:
                break
            r = scan(key, False)
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
    carried = {(r["key"], r["hash"]): r["verdict"] for r in prev_rows if r["verdict"]}
    for r in rows:
        r["verdict"] = carried.get((r["key"], r["hash"]), "")
    labels = sum(1 for r in rows if r["kind"] not in ("cite", "loose"))
    dead = sum(1 for r in rows if dead_cites(r["tokens"]))
    # The bytes the census READ, per doc. The close refuses to commit anything else (a post-ship audit,
    # 2026-09-03: `staged_entries` reads the INDEX, so another session's plain unstaged edit to a doc in
    # our radius was invisible and rode into our close). Escape: re-run `census`, which re-reads and
    # re-pins -- and a line the hand edited then arrives as a NEW row needing its own verdict, which is
    # the point: the trailer's counts must describe the text being committed, not the text before the fix.
    content = {key: sha1(read_text(rs[key][1]) or "") for key in sorted(radius)}
    meta = {"utc": utc_now(), "base": base, "research_base": rbase, "touched": sorted(touched),
            "content": content,
            "new": new_paths, "radius": len(radius), "rows": len(rows), "labels": labels, "cited_dead": dead,
            "sweep": swept, "scanned_whole": sorted(scanned_whole), "k": k, "budget": budget, "cycle": cycle, "read_set": len(rs), "loose": bool(args.loose),
            "full_sweep": bool(args.sweep), "dropped": dropped}
    write_table(pending_path(env), meta, rows)
    print("rows: {} (labels {}, dead citations {}); verdicts carried forward: {}".format(
        len(rows), labels, dead, sum(1 for r in rows if r["verdict"])))
    print("table: " + pending_path(env))
    rv = ratchet_values(env)
    print("ratchet now: " + " ".join("{}={} (target {})".format(c, rv[c], TARGETS.get(c, 0)) for c in RATCHET_COLS))
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
    # On a LABEL row a dead pointer under a live status is the measured rot class (section 2.3 #5, #21,
    # #27). On a plain prose line (kind `cite`) STILL TRUE means "dead on purpose" -- a doc naming a
    # file that was retracted or moved away -- and only the hand can tell; it is recorded, not refused.
    contra = [r for r in rows if r["verdict"] in ("STILL TRUE", "ACTUALLY DONE") and r["kind"] != "cite"
              and dead_cites(r["tokens"])]
    if contra:
        r = contra[0]
        raise SystemExit("REFUSE: #{} {}:{} is verdicted {} but its citation resolved dead ({})".format(
            r["n"], r["key"], r["line"], r["verdict"], " ".join("{}={}".format(t, s) for t, s in r["tokens"])))
    counts = {v: sum(1 for r in rows if r["verdict"] == v) for v in VERDICTS}
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
    # 4b. every committed path must carry the bytes the census read
    pinned = meta.get("content", {})
    drifted = []
    for p in sorted(set(main_paths)):
        if p in pinned and sha1(read_text(os.path.join(env.repo, p)) or "") != pinned[p]:
            drifted.append(p)
    if drifted:
        raise SystemExit("REFUSE: {} doc(s) changed since the census that produced these verdicts: {} -- "
                         "re-run `census` (it re-reads them; an edited line arrives as a new row and is "
                         "verdicted on the text you are about to commit)".format(len(drifted), ", ".join(drifted[:8])))
    # 5. numbers
    rv = ratchet_values(env)
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
    vals = {"base": base[:12], "rows": len(rows), "labels": meta.get("labels", 0),
            "still-open": counts["STILL OPEN"], "actually-done": counts["ACTUALLY DONE"],
            "stale-done": counts["STALE DONE"], "partial": counts["PARTIAL"], "still-true": counts["STILL TRUE"],
            "not-a-label": counts["NOT A LABEL"],
            "cited-dead": meta.get("cited_dead", 0), "accretion": accretion_count(env, rs),
            "ro-bytes": rv["ro-bytes"], "ro-longest": rv["ro-longest"], "mem-over200": rv["mem-over200"],
            "wikilinks-dead": rv["wikilinks-dead"], "pairing-unref": rv["pairing-unref"],
            "pairing-dead": rv["pairing-dead"], "sweep-cursor": cursor, "sweep-cycle": meta.get("cycle", 0), "new": len(new), "foreign": foreign}
    # 6. commit 3 first: the private history (snapshot + state + the verdict table) -> census=
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
    # 7. commit 2: the inner research repo
    rsha = meta.get("research_base") or "-"
    if env.research:
        rpaths = sorted(p[len("research/"):] for p in meta.get("touched", []) if p.startswith("research/") and rs.get(p, ("",))[0] == "research")
        if rpaths:
            rtrailer = format_trailer(dict(vals, base=(meta.get("research_base") or "-")[:12]))
            rsha = private_commit(env.research, rpaths, subject, [rtrailer] + trailers)
            print("research close: {} ({} paths)".format(rsha[:10], len(rpaths)))
    vals["research-base"] = (rsha or "-")[:12]
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
                   help="overwrite a pending table that already holds hand verdicts (they are lost)")
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
    return 2


if __name__ == "__main__":
    sys.exit(main())
