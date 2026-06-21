# 07 — MORPH V2: the bind-model morph on the grabbing_actor held-object channel

> The corrected grab->carry->throw->land design. Supersedes **05** (the morph DESIGN that
> bet on the clump Init-POST observer + `holdPlayer`) and **06** (its AS-BUILT, which FAILED
> the hands-on as a regression). Synthesized 2026-06-20 (session 33) after a full re-read of
> the CURRENT take-17 code + the bytecode RE.
>
> **⚠ STATUS: SUPERSEDED + RETIRED 2026-06-21 — see [08-HOST-AUTH-TRASH-CHANNEL.md](08-HOST-AUTH-TRASH-CHANNEL.md).**
> The "VERIFIED 2026-06-20" claim that was here was a **FALSE POSITIVE**: the autonomous smoke fired
> `playerGrabbed` on ONE sanitized pile and never exercised a pile CLUSTER or the client→host direction.
> The real hands-on (2026-06-21) REFUTED it — the proximity land-watch (`FindNearestChipPile(lastPos,100cm)`)
> consumes a NEIGHBOR pile in a cluster → eid mis-binds → divergence; and the client grab never armed
> (hands-full gate + the direction was never wired). The whole "owner-side re-skin on both peers + proximity
> land" model is retired (RULE 2) in favour of the **host-authoritative state machine** (08). History only —
> everything below is the SUPERSEDED design; do NOT implement it.

## Why 05/06 failed, and the one rule that fixes it

The morph has failed ~11x, every time on a **guessed runtime mechanism**:
- 06 keyed the clump-adopt on `clump.holdPlayer == grabbingPlayer` at the clump's **Init-POST**.
  `holdPlayer` is NULL there (the BP `toClump()` sets it AFTER `FinishSpawningActor`), AND the
  clump's Init-POST observer may not even fire for an `EX_CallMath`-spawned clump. The
  reconstruction was explicitly *"field shapes, not literal bytecode"* (the RE doc says so) — a guess.
- 06 also **replaced the reliable grab `PropDestroy(E)`** with the morph-arm, so a missed adopt =
  the host grab did NOTHING = a regression worse than the working bind.

**The rule: anchor every morph edge on a channel we have PROVEN fires, and never let a missed
morph regress the working grab.** Bytecode-confirmed un-hookable: `toClump`/`playerGrabbed` are
`EX_LocalVirtualFunction` (the `OnPileGrabPre` comment in `trash_collect_sync.cpp:368` states this
directly) — INVISIBLE to our ProcessEvent detour. So the clump's *birth* is unobservable by a
UFunction hook OR (uncertainly) by the Init-POST observer. We do NOT depend on either.

### The proven channel — VERIFIED 2026-06-20 (autonomous chippile grab smoke)

`local_streams.cpp::Tick` reads `mainPlayer`'s held-actor state every frame and fires a new-held edge
for the grabbed clump.

**EMPIRICAL CORRECTION (smoke 2026-06-20, `harness/autotest_chippile.cpp`): the grabbed clump rides
`grabbing_actor` — the PHC light-grab path, written by `playerGrabbed -> pickupObjectDirect` — NOT
`holding_actor`** as 05/06 and the first draft of this doc assumed. A field-routing probe logged
`grabbing_actor=prop_garbageClump_C[clump=1] holding_actor=null` on 16/16 polls across two smokes.
`local_streams` reads `grabbing_actor` FIRST (with `holding_actor` only as a fallback), so the new-held
edge sees the clump regardless and `TryAdoptHeldClump` adopts it. The earlier "`[probe garbage_pickup]`
confirmed holding_actor" claim was FALSE — that probe never emitted a real line, and the 2026-06-08 RE
already said pickup feeds the clump's root component to the grab machinery (= `grabbing_actor`). The
`toClump`/`holding_actor` story was never the grab path. (`holding_actor` may still serve some other
carry; the fallback is left in place — not retired on a hunch.)

