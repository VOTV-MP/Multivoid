# Kerfur PROP-form join adoption — RCA + design (2026-06-16, K-6)

Status: **RCA confirmed from a real hands-on client log; design 2-agent-validated; building.**

Builds on the SHIPPED K-5 grab-dupe fix (commit 17a6f100: client-mint gate + held-pose eid-stream).
K-5 is correct and verified (active/NPC kerfurs work). This doc fixes the PROP-form join path.

---

## 1. The symptom (user hands-on)

"Works good only for the active kerfurs. The ones that were kerfur-PROPS disappeared on the client —
got removed entirely. When the client tried to activate (turn on) the ones lying as props, that action
didn't get mirrored to the host at all, then the kerfur props got destroyed." Goal: **all players
interact with kerfurs as naturally as possible.**

## 2. The RCA (proven from the client log, 19:46–19:47)

- At join the client DID bind 5 prop-form kerfur mirrors (`kerfur prop mirror ... bound at host-range
  eid=3276..3280`). But then the divergence sweep **ABORTED**: `would destroy 2986 of 3236 in-universe
  actors (>50%); host snapshot INCOMPLETE … keeping the loaded world (88 claimed)`. So the client kept
  its OWN save-loaded kerfur props **un-reconciled** = duplicates of the host's.
- On a client turn-off, `ClaimConversionGhosts` `claimed 4 local conversion ghost(s) (prop turn-off)`
  — it claims **every** untracked kerfur prop within 5 m (kR2=500cm), not just the 1 conversion output
  — and 4 s later `orphan conversion ghost … un-adopted after 4075ms -- destroying`. → the user's
  pre-existing prop kerfurs get **destroyed**.
- Those untracked local kerfur props are invisible to the conversion **poll** (`PollKerfurConversions`
  scans only `MirrorManager<Prop>`), so a client turn-on of one is **never detected → never sent**.

### Why NPC kerfurs work but prop kerfurs don't (the architectural gap)
- **NPC adoption is DEFERRED + POLLED** (`npc_adoption.cpp`): `ArmAdoption` records a pending, and
  `ResolvePending` (5 Hz) binds the local twin by **class + nearest-pose** when it materializes
  (gated on `HasLoadTailQuiesced`), fresh-spawning only as a last resort. So it NEVER fresh-spawns a
  duplicate while the async-loading twin is still coming.
- **Prop adoption is SYNCHRONOUS + INLINE** (`remote_prop_spawn::OnSpawn` Gap-I-1, `FindNearbySameClass`
  30 cm): it runs ONCE when the host's PropSpawn arrives. If the client's save-twin hasn't loaded yet
  → fuzzy miss → it **FRESH-SPAWNS a duplicate**. The kerfur's BP key is RANDOM per-peer + late-minted,
  so there's no stable key to re-match on later. → duplication + orphaned untracked local.

## 3. The fix (K-6): give the prop form the NPC form's deferred adoption

New module `coop/kerfur_prop_adoption.{h,cpp}`, modeled 1:1 on `npc_adoption`:
- `OnSpawn` gains a `bool deferKerfur=true` param. For a `prop_kerfurOmega*` PropSpawn whose inline
  fuzzy match MISSES: instead of fresh-spawning, call `kerfur_prop_adoption::Arm(payload)` + return.
  (`deferKerfur=false` on the adoption's own quiescence fresh-spawn fallback + on `MaterializeKerfurMirror`'s
  convert materialize — those must not defer.)
- `Arm(payload)`: idempotent on eid (skip if already a mirror); stores the payload + resolved class +
  pose.
- `ResolvePending` (5 Hz while pending): ONE GUObjectArray walk → untracked (non-mirror) live kerfur
  PROP candidates → bind each pending to its nearest same-class candidate via **BindAsMirror**
  = `remote_prop::RegisterPropMirror(eid,obj,key,cls,0)` + `RecordClaimIfTracking(obj)` (sweep-safe) +
  `SetActorSimulatePhysics(obj,false)` (host-driven mirror) + `kerfur_entity::NotifyKerfurPropMirrorBound`.
  If no twin AND `HasLoadTailQuiesced()` (or 60 s backstop) → fresh-spawn ONCE via
  `OnSpawn(payload, 0, localPlayer, false, /*deferKerfur=*/false)`.
- `Tick`/`OnSnapshotComplete`/`OnClientWorldReady`/`OnSessionEnd` wired at the SAME sites as
  `npc_adoption`: `npc_mirror.cpp:630`, `event_feed.cpp:405`, `net_pump.cpp:567`, `subsystems.cpp:246`.

**Why this fixes all three symptoms at once:** the client's save-loaded kerfur props become single
host-range MIRRORS (no duplicate fresh-spawn). As mirrors they are (a) claimed → the divergence sweep
leaves them; (b) excluded by `ClaimConversionGhosts` (it already skips `IsMirror()` actors) → no
over-claim destroy; (c) visible to the conversion poll → a client turn-on IS detected + sent.

### Belt-and-suspenders: the over-claim guard (race during the join window)
`ClaimConversionGhosts` should claim only the actual conversion output (1 kerfur prop + optional 1
floppy spawn at the exact conversion pos), not every untracked kerfur prop within 5 m. Fix: claim the
**nearest untracked actor PER base class**, not all within the radius. (RE §3: turn_off spawns exactly
ONE prop + optional floppy.) Covers the narrow window where a conversion fires before the adoption poll
has bound a still-loading twin.

### Robustness to the incomplete snapshot (88/3236)
The sweep-abort (host snapshot near-empty bracket) is a SEPARATE reliability issue, not kerfur-specific.
Optional K-6 hardening: a host reliable per-kerfur-prop connect announce
(`kerfur_entity::QueueKerfurPropConnectBroadcastForSlot`) that re-uses `ReliableKind::PropSpawn` and
fires from the connect edge independent of the flaky general prop snapshot, so every prop-form kerfur
is announced (→ armed for adoption) even when the bulk snapshot under-delivers. Deferred unless the
deferred-adoption alone doesn't fully resolve the hands-on.

## 4. Build order (each builds + smokes)
1. `kerfur_prop_adoption.{h,cpp}` + `kerfur_entity::IsKerfurPropClass` + CMake.
2. `OnSpawn` `deferKerfur` param + the kerfur-miss-defer branch (decl/def/call sites).
3. Wire Tick + 3 latches.
4. Over-claim guard in `ClaimConversionGhosts`.
5. Build → deploy x4 → new `mp.py` join-with-prop-kerfur smoke (assert: ONE kerfur prop mirror per
   logical kerfur, no duplicate fresh-spawn, poll-visible, client turn-on reaches host, no orphan
   destroy) + `kerfurtoggle` regression. Audit + commit.

## 5. Modular note
`kerfur_convert.cpp` 872, `remote_prop.cpp` 1000, `npc_sync.cpp` 922, `prop_lifecycle.cpp` 890,
`remote_prop_spawn.cpp` 1273 are over the 800 soft cap (pre-existing). The new module is its own TU
(principle 7). The standing extraction debt (`kerfur_ghost_adopt`, `npc_silent_ops`) remains flagged.
