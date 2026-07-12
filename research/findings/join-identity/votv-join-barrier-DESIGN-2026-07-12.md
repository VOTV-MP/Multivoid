# DESIGN: the join barrier moves to load-tail quiescence — dissolving the two-authority seam (2026-07-12)

Status: DESIGN -> implementation in this session (per RULE 1, user mandate 2026-07-12 — see the
"Architectural verdict" section of `votv-join-window-placed-prop-RCA-2026-07-11.md`).
Supersedes (RULE 2, once built): the join-window spawn-revalidation capture (takes 1-2), the wire-order
queue netting (take 4 root 5), the SnapshotBegin-lost watchdog quiescence-by-ceiling. Does NOT touch:
the host KEY-UNIQUENESS re-key authority (roots 3+6 — orthogonal, stays as `460da7e4` built it), the
outbound destroy-broadcast suppression episode (the v107 host-wipe fix), the save-vs-wire twin/ghost/
kerfur retires, the doom sweep.

## The root, restated

During the join window the client world has TWO CONCURRENT AUTHORITIES: the transferred-save loader
(loadObjects churn — destroy+recreate of every keyed prop, the PAST) and the live wire stream (the
PRESENT). Six roots, four of them (1, 2, 4, 5) pure consequences of the overlap; every take added a
compensation layer ON the overlap instead of removing the overlap.

Why the overlap exists at all: the client announces `ClientWorldReady` when "world up + registry
coherent" (net_pump.cpp:810-835) — an edge that fires SECONDS BEFORE loadObjects' async load tail
settles. The host opens the send gate + runs the full connect replay (R2 key-diff -> snapshot bracket ->
state lanes) at that announce (event_feed.cpp:163-169 -> subsystems::ConnectReplayForSlot), so the
entire authoritative wire stream lands in a world that is still tearing itself down and rebuilding.

## The MTA precedent (measured in the vendored tree, agent sweep 2026-07-12)

MTA's joiner NEVER receives an entity until it is fully loaded, by construction:

- The client explicitly signals readiness: `PLAYER_INGAME_NOTICE` once GTA is loaded
  (Client .../CClientGame.cpp:3536), then — after receiving its ID — fires the
  `INITIAL_DATA_STREAM` RPC (Client .../CPacketHandler.cpp:463).
- ONLY THEN does the server push the world: `CRPCFunctions::PlayerInGameNotice/InitialDataStream`
  (Server .../CRPCFunctions.cpp:94,110) -> `CGame::InitialDataStream` (Server .../CGame.cpp:1393) ->
  `CMapManager::OnPlayerJoin` (CMapManager.cpp:445) — the full `CEntityAddPacket` batch sequence.
- Adds and removes share ONE reliable-ordered channel (`PACKET_RELIABLE | PACKET_SEQUENCED`, default
  ordering channel — CEntityAddPacket.h:21, CEntityRemovePacket.h:21, CPlayer.cpp:277-300), so strict
  arrival-order application suffices; the client has ZERO deferred-queue / replay-at-quiescence
  machinery (`Packet_EntityRemove` just looks up and deletes, CPacketHandler.cpp:4298). The only
  deferral anywhere is intra-packet parent/attachment link-up (CPacketHandler.cpp:4232-4294).
- A puresync arriving for an unknown entity is DROPPED, not deferred (unreliable-sequenced,
  CPlayerPuresyncPacket.h:24) — safe because existence always precedes sync on the ordered channel.

We already ported this shape once: the v56 pre-world send gate (`IsPreWorldSendableKind`,
session_lanes.h:175-223, "the MTA invariant") + `MarkSlotWorldReady` + the replay-on-announce. The
BUG is that our "READY" fires at the wrong edge: MTA's client says ready when its world is DONE;
ours says ready when its world has BEGUN to exist. Everything the six-queue drain does is
compensation for that one mis-anchored edge.

## The fix: move the announce to load-tail quiescence (client-side only; zero protocol change)

`ClientWorldReady` is announced when the world-load TAIL has settled — the exact population-stability
probe the doom sweep already trusts for DESTRUCTIVE decisions (`CountLoadTailUnsettled_`,
join_membership_sweep.cpp:526: keyless-prop tail + allowlisted-NPC tail + chipPile purge-aware field,
5 Hz, stable xN scans, hard deadline). Consequences, in causal order:

1. The host holds ALL non-pre-world traffic until the announce (existing gates: session.cpp:206,271,
   session_relay.cpp:55,87 — no new mechanism). The pre-world allowlist is engine-free by prior
   per-kind audits and stays valid for the longer window.
