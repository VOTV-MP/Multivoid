# Execution board — what is being worked, in what order, and what "done" means

`TRACKER.md` says **what is broken**. The `PLAN_*` files say **how to fix it**. This file says
**where the work stands right now.**

Updated: **2026-07-20**, HEAD `cc0d8686`. Deployed DLL `05479190C7C01528` x4, proto 122.

---

## 1. Current state in one line

> **Nothing has been fixed.** All 20 findings are OPEN. The only security code that exists is TLS
> arcs 1-2, which close none of them. The next action is the **CA spike** (`PLAN_01` §2).

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

### Wave 0 — free, unblocked, ship immediately

| Item | Plan | Cost | Status |
|---|---|---|---|
| **A2 Action 1** — delete the false join-secret comment | `PLAN_04` §1 | Minutes | **OPEN** |
| **W6 comment** — correct the false float-validation claim (with the fix, or before it) | `PLAN_02` §3 | Minutes | **OPEN** |
| **A8** — write the render rules into the site spec | `PLAN_05` | Zero — already written | **OPEN** (waiting on the site) |

Wave 0 is documentation-truth, not defence. It stops the next reader trusting a control that is not
there.

### Wave 1 — remote process kill (CRITICAL, cheap, verified)

| Item | Plan | Cost | Status |
|---|---|---|---|
| **W1** — cap `OnBegin` reserve; define `kMaxSaveBlobBytes` | `PLAN_02` §2 | Small | **OPEN** |
| **W2** — validate sidecar `count` against remaining bytes | `PLAN_02` §2 | Small | **OPEN** |
| **W3** — cap `g_cliBuf` in the sink | `PLAN_02` §2 | Small | **OPEN** |

All three are `[V]`. Three edits, two files, one new constant — **one commit.** This is the highest
value-per-hour work in the folder.

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

### Wave 4 — amplification and streams

| Item | Plan | Cost | Status |
|---|---|---|---|
| **W4** — per-sender assembly cap in `blob_chunks` | `PLAN_02` §3 | Medium | **OPEN** — verify site first |
| **W5** — receive-side `g_mirrors` cap | `PLAN_02` §3 | Small | **OPEN** — verify site first |
| **W6** — role gate + finite check | `PLAN_02` §3 | Small | **OPEN** |

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

### Corrections produced by verification (s30c)

Evidence that Rule S5 is not ceremony — verifying five rows changed three fixes:

- **W1** — the proposed fix cited `kMaxSaveBlobBytes`; `[V]` **the constant does not exist**. The fix
  must define it.
- **W3** — real, but there **is** a sequential index check (`save_transfer.cpp:367-372`), so chunks
  must arrive in order. Exploit shape corrected; severity unchanged.
- **W6** — the false comment is a **fused** claim: the ctx-freshness half is true
  (`trash_clump_pose_stream.cpp:49`), the float-validation half is false. A spot-check of the true
  half would have confirmed the whole comment.

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
