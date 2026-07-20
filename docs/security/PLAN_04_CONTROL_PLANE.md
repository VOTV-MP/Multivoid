# Plan 04 — control plane (master + moderation)

**Closes:** `TRACKER` **A2** (locked lobbies are not locked) · **A5** (unbounded economy write) ·
**A6** (unlimited TURN minting) · **A7** (lobby-list flooding) · **A9** (host IP disclosure) ·
**S2** (IP bans on relayed connections).
**Status: DESIGN**, except the A2 comment deletion which is ready to ship as-is.

Most of this plan is small deletions and limits. It is the cheapest security value in the folder per
line changed.

---

## 1. A2 — "Locked" lobbies are not locked

### The finding `[V]` — personally verified 2026-07-20

- `master.rs:498-499` states: *"lo.locked is a browser UI hint only; the real admission gate is the
  game-layer post-Connected join-secret challenge (a secret the master never sees)."*
- `[V]` A tree-wide grep for `join[_]?secret` across `src/`, `tools/` (cpp/h/rs/py) returns
  **exactly one hit**: `session.h:102`, a comment describing it as intended work.
- **The challenge does not exist.** Anyone can enter any stranger's world, locked or not.

### Two separable actions

**Action 1 — delete the false comment. Ship today, independently.** Per Rule S1 a deletion is never
blocked on building the control. Replace with an honest statement: `lo.locked` is a UI hint and
**there is currently no admission gate.** This costs nothing and stops the next reader trusting it.

**Action 2 — build the join secret.** The design in `session.h:102-104` is sound in shape: a
per-lobby secret the master never sees, challenged after `Connected`, before world access.

**Sequencing note:** this overlaps `PLAN_01_PEER_AUTH.md`. A join secret authorizes *this peer for
this lobby*; a peer certificate proves *who the peer is*. They are different questions and A2 does
not dissolve into P1 — but the transport for the challenge should be decided after the CA spike, so
it is not built twice. **Do Action 1 now; hold Action 2 for the spike.**

---

## 2. A9 — `/v1/join` discloses a direct host's raw IP `[V]`

`[V]` `master.rs:500-506` — for `conn == "direct"` the endpoint returns
`{"conn":"direct","addr":"<ip>:<port>"}` to an **anonymous caller**, before any admission decision.
Confirmed by reading the branch directly.

A scraper can harvest the home IP of every direct host on the list (`THREAT_MODEL.md` asset 5).

**Fix:** gate `addr` behind A2's join secret — the address is released only after the caller proves
lobby authorization. Disappears as a side effect of A2 Action 2.

**Interim, if A2 slips:** rate-limit `/v1/join` per IP so mass harvesting is at least expensive. Not
a fix, and it should be labelled as mitigation in the code comment (Rule S1).

---

## 3. A5 — `BalanceDelta` is an unbounded client-authored economy write `[A]`

- **Claimed:** `event_feed.cpp:294-306` → `balance_sync.cpp:96-103`. Length is validated; the value
  is not. Any peer sets the shared balance to ±2³¹.
- **Fix is RULE 2, not a clamp:** the lane's only user is the dev `+1000` button
  (`add_points.cpp:12-22`). Balance is **already host-authored** everywhere else. Retire the
  client→host delta entirely; make the dev button host-only.
- **Why retirement beats clamping:** a clamp keeps a client-authored economy lane alive as a future
  foothold and needs a "legitimate range" nobody can define. Deleting it removes the question.
- **Verify first:** confirm the dev button really is the only caller before deleting the lane.

---

## 4. A6 — unlimited TURN credential minting `[A]`

- **Claimed:** `/v1/heartbeat` re-mints on every call at 240/min (`master.rs:414-435,482-519`), and
  heartbeat only needs a token from your own lobby.
- **Fix, two parts:**
  1. **Drop minting from heartbeat entirely.** Credentials are time-limited and issued at
     `/v1/host` / `/v1/join`; a heartbeat has no reason to re-mint. Pure deletion.
  2. Set coturn `user-quota` / `total-quota` as the backstop, so the limit does not live only in our
     code.
- Note this interacts with `PLAN_01` Arc B: cert minting must not repeat the same mistake by
  becoming a per-heartbeat signature operation.

---

## 5. A7 — lobby-list flooding evicts real lobbies `[A]`

- **Claimed:** at the 1000 global cap, `evict_if_full` drops the **stalest real lobby**
  (`master.rs:45-46,308-324,350-352`). 8 per /64 × 125 /64s — trivial with a routed /48.
- **Fix:** at the cap, **refuse** new `/v1/host` with 429. Eviction should be reserved for TTL-stale
  rows, which `sweeper()` already handles correctly.
- **The principle:** a full table must not let a new arrival displace an established one. Refusing
  the newcomer degrades discovery for the attacker; evicting the incumbent degrades it for a real
  player.
- Depends on the X-Real-IP correctness noted in `PLAN_05_WEBSITE.md` §2.6 — if a reverse proxy is
  ever put in front and appends rather than overwrites, every per-IP limit collapses into one bucket
  and this fix silently stops working.

---

## 6. S2 — IP bans on TURN-relayed connections `[A]` — MEASURE FIRST

**This row is not a fix task yet; it is a measurement task.** Two outcomes with opposite severities:

| Outcome | Consequence |
|---|---|
| `m_addrRemote` is the **peer's** address on a relayed connection | Bans work; row closes as a non-issue |
| `m_addrRemote` is **coturn's** address | Either the ban is a no-op, **or** we write our own TURN IP into the banlist — after which the fail-closed filter **rejects every future relayed joiner**. That is a self-inflicted outage |

**The measurement:** force two peers onto relay (`iceMode="relay"`, `session.h:97-100`) and log
`m_addrRemote` at the host accept edge (`session_status.cpp:69-82`).

**If confirmed:** key bans on the GUID — `moderation.cpp:104-118` already does exactly this for
offline bans, so the mechanism exists — and **refuse to store a ban equal to the TURN host address**
as a guard against the outage case.

---

## 7. Sequencing

| Order | Item | Cost | Note |
|---|---|---|---|
| 1 | **A2 Action 1** — delete the false comment | Minutes | Unblocked, ship immediately |
| 2 | **A6** — drop heartbeat minting + coturn quotas | Small | Pure deletion + config |
| 3 | **A5** — retire the client BalanceDelta lane | Small | Verify the sole caller first |
| 4 | **A7** — 429 at the cap | Small | |
| 5 | **S2** — the relay measurement | Small, needs two peers | Result decides whether it becomes work |
| 6 | **A2 Action 2 + A9** | Medium | Held for the CA spike so it is not built twice |

Items 1-4 are independent of everything else in the folder and can ship as one control-plane commit.

---

Back to: `README.md` · `TRACKER.md` **A2/A5/A6/A7/A9/S2**.
