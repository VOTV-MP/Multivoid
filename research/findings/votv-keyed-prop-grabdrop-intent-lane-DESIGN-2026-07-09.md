# Keyed-prop grab/drop = a HOST-AUTHORITATIVE INTENT LANE (extend GrabIntent) — DESIGN 2026-07-09

STATUS: DESIGN (post `/qf 15` convergence 2026-07-09). NOT built. Supersedes the seam-catching F2
attempts v2/v3/v4 (all reverted to clean baseline DLL `13A372A36084BF05`). Green-lit by the user.

## Why (the `/qf 15` convergence)
A client moving a HOST-OWNED keyed world prop (rock) via seam-catching is the wrong layer. v2 (FinishSpawn
author) raced; v3 (destroy-suppress) DUPED (blocked the legit grab-destroy); v4 (exclude-hand-actor) is
timing-fragile (poll-set `g_ownHeldPtr` vs event destroy). Root = a MISSING SINGLE OWNER: three client
seams (`EnsureHeldItemBroadcast` ×3 + the K2 destroy-seam) author one keyed identity with no owner/order.
The fix is the pattern chipPiles ALREADY use: a host-authoritative `GrabIntent` lane. See
`[[feedback-map-all-wire-events-before-fixing-missing-sync]]`, `[[feedback-recurring-bug-is-architectural]]`.

## RE FACTS (2026-07-09, two agents; cited)

### The GrabIntent template (chipPiles) — the shape to mirror
- Wire: `GrabIntent = 78` (`protocol.h:1880`), `GrabIntentPayload{ uint32 eid; pad[4] }` (8B, `:3682`).
  Companion `ThrowIntent = 79` (`:1889`) = the release/drop, `ThrowIntentPayload{ eid; u8 mode; pad[3];
  float dir[3] }` (20B, `:3702`); `mode` = kRelease(0)/kHardThrow(1). NO DropIntent — drop = ThrowIntent{kRelease}.
- Client author (`trash_use_intercept.cpp:155-268`): CANCELS the native interaction dispatch (`return true`)
  and `SendGrabIntent(eid)` (`trash_channel.cpp:289`); never self-mutates. Throw toggle at `:158-175`.
- Host handler (`event_dispatch_state.cpp:691-721` → `trash_channel.cpp:340 OnGrabIntent`): runs the pile's
  native `playerGrabbed(Player=puppet-N)` on the host's OWN pile (`:441-449`), where puppet-N = the
  REQUESTER's puppet (`Registry::Get().Puppet(senderSlot)`). Then drives the carried pose itself
  (`puppet_carry_drive::NotePuppetHeld`, `:491`) since the puppet's own tick is dead.
- Downstream: the existing prop lane (`PropConvert` for piles). Release = `OnThrowIntent` (`:496-566`):
  release grab + re-enable physics + apply velocity; the flying clump's ground-hit fires the settle.
- Gates: Host-only + `senderPeerSlot∈[1,kMaxPeers)` (`event_dispatch_state.cpp:700-717`); `g_heldBy`
  (eid→slot, one hold per peer, `trash_channel.cpp:101`); `OnGrabHolderLeft(slot)` disconnect cleanup (`:568`).
- REUSABLE for a rock: the whole request/authority skeleton. NOT needed (chipPile-specific): the clump
  morph (`PropConvert ToClump/ToPile`), `ClumpBirth` certificate, chipType, trash_proxy — a rock stays ONE
  `Aprop_C`, no morph.

