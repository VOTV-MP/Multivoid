# Dead capabilities — built, documented, never called

**Status: LIVING register. Instrument: `tools/dead_api_census.py`. Opened 2026-09-02.**

A **dead capability** is a feature whose plumbing is complete except for the one call
that would switch it on. It compiles. It passes review. It has a doc comment saying
what it does. It never runs, and nothing fails — so nothing reports it.

This is not the same as dead code. Dead code does nothing and costs nothing. A dead
*capability* is worse: the tree, the comments and the reviewer all say the feature
exists, so everyone downstream reasons as if it does. The two entries closed below
both shipped that way for **three months**.

> **Why this register exists, in the user's words (2026-09-02):** after seeing the
> first instance — *"All servers in the list have 1/4 wtf is that"* — and the second:
> *"Some other things are probably fucked up in the same way."* They were right; the
> census found the sibling on the same code path within minutes.

---

## 1. How to run the census

```
python tools/dead_api_census.py          # census + both self-tests (exit 1 if either fails)
python tools/dead_api_census.py --list   # census only
```

It scans every declaration in `src/votv-coop/include/**.h` and counts *call* sites
across the tree, classifying each textual `NAME(` as a declaration/definition or a
use. Current reading (2026-09-02, HEAD `fff4032b`): **138 of 2018 declared names**.

**The raw list is TRIAGE, not truth.** It over-reports constructors, thunks, inline
one-liners and anything reached only through a function pointer. Hand-validate every
hit before acting:

```
grep -rn "\bNAME\b" --include=*.cpp --include=*.h src/votv-coop/{src,include}
```

**Two references = genuinely dead** (a declaration and a definition, nothing else).
**Three or more = it has a caller** the line classifier could not see.

### The instrument's own history is the lesson

`[V]` **v1 could not find `SetPlayerCountFn`** — the very defect it was written to
hunt — because a one-line inline definition in a header (`void F() { ... }`) scored
as a call. An instrument blind to its own target.

`[V]` **v2 found it and then reported SIX LIVE `Tick*` functions as dead**, because
`subsystems.cpp:575-581` packs a scope guard, a walk timer and the call onto one
line, and that prefix scored as a return type. **A recall-only self-test passed v2.**

So the gate asserts **both directions** and the script fails if either breaks:

| canary | asserts | current |
|---|---|---|
| `ClientArmed` (known-dead) | RECALL — it still finds a real one | PASS |
| `TickClientNpcs` (known-live) | PRECISION — it does not flag live code | PASS |

Re-point a canary when its status legitimately changes; a canary that has been fixed
makes the gate fail forever and trains the next reader to ignore it.

---

## 2. CLOSED — fixed 2026-09-02, `fff4032b`

### D1 — `LobbyAnnouncer::SetPlayerCountFn` (the "1/4 on every server" defect)

| | |
|---|---|
| **Born** | `8dd62916`, 2026-06-07 |
| **Callers, ever** | ZERO. `git log -S` and `-G` both return that one commit and nothing else. |
| **Consequence** | `lobby_announcer.cpp:118` took its `: 1` fallback on every heartbeat, so **every lobby in the server browser reported `1/4` regardless of who was in it** — a full 4-player lobby read identically to an empty one. |
| **Blast radius** | Display-only, and that was measured, not assumed: the master never gates on `players_cur` (it stores, clamps and echoes it — `master.rs:128,154,482,529-530,672`), and the client renders it in exactly three places (`server_browser.cpp:227`, `server_browser_panels.cpp:277`, `server_browser_rows.cpp:783`). **It broke no join.** |
| **What it DID break** | The only instrument that could answer *"is this failing for everyone, or just me?"* during a live field incident. See §4. |
| **Fix** | `session_runtime::InstallLobbyPlayerCountSource()` → `connectedPeerCount() + 1`, installed **above the scenario branch** in `harness.cpp`. |

**The placement is the interesting part.** No per-scenario install is correct:
`menu` (the native launch — i.e. every real player) never enters the `netEnabled`
branch; `play` announces from *inside* it, before `RunPlayLoop`; `overlay_test_arm`
announces from other scenarios entirely. An install in any one of those is an install
the other lanes silently miss — which is a fair description of how the seam stayed
unwired for three months. It now sits beside `mod_environment::Run()`, which was
hoisted above the branch on 2026-08-29 for exactly the same reason.