2. The connect replay (R2 deletes -> snapshot -> state lanes -> teleport) lands in a SETTLED world.
   Converge targets can no longer be churn-destroyed after binding (root 2 dead); fresh mirrors can
   no longer be churn-killed (root 1 dead); no wire expression is ever provisional, so nothing needs
   capture/revalidation (take-2 machinery removable) and no phase replay exists to invert wire order
   (root 5 dead + take-4 netting removable); the drain-vs-doom order question dissolves (root 4 moot).
3. The reliable channel's own in-lane FIFO becomes the ONLY ordering authority — the MTA property.
4. The sweep still dooms unclaimed locals (host-truth membership), but fires promptly at
   SnapshotComplete: quiescence already holds by construction (it gated the announce that caused the
   snapshot). Its private probe machinery is retired; it consumes the one owner.
5. The join curtain/panel (up from BeginConnect, down at SnapshotComplete — join_progress.cpp) now
   reveals a FINISHED world; today it reveals at a mid-churn SnapshotComplete and the joiner watches
   props settle for ~5-20 s. Total covered time grows only by the snapshot drain itself (~1-3 s);
   the churn wait was always there. The 240 s join failsafe (join_progress.cpp:33) is untouched.

## One owner of the "is my world settled" axis ([[feedback-one-owner-order-axis]])

Today the axis is smeared: join_membership_sweep owns the probe + g_sweepFired, world_load_episode
owns the suppress latch but is CLOSED EXTERNALLY (NotifyQuiesced), net_pump owns the announce
predicate, and a 150 s watchdog papers over the SnapshotBegin-lost flake (the probe only ran while a
sweep was pending, and the sweep arm rode SnapshotBegin). Consolidation:

- `world_load_episode` (moves to `coop/session/` — it is the client world-load lifecycle, not a prop
  concept) becomes THE owner: Arm() at load start (unchanged), self-ticking probe (moved from the
  sweep), self-closing at stability (NotifyQuiesced retired — RULE 2), `HasQuiesced()` for consumers
  (announce gate, sweep, npc_adoption via the existing HasLoadTailQuiesced delegation), probe deadline
  = the announce-anyway degraded mode (bounded, LOUD), absolute ceiling kept as the last backstop.
- The SnapshotBegin-lost flake CLASS dies: the episode no longer depends on any wire message to
  close, so the watchdog quiescence-by-ceiling companion (`8b1b340a`) folds into the probe deadline.
- Travel re-announce (maybeReAnnounce, net_pump.cpp:601) arms the same probe for the NEW world; the
  single announce gate waits for it — the barrier holds on every world-load, not just the first.

## What is REMOVED (RULE 2) vs KEPT (and why)

REMOVED — structurally unreachable once no wire prop event can arrive in-episode:
- remote_prop_spawn::OnSpawn in-episode capture + fresh-tail suppression (remote_prop_spawn.cpp:374,783).
- quiescence_drain: ArmPendingSpawn / ApplyPendingSpawns (spawn revalidation) + the pending-spawn
  queue + CancelPendingSpawnsForWireDestroy + the ArmPendingSpawn destroy-supersede loop (take-4
  netting) + remote_prop_destroy's cancel call.
- join_membership_sweep: the private quiescence probe state machine (moves), the watchdog
  quiescence-by-ceiling declaration path.

KEPT — not join-window ordering crutches:
- The outbound KEYED-destroy broadcast suppression episode itself (host-wipe root fix — the churn
  still happens locally; only its BROADCAST was the bug).
- ArmPendingDestroy + the steady drain (destroy-before-load): still reachable in the TRAVEL window
  (host->traveller sends flow while a cave/level reload churns — the host gate stays open across
  travel) and in the probe-deadline degraded mode. Removing it needs a travel-start close signal we
  do not have (future dig; documented, not built — no silent scope creep).
- b3 pos-corrections + EnsurePosCorrection: the savePos re-bind assist serves post-purge re-creates
  (travel path, net_pump.cpp:644) — audit call sites during implementation; remove only branches
  provably join-window-only.
- Save-time twins / host-vacate / kerfur retire / GHOST sweep: save-vs-wire STATE reconcile (a stale
  save-loaded native at an old position is a fact of the transfer, not of ordering).
- The sweep's keyed-churn RE-BIND pass: UE incremental GC sporadically churns save natives OUTSIDE
  the load tail ([[feedback-verify-before-retiring-a-fix]] — keep until measured dead).
- The doom sweep + >50% valve + claims; the fuzzy-steal wireMirrorOnly gate (steady-state Gap-I-1).
- KEY-UNIQUENESS host re-key authority (roots 3+6): game-data defect, orthogonal to ordering.

## Follow-up consolidation candidates (verify-before-retiring — NOT in this commit)

