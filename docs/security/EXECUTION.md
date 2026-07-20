# Execution board — what is being worked, in what order, and what "done" means

`TRACKER.md` says **what is broken**. The `PLAN_*` files say **how to fix it**. This file says
**where the work stands right now.**

Updated: **2026-07-20**, HEAD `0fcd2003`. Deployed DLL `3C856F22530B7F5B` x4, proto 122.

---

## 0. ORDERING CHANGED 2026-07-20 (user decision) — read this before the waves

> **Adopt the MTA authority model FIRST; return to the security findings after.**

The board below was written assuming security work proceeds finding-by-finding. That is now wrong for
the **authority-shaped** findings: **A3, A4 and A5 are not bugs to patch — they are the absence of an
architecture**, and the architecture is `docs/COOP_SYNCER_MODEL.md`. Patching them first builds a
mechanism the migration then replaces (RULE 2).

**Two things are NOT held behind the migration:**

1. **Parse-layer findings** — **W4**, **W5**, **W6**. They fire *before* "who owns this element?" is
   ever asked, so the syncer model does not touch them. Cheap, verified sites, no rework.
2. **P1 / the CA spike** — orthogonal. Syncer assignments are only as trustworthy as the identity
   they name, so P1 raises the model's ceiling rather than competing with it.

Everything else in waves 3-7 waits.

## 1. Current state in one line

> **Six findings BUILT today** — the save-transfer one-packet remote kill (W1/W1b/W2, `6f0c2bf8`),
> W3's structural fix (`8eb7f1a1`), and the three parse-layer floods (W4/W5/W6, `0fcd2003`). Both
> false security comments are gone. **15 remain OPEN**, and the authority-shaped ones (A3/A4/A5) are
> now downstream of `docs/COOP_SYNCER_MODEL.md` per §0.
>
> **Nothing is VERIFIED.** No hostile-peer drill has ever run, and W6 is not even runtime-exercised.

**Deployed:** DLL `3C856F22530B7F5B` x4, proto 122 (unchanged — rejection paths, no wire change).

---

## 2. Definition of done

A finding may only be moved to **VERIFIED** when all of these hold. Anything less is **BUILT**.

| # | Requirement |
|---|---|
| 1 | The cited site was **personally re-read** before the fix (Rule S5), and the row is `[V]` |
| 2 | The fix is at the layer the plan names — not a filter, skip-if, or catch-and-ignore (RULE 1) |
| 3 | An **attempt** to exploit it is shown failing, with the log line as evidence |
| 4 | Legitimate behavior still works — a two-peer smoke ≥30 s with clean logs where the lane is live |
| 5 | If a comment described the old (absent) control, the comment is corrected (Rule S1) |
| 6 | Wire-format change → `kProtocolVersion` bumped |
| 7 | The `TRACKER.md` row carries the commit hash and the evidence |

**Requirement 3 is the one that gets skipped.** A cap that was added but never tested against the
attack it stops is an untested rejection path — historically where the bugs are.

---

## 3. The waves

Ordered by *cost now vs cost later*, not strictly by severity.

### Wave 0 — free, unblocked — **DONE `6f0c2bf8`**

| Item | Plan | Status |
|---|---|---|
| **A2 Action 1** — delete the false join-secret comment | `PLAN_04` §1 | **DONE** — replaced with an honest statement that no admission gate exists |
| **W6 comment** — correct the false float-validation claim | `PLAN_02` §3 | **DONE** — recorded as a *fused* claim (half true), which is the nastier shape |
| **A8** — write the render rules into the site spec | `PLAN_05` | **OPEN** — waiting on the site to exist |

Wave 0 is documentation-truth, not defence. It stops the next reader trusting a control that is not
there.

### Wave 1 — remote process kill (CRITICAL) — **BUILT `6f0c2bf8`**

| Item | Plan | Status |
|---|---|---|
| **W1** — the uncapped `OnBegin` reserve | `PLAN_02` §2 | **BUILT** — reserve **DELETED**, not capped |
| **W1b** — no guard against a second `Begin` (**new**) | `PLAN_02` §2 | **BUILT** — fails loudly |
| **W2** — sidecar `count` ceiling | `PLAN_02` §2 | **BUILT** — mirrors the `keyLen` precedent |
| **W3** — pre-Begin accumulation | `PLAN_02` §2 | **OPEN by decision** — root fix is a net-thread `Begin` latch, own commit |