The seams the morph rides, ALL now runtime-verified by the smoke (host + client `votv-coop.log`):

- **new-held edge** fires for the clump — VERIFIED (`net: NEW held ... prop_garbageClump_C ... -> MORPH-ADOPT(ToClump)`).
- **release edge** fires on throw — VERIFIED (`net: held -> released`).
- `RegisterPropMirror` binds/rebinds an actor to an eid IN PLACE — VERIFIED (`eid=E REBOUND mirror in place ... (morph re-skin)`).
- the held-pose stream kinematic-drives a mirror by eid — VERIFIED (`net: PropPose emit #N ... eid=E` throughout the carry).

We build the whole morph on these seams. Nothing rides the clump Init-POST or `holdPlayer`.

## The model: re-skin eid E in place (NO second entity, NO death-watch)

A pile is bound to the host-minted eid `E` (`RegisterPropMirror(E, ownPile)` on the client;
a local tracker Element on the host). The morph **re-skins E** across the three UObjects
(pile-A -> clump -> pile-B); `oldEid == newEid == E` on every edge. One actor per peer per eid,
before and after — no doom, no catalog, no fresh-eid second entity.

This **retires the v52 destroy-and-recreate morph entirely** (RULE 2):
- `OnPileGrabPre` no longer sends a bare `PropDestroy(E)` — it arms the morph (+ a deferred destroy).
- The clump no longer expresses as a **fresh-eid** keyless `PropSpawn` + `WatchClump` death-watch.
- `OnConvert` no longer mints a **fresh** `newEid` — it re-skins E.
- DELETED: `g_watchedClumps`, `WatchClump`, `BroadcastConvertNear`, `TickWatchReleasedClumps`,
  the EnsureHeldItemBroadcast clump branch, the v52 fresh-eid `OnConvert`. (All migration baggage.)

## The four edges

### 1. GRAB (pile-A -> clump), owner-side — `OnPileGrabPre` + new-held edge

```
OnPileGrabPre(mainPlayer):                       # PROVEN seam (existing, ProcessEvent-visible)
  aimed = lookAtActor; require IsChipPile(aimed)
  resolve E + isLocal:  E = GetPropElementIdForActor(aimed)  -> isLocal=true   (host's own pile)
                        else E = ResolveMirrorEidByActor(aimed) -> isLocal=false (client's adopted pile)
  require E valid
  pile_morph::OnGrab(E, GetActorLocation(aimed)):                # AS-BUILT: NO isLocal param (see notes)
      g_pendingMorph = { E, pilePos, armedAt: now }              # single-slot
      deferred PropDestroy(E) fires at now+400 ms if unclaimed   # the FALLBACK (take-17 behaviour)

local_streams new-held edge (heldActor becomes a garbageClump):  # PROVEN seam (existing)
  pile_morph::TryAdoptHeldClump(heldActor):
     if g_pendingMorph live AND heldActor near g_pendingMorph.pilePos (<=600 cm):
        rebind E -> heldActor:  isLocal ? RebindLocalElementActor(E, clump)
                                        : RegisterPropMirror(E, clump, rebindInPlace=true)
        broadcast PropConvert{ kind=ToClump, oldEid=E, newEid=E, clumpClass, chipType, transform }
        CANCEL the deferred PropDestroy                          # the convert subsumes it
        arm land-watch { E, clump }                              # owner land-watches its OWN clump
        clear g_pendingMorph
     # held-pose stream now resolves E (ResolveHeldPropEid mirror/forward fallback) -> streams the carry
```

If `TryAdoptHeldClump` never fires within 250 ms (grab failed / clump unobserved), `pile_morph::Tick`
sends the deferred `PropDestroy(E)` -> the mirror pile vanishes (take-17). **A missed morph never
regresses the working grab.**

### 2. CARRY (held clump pose) — the existing held-pose stream