`playerCountFn_` is now `std::atomic<int(*)()>`: on the env-host lane the heartbeat
worker is **already running** when the setter fires, so the `std::thread`
constructor's happens-before edge does not cover the write.

### D2 — `save_transfer::GetProgress` (the join-abandonment defect)

| | |
|---|---|
| **Born** | `77225106`; the "Download progress for the loading screen (bytes)" comment entered with it |
| **Callers, ever** | ZERO |
| **Consequence** | The **longest stage of a real join** reported nothing. The cover sat in `Phase::Connecting` — an animated marquee reading `"Connecting to <host>…"` — for the entire world download, with a **Cancel button** beside it. |
| **Why that is ~17 s, not ~1 s** | The effective send rate on any internet link is `SendRateMin` = **1 MB/s**, because this build has no bandwidth estimation (`session_status.cpp:72-79`; the rate is `clamp(ping-at-init estimate, Min, Max)` and no loss/ack/timer writer exists). LAN gets `Max` = 25 MB/s. A 17.6 MB world is therefore **<1 s in the lab and 17+ s in the field**. |
| **Fix** | Two new phases — `Downloading` (determinate, in MB, fed by `GetProgress`) and `LoadingWorld` (indeterminate). Progress is **pulled** by the harness loop already spinning at 60 Hz on the transfer, so the net thread gains no per-chunk work and keeps its single writer. |

---

## 3. OPEN — confirmed dead, hand-validated, NOT fixed

Each verified by bare-name reference count (2 refs = declaration + definition only).
**None is known to cause a live defect** — they are listed so the next person does not
have to re-derive their status, and so a lane that *needs* one knows it must wire it.

| symbol | header | note |
|---|---|---|
| `save_transfer::ClientArmed()` | `save_transfer.h:145` | state query nobody asks; the census's RECALL canary |
| `kerfur_entity::GetKerfurIdForActor` | `kerfur_entity.h:82` | **the whole kerfur identity API is unused** — five accessors, |
| `kerfur_entity::GetCurrentEidForKerfurId` | `:84` | and this project's history is largely kerfur duplication. |
| `kerfur_entity::GetFormForKerfurId` | `:85` | Worth a look before the next kerfur identity dig: something |
| `kerfur_entity::IsKerfurEid` | `:91` | was designed here and the consumers went elsewhere. |
| `kerfur_entity::ReleaseKerfurId` | `:160` | |
| `remote_player::GetFood()` / `GetSleep()` | `remote_player.h:138-139` | vitals accessors nobody reads |
| `trash_channel::AnyCarryingEid()` | `trash_channel.h:170` | |
| `element_deleter::PendingCount()` | `element_deleter.h:84` | |
| `object_scan_hub::PassActive()` | `object_scan_hub.h:76` | |
| `session_manager::HostListenPort()` | `session_manager.h:212` | |
| `input_owner::LastGameOwnerName()` | `input_owner.h:151` | already known — its own comment at `input_owner.cpp:291` says it "existed with ZERO consumers" |
| `prop_lifecycle::SyncDestroyedTrackedProp` | `prop_lifecycle.h:102` | declared here, defined in `prop_destroy_seam.cpp:226` after an extraction — likely a **stale declaration**, not a dead feature |

---

## 4. The rule this register enforces

**A capability is not shipped until something calls it.** A declaration, a
definition, a doc comment and a green build are four pieces of evidence that it was
*written* — none is evidence that it *runs*.

Two second-order consequences, both paid for on 2026-09-02:

1. **A dead gauge is worse than a missing one.** D1 broke no gameplay, but during a
   live field incident the user reasonably read "every server shows 1/4" as *nobody
   can join anywhere*. The browser could not have shown anything else. A missing
   number invites a question; a wrong number answers it wrongly.
2. **A dead capability hides inside a working feature.** D2 sat on the join path, and
   the join path *worked* — the transfer completes, the world loads, the peer plays.
   Only the part a human looks at was missing, and no automated scenario looks.

**Related:** `docs/LESSONS.md` §"How to work" carries the rows; the 2026-08-23
`Registry::InvalidateLocal()` finding ("documented invalidation with ZERO callers …
exercised by NO autonomous smoke") is the same class, found the same way, and is why
the user's instinct that there would be more was correct.