**Evidence:** Release build clean; deploy x4; LAN smoke PASS with the save path genuinely exercised
(20 990 211 B / 367 chunks / 873 sidecar entries, CRC ok, `join_progress: Complete 3105/3106`).

**Not VERIFIED:** the rejection paths have never rejected anything. The hostile-host drill is
specified in `PLAN_02` §7 and is owed before any of these three may be called VERIFIED.

### Wave 1b — W3's root fix (next)

Latch `Begin`'s four scalars on the net thread so `WaitingBegin`-with-bytes becomes structurally
impossible, plus the missing `len` vs `kSaveChunkBytes` check in `BulkSink_`. Touches the session
router on the join critical path — **own commit, own smoke.**

### Wave 2 — the CA spike (the architectural gate)

| Item | Plan | Cost | Status |
|---|---|---|---|
| **Spike** — two processes, own root, `AllowWithoutAuth = 0` | `PLAN_01` §2 | Half a day | **OPEN — HALT GATE** |

**Blocks:** P1, A1, A2 Action 2, A9, and the decision on TLS arcs 3/3b/5. Nothing in `PLAN_01`
proceeds until §7 of that file has real output in it.

### Wave 3 — control-plane cleanup

| Item | Plan | Cost | Status |
|---|---|---|---|
| **A6** — drop heartbeat minting + coturn quotas | `PLAN_04` §4 | Small | **OPEN** |
| **A5** — retire the client BalanceDelta lane | `PLAN_04` §3 | Small | **OPEN** |
| **A7** — 429 at the lobby cap | `PLAN_04` §5 | Small | **OPEN** |
| **S2** — measure `m_addrRemote` on a relayed connection | `PLAN_04` §6 | Small, needs 2 peers | **OPEN** |

Mostly deletions. Can ship as one control-plane commit.

### Wave 4 — amplification and streams — **BUILT `0fcd2003`**

| Item | Plan | Status |
|---|---|---|
| **W4** — per-sender assembly cap in `blob_chunks` | `PLAN_02` §3 | **BUILT** — capped at the shared primitive, so all 8 lanes at once |
| **W5** — receive-side `g_mirrors` cap | `PLAN_02` §3 | **BUILT** |
| **W6** — role gate + finite check | `PLAN_02` §3 | **BUILT** — but **not runtime-exercised**, see below |

