# Two Claude sessions, one game rig

**Status: AS-BUILT 2026-08-26.** The lock is implemented (`tools/game_lock.py`), enforced in
`tools/mp.py`, and drilled — five lifecycle drills plus an integration drill, all shown below.
Written after an afternoon in which two sessions working the same tree destroyed several of each
other's test runs and produced one confident, wrong bug attribution.

---

## 1. The problem, measured

Every `mp.py` scenario begins by killing **every** VotV process on the box (`kill_all()`). So two
sessions running scenarios concurrently do not interleave — they terminate each other.

The lost run is the cheap part. The expensive part is what the survivor reports:

> `FAIL: expected 2 peers at end, got 1`

which is indistinguishable from a real defect in whatever that session just changed. On 2026-08-26
this produced a cross-session message asserting *"your build is a boot-killer"*, which took an mtime
census across every modified file to retract. Both sessions then spent time attributing a failure
that had no technical cause at all.

**Two further hazards surfaced the same afternoon and belong in the same protocol:**

- **Whose bytes are on disk?** Both sessions run `deploy-all`, which copies
  `build/votv-coop/Release/*` over all four installs. Whoever deployed last owns the DLL, and the
  other session's next run silently measures foreign bytes. Both sessions got this wrong in
  opposite directions before checking `md5sum`.
- **Shared files carry two lanes' uncommitted work.** `tools/mp.py` held hunks from both sessions
  simultaneously. A `git add tools/mp.py` from either one would have swept the other's uncommitted
  work into an unrelated commit.

  **This HAPPENED on 2026-08-26, and the direction matters: it is not only a risk you create, it is
  one you are exposed to.** Session A wrote three lesson rows into `docs/LESSONS.md` and left them
  uncommitted while finishing a build; session B ran its own `/documentize`, staged `docs/LESSONS.md`,
  and committed `941a796f` — which therefore contains session A's rows under session B's commit
  message. Nothing was lost and nothing conflicted, so neither session noticed until A's own
  `git diff --cached --stat` came back at 6 lines where ~50 were expected. **That diffstat is the
  detector**: if the number of staged lines does not match the change you believe you made, stop and
  ask who else touched the file. The cost here was only misattributed authorship in one commit; the
  same mechanism with overlapping edits is a lost hunk.

  Practical consequence: `docs/LESSONS.md`, `MEMORY.md` and the topic docs are the MOST shared files
  in the tree, because `/documentize` writes all of them and both sessions run it. Treat them like
  `tools/mp.py` — stage your own hunks, and check the diffstat before committing.

---

## 2. Three channels, three jobs — do not collapse them

| Channel | Answers | Lifetime |
|---|---|---|
| `ignore_folder/_GAME_LOCK.json` | **"Is the rig free RIGHT NOW?"** | Exists only while held |
| `ignore_folder/_FRIENDLY_SESSION.txt` | **"What happened, and what did we learn?"** | Append-only, never truncated |
| `SendMessage` (cross-session) | **"I need you to know this now."** | Ephemeral |

**Why not one file for the first two.** State and history are different questions. Deriving "is it
free" from an append-only log means pairing `TAKING`/`DONE` lines by eye — which is ambiguous the
moment a session dies without writing `DONE`, and that is exactly the case the protocol has to
survive. Conversely, deleting the log on unlock would throw away the cross-lane findings, which have
been worth more than the coordination itself (byte-ownership corrections, a UMG getter that echoes
its own input, a retracted bug attribution).

So: **the lock is state and is deleted; the log is history and is kept.** Both live in
`ignore_folder/`, which is `.gitignore`d — this is machine-local state and must never be committed.

---

## 3. The lock

```
python tools/game_lock.py status      # FREE, or who holds it and why
```

Created atomically (`open(..., "x")` — O_EXCL, no TOCTOU window). Records session, purpose, PID,
start and deadline.

**Enforced in `mp.py`, not merely agreed.** The lock is taken at the single `args.func(args)`
dispatch point every command passes through. A protocol that must be *remembered* is one that gets
forgotten under time pressure — this project's own lesson about a convention wearing a ratchet's
hat. Being unable to launch beats being asked politely not to.