**STATUS UPDATE 2026-07-12 pm (post-take-6): the smear audit RAN; retirement is BLOCKED on
evidence — the take-6 save had ZERO kerfurs (census 0 NPC + 0 PROP both peers), so the take-6
live logs prove nothing about any kerfur layer.** The full 13-mechanism map was enumerated (agent
sweep across kerfur_convert / kerfur_entity / kerfur_reconcile / kerfur_prop_adoption /
npc_adoption / quiescence_drain / join_membership_sweep). Verdicts:
- KEEP (not overlap-born; save-vs-wire STATE reconcile or steady-state): the PollKerfurConversions
  client quiescence gate (guards against OUR OWN reconcile deaths being read as conversions — that
  premise survives the barrier: the sweep still destroys actors post-announce), the ghost claim +
  4 s reaper, the kerfur_reconcile retire chain (arm-from-EntitySpawn + bounded sweep), RunReconcile
  step 3, the convertFromEid exact ghost adopt, the kerfur census (diagnostic), the sweep kerfur
  exemption (grab-guard predicate).
- MEASUREMENT-GATED (the two real candidates, below): the K-6 join-wait branch + the keyed-churn
  RE-BIND pass.
- INSTRUMENTED for the decision (built + deployed DLL `4B2E4024` x4): both adoptions now log
  `poll #N, M ms after arm` at bind AND fresh-spawn (barrier expectation: poll #1), plus a
  TRIPWIRE WARN if the 60 s last-resort timeout fires BEFORE quiescence (structurally unreachable
  under the barrier — a hit means the probe/sweep chain wedged). RE-BIND hit-count ledger:
  take-6 = 0 hits, 0 SURVIVED-tripwires (session 1 of the "few sessions" measurement).
- Stale spawn-revalidation references in join_membership_sweep (a present-tense tripwire log line +
  the RunReconcile order comment) rewritten to the post-barrier truth (RULE 2).

**USER MANDATE 2026-07-12 (original): the KERFUR join-layer anti-smear** — the layers below plus a
general smear audit across the kerfur files (adoption / convert / reconcile / entity /
prop_adoption / menu_input), gated on LIVE LOG evidence (never on theory). The decisive evidence
now requires a KERFUR-PRESENT join (host save with >=1 kerfur, off or active) read against the new
poll-count logs:
- `kerfur_prop_adoption` (K-6): the "snapshot PropSpawn beats the async save twin" premise is
  structurally gone for the JOIN (snapshot is post-quiescence, twins materialized) — if the live
  log shows every join-path adoption resolving on its FIRST poll, the join-wait branch collapses;
  the convert-timing (steady-state) use stays.
- `npc_adoption`'s quiescence wait: EntitySpawns now arrive post-settle by construction — the
  HasLoadTailQuiesced gate becomes a second guard behind a structural one; keep the gate (cheap),
  but the 60 s last-resort timeout path should never fire again (tripwire if it does).
- The sweep's keyed-churn RE-BIND pass: no churn can follow a wire binding at JOIN anymore; it
  remains live only for sporadic mid-session GC re-instantiation — measure its hit count across
  a few sessions before any retirement.

## Degraded mode (probe deadline fires)

If the tail never stabilizes (45 s deadline, today's sweep envelope), we announce ANYWAY with a LOUD
log — behavior degrades to the PRE-redesign overlap for that join, minus the removed compensation
layers: destroy-before-load still defers (kept queue); a churn-killed converge target is re-delivered
by the next steady-world re-seed / rejoin rather than a same-join revalidation. Accepted: the deadline
is a pathological-stall backstop (measured tails settle in 5-20 s), and keeping the full capture
machinery alive for a path that fires only when the world is already pathological is exactly the
parallel-old-and-new-code RULE 2 forbids.

## Behavior deltas (user-visible)

- The joiner's teleport-to-host + world state + puppet-visible-to-others-in-world all land ~5-20 s
  later (at true settle) — under the cover, which already spans the window.
- The joiner sees NO live host prop/NPC/state events during its load; the post-settle snapshot +
  replays carry their net effect (R2 deletes cover in-window host deletes; the snapshot covers
  in-window spawns/moves/states).
- Cave/level travel re-syncs also wait for the new world's settle before the host re-replays.

## Test plan (runbook take-6)

The take-2/take-4 repro IS the acceptance test: host hotbar-pickup + re-place of a world rock during
the client join window -> the rock appears at the placed spot on the client; no tripwire; both logs
show the announce AFTER the churn tail (client "ClientWorldReady announced (load tail quiesced ...)"
strictly after the last churn destroy; host "[PILE-1C] ... JOIN-WINDOW CLOSED" after the client's
settle). Plus: bare-join host-wipe regression (host keyed-prop census stable), fps sanity, join
regression (props/piles/kerfurs present), travel re-sync sanity (cave in/out).