All three sites were re-read personally first (Rule S5), and that changed two of them: W4 is
per-SENDER rather than global (a global table is finding W10's shape), and W6 turned out **worse
than recorded** — neither the store nor the game-thread apply had any role check, so a client could
send this host-originated kind *to* the host.

**Coverage gap, stated plainly:** the join smoke exercises W4's lanes and W5, but **not W6** — the
trash-carry stream only runs when a client grabs a pile, which needs an interaction smoke
(`[[feedback-interaction-smoke-not-join-smoke]]`). W6 ships as pure rejection paths on a lane the
host never legitimately receives, so the risk is bounded, but it is untested at runtime.

### Wave 5 — peer auth implementation

Arcs A-D of `PLAN_01` §3. Shape depends entirely on the Wave 2 spike; do not pre-plan further.

### Wave 6 — authority

`PLAN_03`, in its own internal order: A3 Half 1 (validate before relay) → A3 Half 2 (holder
predicate) → A4 steps 1-3 (host-authoritative claims → Principle-8 answers → enforcing).

**Highest regression risk in the folder.** Every step touches lanes the user has hands-on tested.

### Wave 7 — the long tail

W7-W10, A9 (if not dissolved by A2), the `PLAN_02` §5 pattern rollout, and a re-audit sweep of lanes
neither agent covered.

---

## 4. Dependency graph

```
CA spike (Wave 2)
  ├─> P1 peer certs ─> TLS arcs 3/3b/5 decision
  ├─> A1 (dissolves)
  └─> A2 Action 2 ─> A9

A2 Action 1 ────────> (independent, ship now)
Wave 1 (W1-W3) ─────> (independent, ship now)
Wave 3 ─────────────> (independent, ship now)

A3 Half 1 ─> A3 Half 2 ─> A3 intent conversion (protocol bump)
A4 step 1 ─> A4 step 2 (Principle-8) ─> A4 step 3 (enforcing)
```

Three independent tracks can run in parallel. Only the peer-auth track is gated.

---

## 5. Session log

Append one row per session that touches security. **Never edit a past row.**

| Date | Session | What happened | Findings moved |
|---|---|---|---|
| 2026-07-20 | s30 | TLS arcs 1-2 built and deployed (`7aff6b73`, `87e66bce`) | None — closes no finding |
| 2026-07-20 | s30b | Threat model written; 2 audits ran; `docs/security/` created (`d95683cc`, `cc0d8686`); arcs 3-5 held; Tier C dissolved; `net.master.insecure` dropped | 20 findings opened |
| 2026-07-20 | s30c | Folder expanded to plans + this board; **A2, W1, W2, W3, W6 personally verified `[A]` → `[V]`**; three corrections found (see below) | 0 fixed, 5 upgraded |
| 2026-07-20 | s30e | W3 root fix (`8eb7f1a1`, net-thread Begin latch — window closed structurally, no constant) + wave 2 W4/W5/W6 (`0fcd2003`). MTA precedent researched (`a96b620e`); syncer model adopted as architecture (`5eb92c11`, `379f252c`) | **3 more BUILT** (W4, W5, W6) |
| 2026-07-20 | s30d | Waves 0+1 built (`6f0c2bf8`) after a 3-round `/qf` that reframed the fix twice. **W1's fix became a deletion, not a cap. W1b found — neither audit had it.** W3 held back rather than shipping a sized window | **3 BUILT** (W1, W1b, W2), 2 false comments deleted |

### Corrections produced by verification (s30c)

Evidence that Rule S5 is not ceremony — verifying five rows changed three fixes:

- **W1** — the proposed fix cited `kMaxSaveBlobBytes`; `[V]` **the constant does not exist**. The fix
  must define it.
- **W3** — real, but there **is** a sequential index check (`save_transfer.cpp:367-372`), so chunks
  must arrive in order. Exploit shape corrected; severity unchanged.
- **W6** — the false comment is a **fused** claim: the ctx-freshness half is true
  (`trash_clump_pose_stream.cpp:49`), the float-validation half is false. A spot-check of the true
  half would have confirmed the whole comment.

### What the s30d `/qf` produced (3 rounds, each one overturned something)

Recorded because the value was not in the questions being clever — it was in each one exposing a
cheap measurement I had skipped.

- **R1: "is the reserve deletable?"** → it was. The entire "what cap value, 64 MB, what if a save
  grows past it" debate was **a fork I invented by choosing to keep a primitive I did not need.**
- **R1: "which of the three is actually one-packet?"** → only W1. My own plan text had overstated it.
- **R1: "your principle-8 claim rests on a comment"** → it did, violating **my own rule S1** written
  the same day. Re-derived from `session_runtime.cpp:336-355`.
- **R2: "collapse W1+W3 into one invariant"** → **REJECTED on measurement.** The pre-Begin window is
  legitimate cross-thread lag, not wire reordering; the tidier invariant would have broken real joins.
  A critic's cleaner idea is still a hypothesis.
- **R2: "you censused the USES, not the WRITERS"** → found **W1b**, which neither audit agent had.
- **R3: "why size the window instead of closing it?"** → the sized window was dropped entirely.

---

## 6. Standing reminders

- **Every `[A]` row must be re-read before its fix** (Rule S5). Fourteen rows are still `[A]`.
- **Do not batch a security fix into an unrelated commit** — these need to be revertible alone.
- **A fix that changes a lane's behavior needs a two-peer smoke**, per the pre-deploy checklist. A
  clean build is not evidence.
- **Unrelated but pending:** take 4 hands-on (DLL `05479190C7C01528`, proto 122, relaunch both
  peers) — not a security item, but it holds the same test slot.

---

Back to: `README.md` · `TRACKER.md`.