Once the clump is bound to E, `ResolveHeldPropEid` resolves E (forward map for the host's own;
mirror map for the client's), `pp.elementId = E`, and the stream kinematic-drives the mirror in the
grabber-puppet's hands on every peer. **No new stream code** — the gap was only the missing
`ResolveMirrorEidByActor` fallback for the bound clump.

### 3. THROW/LAND (clump -> pile-B), owner-side — release edge + bound-clump land-watch

```
local_streams release edge (eid-bound clump leaves the hand):    # PROVEN seam (existing)
  pile_morph::OnRelease(clump, E)  -> the land-watch keeps tracking E's clump (now flying)

pile_morph::Tick (per-tick, while any land-watch is armed):     # AS-BUILT: g_land is a VECTOR (N in flight)
  if E's bound clump still live: update lastPos; continue        # cheap: IsLiveByIndex on ONE actor
  else (the clump died -> turnToPile ran):                       # owner-side, SOUND (pile co-located)
     pile = FindNearestChipPile(lastPos, 100 cm)                 # AS-BUILT: 100cm + an UNBOUND check
     if pile && UNBOUND (not in forward map / mirror registry):
        MarkKnownKeyedProp(pile); MarkProcessedInit(pile)        # AS-BUILT: SUPPRESS the host re-seed dupe
        rebind E -> pile (RegisterPropMirror rebindInPlace -> IsMirror-routed); PropConvert{ToPile, E}
     else if pile already bound to E2 (the host re-seed WON the race):
        broadcast PropDestroy(E)                                 # drop the clump; E2 is the pile (no dupe)
     else (no pile near -> LifeSpan despawn / eaten):
        broadcast PropDestroy(E)                                 # mirrors drop the flying clump, clean
     erase this land-watch
```

The land search is the **owner's own just-spawned pile** at the clump's last position — the case the
DECISIVE RE proved sound (the unsound case it retired was `FindNearestChipPile` on a RECEIVER, where
piles are not co-located). Gated by liveness-death + radius + chipType + a 6 s TTL — cannot over-fire.

### 4. RECEIVER — `OnConvert{kind, E}` re-skins in place

```
OnConvert(kind, E, transform, class, chipType):
  cur  = ResolveLiveActorByEid(E)                                # our bound rendering of E
  next = (kind==ToClump) ? SpawnLocalClump(class, chipType, transform)   # kinematic, physics off
                         : SpawnLocalPile (class, chipType, transform)    # settled, QueryAndPhysics
  rebind E -> next:  E-is-local-element ? RebindLocalElementActor(E,next) : RegisterPropMirror(E,next,rebind)
  if cur && cur!=next: ClearAnyDriveFor(cur); MarkIncomingDestroy(cur); K2_DestroyActor(cur)
  # order: spawn-new -> rebind -> destroy-old, so E never resolves to a dead actor (no flicker)
```

The host applies a CLIENT's convert against its OWN local element E (the authoritative pile) -> the
host shows the client's grab without spawning a duplicate (E is host-minted, so every peer including
the host converges on the same identity). Host-authority via E, not via a request/relay round-trip.

## Host conflict arbitration (two peers grab the same E)

**AS-BUILT: handled by `OnConvert` IDEMPOTENCY, not an explicit host state-map.** A convert whose target
rendering already matches the current one (`IsGarbageClump(cur)==wantClump`) is a no-op — this absorbs
echoes + the grab-race loser's convert (dupe-safe). The residual is brief pose-jitter in the sub-RTT
double-grab window (cosmetic, not a dupe). The design's `g_morphStateByEid` map is NOT implemented; add it
only if a real test shows the jitter matters. (MTA `CClientGame` sync-context arbitration is the map shape.)

## Wire

Reuse `PropConvert=41`. `PropConvertPayload` (100 B, unchanged size): carve `uint8_t kind` from
`_pad[3]` (`propconvert_kind{ kToClump=0, kToPile=1 }`); `oldEid==newEid==E`. Bump `kProtocolVersion`
80 -> 81. The dispatch eid range-check accepts **either** range (a client relays a host-range E with a
client sender slot — the CRITICAL fix a host-grab-only smoke would mask).

