---
name: project-clump-convert-v52-2026-06-09
description: "2026-06-09 session 3 -- CLUMP ball<->pile DUPE root-cause fix (v52): atomic PropConvert swap + identity mirror-pile death-watch, retiring the InpActEvt grab-guess + FindNearestChipPile-on-receiver. Build-clean, 2 agent-audits 0-CRIT (+2 fixes), deployed 4 folders hash-MATCH. HANDS-ON IN PROGRESS (user took the test; no autonomous smoke). UNCOMMITTED."
metadata:
  node_type: memory
  type: project
---

# 2026-06-09 session 3 -- trash CLUMP ball<->pile DUPE: the root-cause fix (v52)

All UNCOMMITTED (protocol **v52**, builds on the v51 firefly/wind pile). Deployed 4 folders
(HOST/CLIENT/CLIENT2/DEV) hash-MATCH (E0D3043D...). User reported the clump dupe AGAIN:
"clump pile -> release grab -> clump ball -> never converts to a clump pile -> converts to a
bigger clump ball -> grab -> duplicates clump ball -> some turn into piles after 5s -> more
dupes." User then said "im gonna go test" -> took the hands-on; NO autonomous smoke ran.

## Root cause (from the 2 prior DECISIVE/pass2 RE docs -- already done, IDA-proven)
The clump morph-destroy (ball lands -> spawns chipPile + self-destructs; pile grabbed -> spawns
held clump + self-destructs) is **BP-internal / unobservable** (CALLVIRT->ProcessInternal,
bypasses our ProcessEvent detour). The old sync detected the convert by a liveness death-watch
and then sent a SEPARATE landed-pile PropSpawn + PropDestroy with **two DIFFERENT eids**,
locating the pile via `FindNearestChipPile` -- which is cross-peer-UNSOUND on the receiver
(chipPiles are NOT co-located; unseeded RNG + client blank save). Result: the ball mirror
lingered + the wrong pile was consumed = the infinite re-grab DUPE. The prior pass1 fix (an
`InpActEvt_use` lookAtActor grab-guess in prop_lifecycle) was a single-edge heuristic that only
fired on the grabber and was never robust -> still duping.