### Native rock grab/drop (mainPlayer_C methods — all puppet-callable)
- **R = the `drop` ACTION = INVENTORY path (the user's workflow).** R-pickup: `Hold Object(useHold, Manual
  AActor, collected)` (`mainPlayer.hpp:464`) → `getData` (captures Key@0x40 + transform@0x10,
  `struct_save.hpp:7-8`) → `addEquip` (into `equipment` TArray<FName>@0xB58) → `updateHold` (FinishSpawning
  the hand DISPLAY actor) → `ac.K2_DestroyActor()` = **the world rock is DESTROYED** (exactly ONE prop,
  finding-2026-07-08:296). R-place: `simulateDrop(dontWakeup, place, dontCollect)` (`mainPlayer.hpp:787`)
  → BeginDeferred + FinishSpawningActor → `loadData` restores Key verbatim = a **FRESH `Aprop_C`** ([H1]
  GREEN). `Manual` param lets a caller bypass the camera `lookAtActor` raycast.
- **E = the `use` ACTION = PHYSICS-HANDLE path (recovery lane).** `pickupObjectDirect(Actor, Component)`
  (`mainPlayer.hpp:666`) → sets `grabbing_actor`@0x7D0 + PHC `grabHandle`@0x688; rock stays LIVE (NOT
  destroyed). Release = `dropGrabObject()` (`mainPlayer.hpp:670`): `grabbing_actor=None` + `ReleaseComponent`.
- PUPPET feasibility: `pickupObjectDirect` on a puppet is PROVEN (grab engages+holds 40/40,
  `votv-puppet-grab-feasibility-2026-06-22`); no controller/HUD reads in the grab path. Caveat: the
  puppet's tick doesn't drive the PHC → host must kinematically drive the pose (`puppet_carry_drive`).
- NOT-FOUND (needs bytecode/IDA before relying): (1) does generic `prop_C::playerGrabbed(player)` forward
  to `pickupObjectDirect` like the pile does; (2) `simulateDrop` placement-transform source (aim vs saved);
  (3) whether `Hold Object`/`updateHold`/`addEquip` read local-only HUD/inventory-UI state a puppet lacks.

## DESIGN (draft — to be pressured by a DESIGN /qf pass)
Model: the CLIENT authors NO keyed-prop lifecycle broadcasts; it sends INTENTS; the HOST is the sole
authority (mirrors the host's own r-hold, which works).

- Wire (new): `PropGrabIntent{ eid; u8 mode(R-inv/E-phys) }` (client→host); `PropDropIntent{ key[32] or
  eid; float T[7] transform; float vel[3]; u8 mode }` (client→host). (Or reuse GrabIntent/ThrowIntent with
  a keyed-prop discriminator.)
- Client: SUPPRESS keyed-prop seam broadcasts (grab-destroy + drop-spawn) — not authoritative. On the grab
  edge send `PropGrabIntent{E,mode}`; on the drop edge send `PropDropIntent{K,T,mode}`.
- Host OnPropGrabIntent: resolve eid E → rock actor + puppet-N. R-mode: destroy the host rock (or `Hold
  Object(Manual=rock)` on puppet-N) → broadcast DESTROY(E); held visual = the HandItem hand mirror. E-mode:
  `pickupObjectDirect(rock,comp)` on puppet-N → rock stays live → `puppet_carry_drive` streams the pose.
  Set `g_heldBy[E]=slot`.
- Host OnPropDropIntent: R-mode: author a fresh keyed spawn at T (Key K) → broadcast spawn. E-mode:
  `dropGrabObject()` on puppet-N + set transform T. Clear `g_heldBy`.
- Gates: Host-only + senderPeerSlot + `g_heldBy` + disconnect cleanup (mirror trash_channel).
- Deletes: v2/v3/v4 (gone) + the client keyed-prop seam broadcasts (replaced by intents).

## OPEN DESIGN QUESTIONS (for the DESIGN /qf)
- R-mode: does the host even need `Hold Object` on the puppet, or just destroy(E)+spawn(K@T) with the
  HandItem mirror as the held visual? (Simpler; the puppet needn't natively hold it.)
- Identity: eid (wire handle) vs Key (durable). The grab uses eid E (must be cross-peer-agreed via the
  Element Registry); the drop uses Key K (durable). Confirm a grabbable rock always has a bound eid.
- The grab-destroy currently ALREADY crosses (bidirectional destroy-seam, settled session) → is the ONLY
  missing piece the DROP intent (spawn), or must the client stop the seam-destroy too (to avoid the husk
  race)? De-braid: the WORLD-rock grab-destroy (legit) vs the HAND-DISPLAY husk-destroy (spurious).
- Reuse GrabIntent/ThrowIntent (add a keyed-prop path) vs new PropGrab/PropDropIntent kinds.

## SCOPE DECISION (2026-07-09, measured from the clean baseline)
In the clean baseline (v2/v3 reverted), the R-GRAB HALF ALREADY WORKS: the client R-pickup's world-rock
`K2_DestroyActor` crosses the bidirectional destroy-seam (settled session) → the host destroys eid E → the
rock DISAPPEARS on the host (correct). The ONLY broken half is the R-PLACE SPAWN (host-auth-skipped at
`prop_lifecycle:210`) → invisible. The v3 DUPE was purely the suppression of the grab-destroy; WITHOUT
suppression, grab removes the rock and a drop-intent re-adds it → NO dupe.

=> INCREMENT 1 (build first, smallest, targets the user's need): a clean host-authoritative DROP INTENT.
   Client detects the R-place (the `simulateDrop`→FinishSpawningActor seam, +1 tick for the restored Key) →
   sends `PropDropIntent{ key, transform }` → HOST spawns/adopts the keyed prop at T (by Key) → broadcasts
   via the existing prop-spawn lane. NO destroy-suppression (the grab-destroy already removes the host
   rock), NO EnsureHeldItemBroadcast churn (a clean reliable one-shot intent). Verify a CLEAN single
   grab→place: rock vanishes on grab, reappears at the placed spot on the host, no dupe.
   - The husk-destroy on place (the hand-display actor's K2) is a NO-OP on the host (the host already has
     no rock post-grab), so it cannot kill the drop-spawn on a clean cycle.
INCREMENT 2 (harden, after Inc-1 verified): rapid-cycle ordering (the grab-destroy vs drop-spawn interleave)
   + the E-physics puppet-grab recovery lane (`pickupObjectDirect(puppet)` + `puppet_carry_drive` +
   `dropGrabObject(puppet)`). Only if Inc-1's clean case passes and the rapid case still misbehaves.