While the rig is held, any scenario refuses:

```
HELD by votv-mp-43
  purpose : browser --fake-master 30
  pid     : 1888 (persistent launcher)
[mp] REFUSING to launch: another session holds the game rig (see above).
[mp] If that session is genuinely gone, delete ignore_folder/_GAME_LOCK.json.
```

### Two kinds of lock, judged differently

Conflating these made the first implementation useless, and a drill caught it handing a held lock
straight to a second session:

- **TRANSIENT** — every scenario (`smoke`, `browser`, `nativeui`, …). The holder runs the whole
  scenario in-process, so **a dead PID proves the lock is stale** and any waiter may break it. The
  deadline is only a backstop against PID reuse; an alive-but-overdue lock is *never* broken,
  because breaking a live run is the precise harm this exists to prevent.
- **PERSISTENT** — `host`, `client`, `client2`, `client3`. These launch a game and **exit on
  purpose**, so the PID is dead by design and PID-liveness would mark every one stale the instant it
  was written. They are stale only when the deadline has passed **and** no VotV process is running.
  `mp.py kill` releases them.

`kill` is the cleanup path and is **never blocked** — it is what you run when a session died holding
the rig. It releases the lock instead of taking one.

### Drills (all run 2026-08-26, all passing)

1. `acquire` → `ACQUIRED`.
2. `status` while held → `HELD`, not stale, exit 1.
3. Second session `acquire` → `BUSY`, exit 1.
4. `release` → `FREE`.
5. Planted expired persistent lock, PID 999999, no game running → correctly reported
   `STALE: persistent lock expired and no VotV process is running` and broken.
6. **Integration:** `mp.py smoke` while held → refused, exit 2, **zero** VotV processes spawned.

Drill 3 is the one that matters: it *failed* on the first implementation and is why the
persistent/transient split exists.

---

## 4. The protocol

**Before a run** — nothing to do. `mp.py` takes the lock and writes the `TAKING` line for you.

**After a run** — nothing to do. The lock releases and writes `DONE`.

**What is still yours to do, because no tool can:**

- **Post findings to the log.** The lock records *that* you ran; only you can record *what you
  learned*, especially anything that affects the other lane.
- **Say whose bytes are on disk.** After `deploy-all`, `md5sum` the DLL and note it. A session
  reading a log line "measured X" needs to know which build X came from.
- **Never `git add` a shared file wholesale.** If a file holds both lanes' work, stage only your
  hunks:
  ```
  git --no-pager diff -U3 tools/mp.py > mine.patch   # then keep only your hunks
  git apply --cached --check mine.patch              # dry run FIRST
  git apply --cached mine.patch
  ```
  `--cached` touches only the index, so the other session's working tree is untouched. Verify with
  `git diff --cached --stat` that the line count matches *your* change, not the file's total.
- **Use `SendMessage` for anything time-sensitive or corrective** — "I'm taking the rig", "your
  claim about my build is wrong, here is the mtime census". The log is where it gets written down;
  the message is how it arrives in time to matter.

---

## 5. What this does not solve

- **Concurrent builds.** Both sessions build into `build/votv-coop`. The lock covers the *game*, not
  the build directory. Two simultaneous `cmake --build` runs there would still collide; in practice
  they have not, and adding a second lock for it is not justified by any measured failure yet.
- **Who owns the deployed DLL.** The lock guarantees only one session *runs* at a time, not that the
  bytes on disk are yours. Rebuild and redeploy before a run you intend to trust, and `md5sum` it.
- **A shared tree.** Both sessions edit the same working copy. Nothing here prevents one session
  from rebuilding while the other's changes are uncommitted — that is what commits are for.

---

## 6. Files

| Path | Role |
|---|---|
| `tools/game_lock.py` | The lock: `acquire` / `release` / `status`, plus the staleness rules |
| `tools/mp.py` | Enforces it at the dispatch point; `kill` releases |
| `ignore_folder/_GAME_LOCK.json` | The lock itself — present iff the rig is occupied |
| `ignore_folder/_FRIENDLY_SESSION.txt` | The append-only cross-session log |