## THE FIX (pass2 §7, the RULE-1 root-cause): atomic PropConvert + mirror-pile death-watch
- **`ReliableKind::PropConvert=41` + `PropConvertPayload`(112B)**, v52. Bulk lane (ordered vs the
  ball's PropSpawn) + host-relay whitelist (a client can own a clump). protocol.h + session_lanes.h.
- **Owner (`trash_collect_sync::BroadcastConvertNear`, in the reworked `TickWatchReleasedClumps`):**
  when a watched clump dies, FindNearestChipPile on the OWNER side (SOUND -- the owner's pile IS
  there), mint a NEW pile eid, MarkPropElement(ownerPile), broadcast ONE PropConvert{oldEid=ball,
  newEid=pile, pileClass, transform, chipType, vel}, and `WatchPile(ownerPile, newEid)`. If no pile
  (clump expired w/o converting) -> bare PropDestroy(oldEid) backstop.
- **Receiver (`remote_prop::OnConvert` -> void*):** atomically `OnDestroy(oldEid ball)` then
  `OnSpawn(newEid pile, kFreshLanded)` (turnToPile impact sound, grabbable) in one handler ->
  ball vanishes the exact frame the pile appears; returns the spawned pile so event_feed
  `WatchPile(pile, newEid)`. Two distinct eids -> no collision.
- **Identity re-grab destroy (`TickWatchReleasedPiles`, net_pump per-tick, transition-gated by
  `g_fleeing||join_progress::Active()`):** both peers watch the shared pile by eid. Whoever grabs
  it, the pile dies LOCALLY near their camera (800cm^2 proximity gate, the grime super-sponge
  precedent) -> broadcast PropDestroy(eid) -> others drop their mirror. `NotifyPileConsumed(eid)`
  in event_feed's PropDestroy handler drops the watch on a WIRE-driven destroy so the receiver
  doesn't re-broadcast (echo guard; proximity is the second line).
- **DELETED (RULE 2):** the `InpActEvt_use` grab observer (prop_lifecycle, -60 LOC -> file now 729,
  UNDER the 800 cap) + the now-dead `ResolveMirrorEidByActor` + `g_mirrorActorToEid` reverse map
  (remote_prop). `BroadcastLandedPileNear` became `BroadcastConvertNear`.

## Why this is the COMPLETE fix (not just lag)
The "~5s then converts" the user saw is the clump's INHERENT physical settle time (the ball SHOULD
be visible until it piles, mirroring the owner). The BUG was the non-atomic separate-message
convert + wrong-pile FindNearestChipPile + un-removed mirror on re-grab. All three are closed by
the atomic swap + identity death-watch. A FHitResult-gate PRE observer (pass2's instant-convert)
was DELIBERATELY SKIPPED: it gives ~0 latency benefit (the convert IS the death the death-watch
catches within a frame) while risking divergence from a mis-replicated slope/physics gate. Lives
in trash_collect_sync (the trash feature file), NO new module.

## Audits (2 agents, 0 CRITICAL -- deploy bar met) + the 2 fixes applied
- **Correctness (code-reviewer):** Issue-1 (conf 85) FIXED -- a transient OnSpawn FindClass miss
  -> null pile -> WatchPile silently skipped; added a WARN at the event_feed callsite. Echo loop /
  atomic-swap distinct-eids / no-double-watch / stale-idx-covered-by-proximity all reviewed SOUND.
- **Perf:** 1 WARN FIXED -- `TickWatchReleasedPiles` was calling GetActorLocation per LIVE pile per
  tick (~125Hz, up to 8k UFunction calls/s at cap 64); piles are STATIONARY so the spawn-time
  lastPos is permanent -> dropped the per-tick refresh. Hot-path otherwise PASS (no per-tick
  GUObjectArray walk; FindNearestChipPile is DEATH-only, unchanged cadence; IsLiveByIndex is O(1)).
- **File-size:** remote_prop.cpp 893 (>800, +52 mine), event_feed.cpp 1270 (+~60 mine, mostly
  pre-existing dispatch) -- under 1500 hard cap. DEFERRED extraction proposals: OnConvert+OnDestroy
  -> remote_prop_lifecycle.cpp sibling (like remote_prop_spawn.cpp); event_feed prop-family cases
  -> event_feed_prop.cpp. weather_sync 1079 still pending (DebugForce* -> weather_debug).

## HANDS-ON #1 FAILED -> THE REAL ROOT CAUSE FOUND (turnToPile is the GRAB morph)
First hands-on (CLIENT grabs/throws a pile repeatedly) still DUPED on the HOST. Logs were decisive
(verify-don't-guess paid off): the CLIENT side was FLAWLESS (CONVERT + watching pile + "grabbed
locally -> PropDestroy" all fired). The HOST side: **EVERY** OnConvert logged "[pile spawn FAILED]"
and every incoming PropDestroy(pileEid) logged "has no local actor" -- yet RegisterPropMirror logged
"bound" and UnregisterPropMirror logged "drained". I.e. the pile mirror was in the MirrorManager but
`ResolveLiveActorByEid` (Registry::Get + IsLiveByIndex) returned null -> the pile actor was ALREADY
DEAD right after spawn. **Disassembly (`_disasm.py actorChipPile turnToPile`) proved why:
`actorChipPile_C::turnToPile` is the pile->CLUMP GRAB morph, NOT a sound effect** -- it
BeginDeferred-spawns a clump (Max:=2.0), SetPhysicsLinearVelocity(throws it), and **K2_DestroyActor's
SELF**. The v29 `kFreshLanded` "landed-pile impact sound" path
(`remote_prop_spawn.cpp` + `ue_wrap::prop::TurnChipPileToPile`) had been calling it on the
freshly-spawned landed-pile mirror -> it DESTROYED that pile (-> unwatched + the re-grab PropDestroy
no-ops -> piles never removed on the host) AND spawned a STRAY, untracked, hit-notify-ENABLED clump
that flew, landed, self-converted to a STRAY pile -> the accumulating DUPE. The misleading old
comment ("operates on `this`, spawns nothing -> no dupe") was the wrong belief every prior clump
attempt inherited. **THIS is the true root cause behind the entire multi-session clump-dupe saga.**

**FIX (RULE 1+2):** ripped out turnToPile entirely -- `TurnChipPileToPile` (ue_wrap/prop .h+.cpp +
its ResolveTurnToPileFn/g_turnToPileFnByClass cache), the `kFreshLanded` flag (0x08, protocol.h), the
OnSpawn dispatch block, and the now-dead `vel` field of PropConvertPayload (112->100B) + its cascade
(BroadcastConvertNear vel param, WatchedClump.lastVel + its per-tick GetActorVelocity, event_feed
9->6 float validation). OnConvert now spawns the pile with `physFlags=0` (settled, no sim, no morph)
-> the pile survives -> ResolveLiveActorByEid succeeds -> WatchPile installs + the incoming
PropDestroy resolves -> re-grab destroy propagates, no stray clump, NO DUPE. Impact dust+sound on the
mirror is DEFERRED polish (needs the CORRECT verb, not the grab morph). build-clean, deployed 4
folders hash-MATCH (EC3E9FD7). RE-TEST PENDING. Expected logs now: "OnConvert: ball->pile" WITHOUT
"[pile spawn FAILED]"; HOST "watching pile eid=N"; on re-grab HOST "OnDestroy: ... destroying local
actor" (NOT "no local actor"). NOTE: payload 112->100B within uncommitted v52 -> both peers must run
this exact build.

## HANDS-ON #2 (post-turnToPile-fix): 2 MORE root causes found (eid-range + watch-coverage)
turnToPile fix worked (logs: OnConvert NO "[pile spawn FAILED]", piles watched, "grabbed locally
-> PropDestroy" fires). But the CLIENT-grabs-a-HOST-owned-pile case STILL duped. Logs decisive again:
1. **EID-RANGE REJECTION (the dominant cause):** host log `event_feed: PropDestroy elementId=0x00000918
   out of allowed peer range (senderPeerSlot=1) -- dropping`. 0x918=2328 = the exact pile the client
   grabbed. A grab-destroy references an EXISTING SHARED entity whose eid is in the ORIGINAL OWNER's
   range, NOT the grabber's -> the `IsAllowedSenderEid(senderRole)` check WRONGLY dropped a client's
   PropDestroy of a host-owned pile -> host never removed its copy. **FIX:** PropDestroy now accepts
   EITHER range (host OR peer) -- a destroy isn't an allocation. (PropSpawn KEEPS its sender-range
   check = allocation authority; PropConvert KEEPS its both-eids-in-sender-range = the converter owns
   both ball+pile.) event_feed.cpp.
2. **WATCH-COVERAGE GAP (the "initial pile" case):** the mirror-pile death-watch only enrolled
   PropConvert-born piles, NOT pre-existing/snapshot piles. So grabbing an INITIAL pile (broadcast to
   the peer via the normal PropSpawn path) never propagated its destroy. **FIX:** `remote_prop_spawn::
   OnSpawn` now `WatchPile`s EVERY chipPile mirror (IsChipPile + eidOnly), covering initial+snapshot+
   convert piles uniformly. Removed the now-redundant event_feed OnConvert WatchPile (OnSpawn does it).
   Bumped kMaxWatchedPiles 64->256 (a base has many ground piles; per-tick cost is O(1) IsLiveByIndex,
   piles stationary). Both fixes build-clean, deployed 4 folders hash-MATCH (38EFB8A8). RE-TEST PENDING.
   Still-open SYMMETRIC gap (low pri, note if user hits it): the OWNER doesn't watch its OWN non-convert
   piles (only convert ones via BroadcastConvertNear) -> if the HOST grabs an INITIAL host pile, the
   client's mirror may linger. Fix when needed = WatchPile in prop_lifecycle Init POST for chipPiles
   (verify it doesn't double with the Init-POST-PropSpawn-vs-PropConvert broadcast first).

## HANDS-ON #4 (FIFO build): MORPHING GONE (confirmed) + 2 remaining items
User: "на клиенте 15 фпс, клиент берёт существующий pile -> существующий pile не уничтожается, кладёт
шарик - он превращается в pile, опять берёт этот pile и он исчезает + превращается в clump ball, т.е.
далее уже работает как надо." = (1) **client 15 FPS**; (2) FIRST grab of a PRE-EXISTING pile does NOT
destroy the host's copy; (3) but the CONVERT-born pile (place ball->pile->re-grab) WORKS correctly +
NO MORPHING. So the FIFO fix WORKED (morphing closed) and the convert/re-grab core works.
- **(2) FIX = keyed-pile coverage:** the OnSpawn WatchPile was gated `eidOnly` -> it watched only the
  convert-born (key=None) piles, NOT pre-existing world chipPiles (broadcast KEYED via a synth-Key from
  Init-POST). DROPPED the `eidOnly` gate: now watch EVERY chipPile mirror (IsChipPile -- which is
  actorChipPile_C lineage ONLY, NOT the 785 trashBitsPiles, byte-verified prop.cpp:153) by its eid (the
  eid resolves cross-peer via RegisterPropMirror; the relaxed PropDestroy eid-range lets the grab-destroy
  through). build-clean, deployed (96865B2B). UNTESTED.
- **(1) 15 FPS = NOT this session's code + NOT the launcher (user CONFIRMED "launcher not at fault").**
  It is a **PRE-EXISTING DLL-wide hot path** -- `coop/dev/perf_probe.h` header (2026-06-04 audit) states
  "the whole game runs at ~15 FPS on BOTH host and client = a SHARED hot path our DLL adds," hypothesis =
  a HOT per-tick caller / observer-callback BODY doing an uncached reflection Find*/CountObjectsByClass
  (~1M-entry GUObjectArray walk + wstring/entry) = the whole ~50ms/frame budget in ONE call/frame. My
  clump code adds NO per-frame walk (FindNearestChipPile is death-only; IsChipPile latches; the
  death-watch is IsLiveByIndex O(1)). **MEASURE-FIRST (user: "no guessing"):** ENABLED `perf_probe=1` in
  all 4 folder inis (Binaries/Win64/votv-coop.ini; runtime flag, no rebuild). Next run logs `[perf]` ~1Hz:
  PE/frame, detour self-ns/dispatch, **per-net_pump-subsystem ms/frame** (Reaper/RemoteProp/EventFeed/
  Nameplate/Interactable/TrashWatch/Puppets/...), + the SINGLE WORST observer/interceptor cb BODY (ms +
  UFunction name -- catches an observer walking GUObjectArray). **NEXT: read the [perf] lines from a run
  (host=s_1234 populated, ~2306 props streamed -> a per-object-count walk would spike here) -> find the
  hot bucket / worst-cb -> fix THAT. Do NOT guess.** (perf_probe.cpp Sample() prints the report; gated +
  ~free when off.) NOTE: a wrong earlier guess (FreeId insert(begin) = the 15 FPS) was REFUTED by the
  first agent (FreeId is low-rate) -- the deque change is correct but is NOT the FPS fix.

## TEST INFRA change (2026-06-09, user request) -- host save + client fresh
`tools/mp.py launch_peer` now sets per role: HOST -> `env VOTVCOOP_SAVE=s_1234`; every CLIENT ->
`env VOTVCOOP_FRESH=1` (always a blank New Game, NEVER a save load). Harness `BootStorySaveBlocking`
honors both as env overrides over votv-coop.ini: `slotA = ReadEnv("VOTVCOOP_SAVE") | ini save |
s_may2026`; `freshBoot = forceFresh || ReadEnv("VOTVCOOP_FRESH")==1 || ini fresh_boot==1`. (NOTE: s_1234
must EXIST as a host save or the host won't reach gameplay.) Applies to all launch paths incl. smoke.

## HANDS-ON #3: the DEEP root cause -- EID-REUSE COLLISION (morphing "bottle->cassette->clump")
Build 38EFB8A8 regressed: a grabbed pile's HELD item visually morphed bottle->cassette->clump + still
duped. Logs DECISIVE (host save w/ ~4504 live elements; client fresh): the held clump was rebroadcast
as the SAME low eid (126) EVERY grab, recycling a tiny pool (126/716/717/718/820) ~2/s; client log
`GRAB-IN key='None' eid=126 -> local actor ... mesh=... (Aprop physics-off)` = eid 126 resolved to an
APROP mirror (has a mesh; clumps have NULL mesh), NOT the clump -> the held-clump pose stream drove the
WRONG actor = the morphing. **ROOT CAUSE (agent-CONFIRMED w/ code evidence):** the ElementId allocator
(`coop/element/registry.cpp`) was **LIFO** -- `FreeId` push_back + alloc pop_back -> a just-freed eid is
RE-ISSUED IMMEDIATELY. A transient clump frees+reallocs ~2/s, grabbing a low eid still bound to another
prop's in-flight mirror on the peer. The EXACT failure: the host frees 126 + instantly reuses it for the
new clump; the peer still holds the OLD 126 mirror, so the new clump's PropSpawn `RegisterMirror(126)` is
SILENTLY REJECTED (registry.cpp:214 rejects an occupied slot) -> no binding -> `ResolveLiveActorByEid(126)`
returns the STALE prop's mirror -> pose drives it. (Intra-process double-alloc is already guarded by the
AllocHostId `if(m_byId[id])` check, so this is purely a CROSS-PEER timing collision.)
**FIX (v52, agent-PASSED as the correct root-cause repair):** defer eid reuse so a freed id is only
re-issued after the entire never-allocated pool (~28k host / ~8k peer ids) drains = reuse DEFERRED by
hours, so every in-flight PropPose/PropDestroy/PropConvert for the old id drains before reuse. A new clump
always draws a FRESH eid -> RegisterMirror succeeds -> correct binding -> **MORPHING GONE (user-CONFIRMED
hands-on #4)** + the convert/destroy races close. First cut used `m_*Free.insert(begin())` on a vector
(O(n)); **REPLACED with std::deque + push_front (O(1) both ends)** so FreeId is never O(n) regardless of
rate (the vector reserve() calls removed; registry.h includes <deque>). Both = same deferred-FIFO logic. The other v52 clump fixes (turnToPile removal,
eid-range relaxation, watch-all-chipPiles) are KEPT -- FIFO makes the eid space non-colliding so they now
operate correctly. RESIDUALS the agent flagged (defer; now rare post-FIFO, add only if morphing recurs):
(1) RegisterMirror silently DROPS on a slot collision -> belt-and-suspenders = evict-then-rebind (new
authoritative spawn wins; safe post-FIFO since a collision => the old IS stale); (2) unreliable PropPose
can still arrive before the rebinding reliable packet (mostly defused -- a fresh high eid resolves to
null->skip, not to a stale actor); (3) host-side NpcMirrors DrainAll at disconnect is the one O(n^2)
FreeId burst (perf footnote, rare 1-time teardown). [[project-bug-trash-chippile-uaf-crash]]

## KNOWN EDGE (noted to user, NOT this fix's regression -- pre-existing)
A CLIENT-owned clump's pile, if RE-GRABBED BY THE HOST, may not clean up: the host's PropDestroy
carries the client's PEER-range eid, and `IsAllowedSenderEid(senderIsHost=true, peerEid)` REJECTS
it (the eid-range trust check assumes the destroyer's role matches the eid's allocation range --
wrong for cross-originated destroys, but pre-existing across the whole PropDestroy path). The
host-owned case (the user's actual report) uses host-range eids throughout -> fully covered. Fix
if it recurs: relax the PropDestroy/PropConvert eid-range check to accept EITHER range (a destroy
references an existing eid, not a new allocation).

## Log markers (confirm hands-on)
`trash_collect: CONVERT ball eid=N -> pile eid=M` (owner) | `remote_prop::OnConvert: ball eid=N ->
pile eid=M` (receiver swap) | `trash_collect: watching pile eid=M` (both enrol) | `watched pile
eid=M grabbed locally ... -> PropDestroy` + `pile eid=M consumed by incoming wire destroy -> dropped
watch` (re-grab propagation). If dupes persist hands-on: check WHICH peer owns (host vs client eid
range) + whether CONVERT/OnConvert/watching lines appear on both peers.

## Compact handoff (end of session 3, 2026-06-09)
- **Deployed DLL = 93aceb72** (4 folders, deque O(1) FIFO + keyed-pile watch + all v52 clump fixes).
  `perf_probe=1` set in all 4 LOCAL inis (gitignored). Protocol v52. ALL UNCOMMITTED (v44->v52 pile).
- **Clump status:** morphing FIXED+user-confirmed; convert + convert-born re-grab WORK; pre-existing-
  KEYED-pile grab fix shipped UNTESTED; SYMMETRIC owner-grab-own-initial-pile gap still open (low pri).
- **#1 NEXT = the 15 FPS:** MEASURE via the now-enabled perf_probe -> read the `[perf]` per-bucket +
  worst-cb lines from a run -> fix the hot path. PRE-EXISTING (perf_probe.h header, 2026-06-04), NOT
  this session's code (user-confirmed "launcher not at fault"; my clump code adds NO per-frame walk).
- **User Q (pending decision):** ".net = default" in votv-coop.ini -- the raw `net.master=87.121.218.33:10001`
  line is DEAD when `net.master.custom=0` (config.cpp:141 already uses the compiled-in built-in VPS,
  IGNORING the ini line). To "use default": set custom=0 + delete the net.master/net.signaling address
  lines (built-in IS the same VPS). Offered (a) clean the 4 local inis or (b) add a `default` sentinel
  in config.cpp -- USER HAS NOT CHOSEN.

## Standing
- Commit ONLY when user asks (NOT yet -- v52 + everything since v44 UNCOMMITTED). VPS creds/game
  inis/debug shots stay LOCAL (gitignored).
- NEXT: user hands-on result. If clean -> the clump arc finally closes. If client-owned edge bites
  -> relax the eid-range check (above). Detail RE: votv-clump-lifecycle-observability-and-robust-
  design-2026-06-08-pass2.md (§7 the design) + votv-clump-ball-to-pile-conversion-...-2026-06-08.md.