## Files

| File | Change |
|---|---|
| `include/coop/net/protocol.h` | `kind` byte + bind-model doc; `kProtocolVersion` 81. |
| `coop/prop_element_tracker.{h,cpp}` | `RebindLocalElementActor(eid,newActor)` (host's own morph; keep `g_actorToPropElementId` consistent). |
| `coop/remote_prop.{h,cpp}` | `RegisterPropMirror(... , bool rebindInPlace=false)`; `OnConvert` -> bind-model re-skin; `SpawnLocalClump`. |
| `coop/pile_morph.{h,cpp}` (NEW) | grab adopt + deferred-destroy fallback + land-watch + conflict map + `Tick`/`OnDisconnect`. |
| `coop/trash_collect_sync.{cpp,h}` | `OnPileGrabPre` -> `pile_morph::OnGrab`; RETIRE the v52 death-watch; EnsureHeldItemBroadcast clump early-return. |
| `coop/local_streams.cpp` | new-held -> `TryAdoptHeldClump`; `ResolveHeldPropEid` mirror fallback; release -> `OnRelease`. |
| `coop/event_dispatch_entity.cpp` | PropConvert either-range eid check. |
| `coop/subsystems.cpp` | drop `TickWatchReleasedClumps`; add `pile_morph::Tick` + `OnDisconnect`. |
| `CMakeLists.txt` | + `pile_morph.cpp`. |

## Pitfalls (the 11-failure lessons baked in)

1. **No second entity** — `oldEid==newEid==E`; re-skin, never mint a fresh newEid (the v52 dupe).
2. **No dependence on the clump Init-POST / `holdPlayer`** — anchor on the held-object channel only.
3. **A missed morph must not regress the grab** — the deferred `PropDestroy(E)` fallback (cancel on adopt).
4. **Land search only on the OWNER** — `FindNearestNativeChipPile` at the clump's last live pos
   (sound: co-located). NEVER on a receiver (cross-peer piles aren't co-located — the s23 bug).
5. **Don't run BP `toClump`/`turnToPile` on a mirror** — the receiver spawns+destroys (kinematic clump,
   settled pile); the grabber runs the real BP (native mesh/variant correct by construction).
6. **Echo-suppress every receiver destroy** (`MarkIncomingDestroy`) so OnConvert doesn't ping-pong.
7. **Instrument every edge** — this build IS the probe: the first hands-on log shows exactly which
   edges fired (grab-arm / clump-adopt / convert-sent / land-detected / deferred-fallback-fired).

## AS-BUILT (2026-06-20, session 33) — BUILT + 2 reviews applied, NOT deployed, NOT verified

DLL `2f0970276799478c` (proto v81), build clean. **Two adversarial review passes + a 4-agent
code-trace** shaped the final code. Net architecture = exactly the held-object grab + death-watch land
poll above. Divergences from the sketch (the code is truth):

1. **NO `isLocal` plumbing.** The local-vs-mirror rebind is routed on the Element's AUTHORITATIVE
   `IsMirror()` flag INSIDE `RegisterPropMirror(...,rebindInPlace=true)` (a MIRROR → `SetActor`; a LOCAL
   element → delegate to `RebindLocalElementActor`). So `OnGrab`/`pile_morph` never guess local-vs-mirror.
   (Review fixed: deriving it from a live `cur` mis-routed a host's own element when `cur` was momentarily
   dead.)
2. **Deferred-destroy window 400 ms** (not 250); **clump-near-pile 600 cm**; **land search 100 cm + an
   UNBOUND filter** (skip an already-bound neighbour). **`g_land` is a VECTOR** (≤16) so several thrown
   clumps in flight each get their own re-pile (single-slot would clobber).
3. **Hands-full gate** in `OnPileGrabPre` — skip arming the morph if the player is already holding
   something (else pressing E at a pile while carrying would spuriously vanish that pile's mirror).
4. **Held-eid CACHE** (`g_lastHeldEid`) — `ResolveHeldPropEid`'s O(n) `ResolveMirrorEidByActor` fallback
   runs ONCE on the new-held edge, not per-tick (the per-frame-O(n) hot-path the review caught).
5. **Conflict** → `OnConvert` idempotency (no host state-map).
6. **OnConvert reuses OnSpawn** (RULE 2) via `fromConvert` (skip eid-dedup) + `skipBind`/`outSpawned`.

### The CONFIRMED dispatch fact (the take-18 trap, now in the canonical map)
The morph product's **Init-POST observer does NOT fire** for a BP-deferred clump/pile
(`EX_LocalVirtualFunction`; `host_spawn_watcher.cpp:132`). Take-18 bet on it — failure #12 avoided by
review #2. **This is now in [docs/COOP_DISPATCH_VISIBILITY.md](../COOP_DISPATCH_VISIBILITY.md) +
[docs/COOP_ENTITY_EXPRESSION_MAP.md](../COOP_ENTITY_EXPRESSION_MAP.md)** — the cross-cutting truth that
makes much of 01-06 historical. The trace confirmed: the **clump has exactly ONE expresser** (pile_morph,
no dupe); the **landed pile has TWO** (pile_morph + the host re-seed), made exclusive by the
`MarkKnownKeyedProp`+`MarkProcessedInit` suppression + the race-loser `PropDestroy(E)`.

### The runtime link — SETTLED 2026-06-20 (was "the ONE unverified link")
Does the morphed clump reach a held-actor field the new-held edge sees → `TryAdoptHeldClump`? **YES —
verified.** The clump lands in `mainPlayer.grabbing_actor` (NOT `holding_actor`; see the corrected
"proven channel" above), and `local_streams` reads `grabbing_actor` first, so the new-held edge fires and
`TryAdoptHeldClump` adopts. Settled by `harness/autotest_chippile.cpp` (the faithful trigger: arm via
`InpActEvt_use` + run the pile's real `playerGrabbed(mainPlayer, HitResult)` directly via reflection, then
a field-routing probe). The deferred-destroy fallback remains as the no-regression backstop.

## Verification — VERIFIED 2026-06-20 (autonomous, cross-peer; plumbing end-to-end)

The morph's full grab→carry→throw→land cycle is runtime-verified by `autotest_chippile.cpp` on a 2-peer LAN
smoke (host + client `votv-coop.log`). This counts as a *matching real log* because the harness ran the
pile's OWN `playerGrabbed` — the genuine conversion (spawn clump → `pickupObjectDirect` → destroy pile) —
not a fake. Observed, single eid re-skinned pile-A → clump → pile-B:

| Edge | Host log | Client log |
|---|---|---|
| GRAB (pile→clump) | `grab armed eid=E` → `ADOPTED held clump → PropConvert{ToClump}` → `NEW held ... MORPH-ADOPT(ToClump)` | `eid=E REBOUND ... prop_garbageClump_C` → `OnConvert: eid=E re-skin -> clump` |
| CARRY | `PropPose emit #N ... eid=E` (poses track the player) | (drives the clump mirror by eid) |
| THROW | `held -> released` | — |
| LAND (clump→pile) | `land detected for eid=E -> pile at 14cm -> PropConvert{ToPile} (re-seed suppressed)` | `eid=E REBOUND ... actorChipPile_C` → `OnConvert: eid=E re-skin -> pile` |

The re-seed dupe guard fired (`re-seed suppressed`). RAM flat, no crash. **Remaining for a human glance**
(rendering only, not plumbing): the clump is *visibly* in the host-puppet's hand on the client screen, and
the re-piled pile is the correct variant at the right spot. The grab/throw inputs in the harness are a
direct `playerGrabbed` call + a PHC release+lift (not a live key/mouse), so a real hands-on grab remains the
gold-standard confirmation of the on-screen feel — but the sync plumbing is verified.
