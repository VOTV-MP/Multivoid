# Increment 2 (chipPile CLIENT-grab) — HOST-SIDE implementation blueprint — DESIGN

**Date:** 2026-06-22. **Status:** DESIGN (code-architect blueprint, agent-derived + cited). Gated by the
RESOLVED puppet-grab `[?]` (`votv-puppet-grab-feasibility-RE-2026-06-22.md`, commit `32ccd1bc`). Mirrors the
in-tree DOOR channel (`interactable_channel.h:220 OnRequest`, `Channel::Mode::HostAuth`) per CLAUDE.md RULE
2026-05-28 (follow MTA: `CObjectSync.cpp:214` single-syncer, `CStaticFunctionDefinitions.cpp:1644/1689`
attach-by-id + ctx, `CElement.cpp:1281/1300` ctx — already ported as `trash_channel` ctx).

Scope: the HOST-SIDE arm only — buildable + harness-verifiable NOW via a SYNTHETIC `GrabIntent` (the
`VOTVCOOP_RUN_PLAYERDMG_TEST` relay-e2e pattern), WITHOUT the phase-2 collision prerequisite. The
CLIENT-initiated path (suppress-native at `OnPileGrabPre` + the `garbageCollider` hull so a client can aim a
proxy) + the feel are OUT OF SCOPE here (hands-on-gated, separate prerequisites).

## The 5-commit build sequence

1. **Protocol v83→v84 (build-only).** `protocol.h`: `GrabIntentPayload{uint32 eid; pad}` (8 B, static_assert),
   `ThrowIntentPayload{eid; lin xyz; ang xyz}` + `PileResyncRequestPayload{}` (designed + ID-reserved, staged).
   Enum: `GrabIntent=78, ThrowIntent=79, PileResyncRequest=80` (CLIENT→HOST only — NOT in any relayable set).
   Bump `kProtocolVersion` 83→84.
2. **Router (build-only).** `event_feed.cpp`: add the 3 kinds to the state-family fall-through. 
   `event_dispatch_state.cpp`: GrabIntent case = full validation (role==Host, senderPeerSlot!=0, payloadLen,
   eid!=0) → `trash_channel::OnGrabIntent(s, eid, senderSlot)`; ThrowIntent/PileResyncRequest = log-and-drop
   stubs. (3-place wiring per `[[feedback-reliablekind-router-checklist]]`.)
3. **`OnGrabIntent` + HELD_BY (`trash_channel.{cpp,h}`).** Gates (mirror door `OnRequest`): eid not already
   carrying (`g_carry`), eid tracked (`g_ctx`), sender not already holding (per-peer guard = door `holdOpen_`).
   Resolve `Registry::Puppet(slot)->GetActor()` + `ResolveLiveActorByEid(eid)` (IsChipPile). Execute
   `playerGrabbed` on the puppet via the PROBE'S PROVEN PATTERN — `ue_wrap::ParamFrame pf(grabFn);
   pf.Set<void*>(L"Player", puppet); ue_wrap::Call(pile, pf)` (NOT a 3-arg CallFunction; Hit left zeroed, the
   probe confirmed that ENGAGES). Read `grabbing_actor` synchronously after Call (RE: `pickupObject` sets it
   with no tick gate; deny+log if null). `OnHostConvert(eid, kToClump, clump, loc, rot, chipType)` (opens the
   carry latch + broadcasts) → `puppet_carry_drive::NotePuppetHeld(eid, slot, clump)`. `OnGrabHolderLeft(slot)`
   + `g_heldBy.clear()` on disconnect. **Add `g_heldBy.erase(eid)` in `TickCarry`'s land-settle commit** (else
   the eid stays HELD after re-pile).
4. **`puppet_carry_drive.{cpp,h}` (NEW file — build-step 4, the probe's host-driven hold).** Per-tick: for each
   registered (eid, slot, clump), guard (clump `IsLiveByIndex`, puppet valid, `IsCarrying(eid)`), compute
   `holdPoint = puppet.GetHeadPosition() + GetSyncedAimDirection()*150` (grabLen 150 = probe-frozen value),
   `SetActorLocation(clump, holdPoint)`. Mirrors `local_streams` host-held drive + MTA `CClientVehicle`
   kinematic target. NEW `RemotePlayer::GetSyncedAimDirection()` (forward from `curYaw_+curHeadYawDelta_`,
   `curPitch_` — mirror `remote_player.cpp:799-811`). Wire `Tick()` into `subsystems::TickGameplay` AFTER
   `TickCarry`; `OnPeerLeft`/`OnDisconnect` into the disconnect paths. Drive ENDS on `!IsCarrying` (re-pile
   land-settle closes the latch → stream-through, same as the host's own throw arc).
5. **Synthetic harness test + invariants.** `autotest_chippile.cpp` `RunGrabIntentTest`
   (`VOTVCOOP_RUN_GRAB_INTENT_TEST=1`): wait puppet live → find a tracked PILED eid → call
   `trash_channel::OnGrabIntent(s, eid, 1)` directly (host-side synthetic, no wire needed) → poll 4 s. New
   `pile-test-assert.ps1` invariants: `grab-intent-received/exec/success`, `puppet-drive-active`,
   `client-got-convert` (CLIENT `[GRAB-IN]` echo), `puppet-hand-tracks` (holdPoint moves).
   Log markers: `[GRAB-INTENT] RECEIVED/EXEC/SUCCESS/DENIED`, `[PUPPET-DRIVE] DRIVING`.

## Risks / open Qs to resolve while building (from the architect)
- (a) `playerGrabbed` param frame: use the probe's `ParamFrame Player=puppet` (Hit zeroed) — PROVEN. Don't use
  the sketch's 3-arg CallFunction.
- (b) synchronous `grabbing_actor` read after Call — RE says set synchronously; deny+log on null.
- (c) real client RELEASE teardown (ThrowIntent vs existing re-pile) — only matters for the client-initiated
  path (phase 2); the synthetic test ends via the land-settle latch.
- (e) PHC fight: probe showed the PHC target never moved (frozen at grab spot), so direct `SetActorLocation`
  wins; if oscillation appears, also drive `grabHandle.SetTargetLocationAndRotation`. Log clump pos next tick.
- (f) `g_heldBy.erase(eid)` on land-settle commit (above) — REQUIRED.

## Modular-file flags (RULE 2026-05-25)
- `event_dispatch_state.cpp` 717→~757 LOC: the NEXT addition after this MUST extract the trash-intent cases to
  a new `event_dispatch_trash.cpp`.
- `remote_player.cpp` ~840 LOC (already past the 800 soft cap): `GetSyncedAimDirection` is +15, but the file
  is over — carry an extraction proposal (candidate: the kinematic-drive math into `remote_player_drive.cpp`).
- New `puppet_carry_drive.{cpp,h}` (~120/40 LOC) keeps the drive out of the over-cap files (one-feature-per-file).

Full blueprint (file:line, the exact case-block code, the gate rationale) is the code-architect transcript for
this session; this doc is the durable digest to build from.
