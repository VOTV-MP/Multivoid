# Phase 1 (X) — native-authoritative chipPile: FIX DESIGN (on review, ZERO code)

Status: **DESIGN on review.** Fork (X) confirmed (interaction RE: proxy loses the native interaction window;
only a kept native restores it). This design covers the full change + the tests, and **separates the two
levels explicitly** (the user's critical requirement). Build only after review.

## 0. The two levels — separated explicitly (NOT "all native")

(X) is **native interaction SURFACE + host-authoritative GRAB mutation**. These are different axes; conflating
them is the trap:

- **Interaction SURFACE = native-local.** A *kept* native chipPile is a genuine `actorChipPile_C`, so the
  game's own interaction trace makes it `lookAtActor` (the `int_player_C` filter passes), its mesh blocks
  movement (collision), and aim is occlusion-correct. These return **for free** by not destroying the native.
  This **replaces the camera-cone workaround** (`EidForAimedPileProxy`) with the real game trace.
- **Grab MUTATION = host-authoritative.** The native's *local* `playerGrabbed` self-destructs the pile + grabs
  the clump LOCALLY = a host DUP (the codebase forbids a client authoring a shared trash entity,
  `trash_collect_sync.cpp:192-217`). So the local native grab must be **suppressed and routed via
  `GrabIntent`** — the host runs `playerGrabbed` on the requester's puppet (`trash_channel.cpp:346-361`). The
  host stays the sole author of the grab mutation.

So: aim/collision/occlusion/`lookAtActor` go native-local; the grab VERB stays host-arbitrated. (X) does NOT
make the grab mutation local.

## 1. Quiescence-pivot — RE-EXAMINED: the evidence says it is **NOT needed** (recommend KEEP the spawn-time bind)

The purge-RE hypothesized the spawn-time bind "races by construction" (binds transient pre-rebuild piles the
loadObjects rebuild then purges), motivating a pivot to a deferred location-join at quiescence. **The Path-A
re-smoke evidence refutes the race as the killer:**

- The bind bound **874/874** with the family tripwire **SILENT** — it caught the correct final piles, not
  transient ones.
- The eids `TryDestroyTwin` destroyed (`[PILE] DESTROY native level-pile twin eid=2275 / 2276 / 2277 …`) are
  **the exact eids the bind bound** (host map `[873] eid=2275`, …). So the bound natives **survived the
  loadObjects rebuild** (21:08:46 → 21:09:02) and died **only** from `TryDestroyTwin` at 21:09:02 — the proxy
  replacing the native — **not** from the rebuild purge (which reaped *other* props, 21:08:47-21:09:02).

Therefore the spawn-time bind already binds the **surviving** natives; the killer is the proxy/`TryDestroyTwin`
(item 4), not a pre-quiescence race. **Recommendation: KEEP the verified spawn-time bind (Path A, 874/874) and
fix the proxy.** The quiescence-pivot + location-join is a *safety refinement* to adopt **only if** a re-smoke
WITH proxy-suppression shows some bound natives die in the rebuild before the proxy arrives. Building the
deferred location-join now would retire the verified Path A for an unverified mechanism — against "don't
rebuild what works." (If review still wants the pivot upfront, it replaces the spawn-order thunk with a
deferred walk of the surviving natives + a `{eid→location}` exact join — host-local 0.1cm, not cross-peer
fuzzy — and retires Path A's objectsData parsing. Quiescence signal to reuse: `HasSeededOnce()` &&
`!InPurgeEpisode()` && `CountLoadTailUnsettled_` stable, surfaced as `HasLoadTailQuiesced()`.)

## 2. Native interaction SURFACE restore

- **Keep the native** (item 4 stops `TryDestroyTwin` from destroying it). The kept native is a normal
  `actorChipPile_C` (collision-disable is `garbageClump`-only, not the pile —
  `votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md:225-237`), so trace `lookAtActor`, collision, and
  occlusion-correct aim are native, no workaround.
- **Retire the camera-cone for bound piles (RULE 2).** `OnPileGrabPre`'s client branch currently uses
  `EidForAimedPileProxy` (camera-ray cone) because the proxy can't be `lookAtActor`. A bound native IS the
  `lookAtActor`, so for bound piles the cone path is replaced by reading `lookAtActor` directly. (The cone
  stays only if any unbound proxy piles remain — see item 6; intent is none remain for save-loaded piles.)

## 3. Native-grab-suppress → GrabIntent (host-auth grab) — MECHANISM RESOLVED (probe done, bytecode-proven)

The suppress mechanism was the one unproven piece; the grab-dispatch RE (2026-06-25) resolved it **Option 1:
clear `lookAtActor` on the PRE edge** — clean, byte-exact, no runtime probe needed.

- **On `InpActEvt_use` PRE (`OnPileGrabPre`), in the EXISTING positive grab branch** (the E-press is resolved
  as a grab aimed at a tracked mirrored pile): if the aimed pile is a **bound** native
  (`IsBoundMirrorNative` / its eid resolves), **null `mainPlayer.lookAtActor` (raw write @0x0AA0)** then
  `SendGrabIntent(eid)`. The InpActEvt_use ubergraph reads `lookAtActor` FRESH at the `playerGrabbed` cast
  (`EX_ObjToInterfaceCast` Target = `EX_InstanceVariable lookAtActor`, mainPlayer.json:162172-162188) — which
  runs AFTER our PRE observer (the handler is `[0] write Key; [1] CALL ubergraph(...cast...); [2] Return`; PRE
  fires before `[0]`). So the cast reads NoObject → the cast fails → **`playerGrabbed` is skipped** → no clump,
  no local destroy, no client-authored dup. The host then runs `playerGrabbed` on the puppet (existing
  host-auth path), eid from `GetPropElementIdForActor`/`ResolveMirrorEidByActor` on the native.
- **Surface stays intact:** `lookAtActor` is a plain `ObjectProperty` (no RepNotify, no setter side-effects),
  written by `LookAtFunction` (the interaction trace) on a SEPARATE per-tick dispatch — so the clear
  **self-heals next frame** (the prompt/aim returns) and **collision is never touched**. Only this one
  E-press's grab-dispatch is cancelled. Add `ue_wrap::...ClearMainPlayerLookAtActor()` symmetric to the
  existing `ReadMainPlayerLookAtActor` (RULE 7 wrap boundary).
- **Why not the alternatives** (rejected, evidenced): a UFunction-patch on `playerGrabbed` can't cancel
  (`ufunction_hook` is POST-only, forward-then-observe — `ufunction_hook.cpp:59-80`); a "temp non-grabbable
  flag" is impossible (`playerGrabbed` has ZERO conditional branches entry→clump-spawn, no gate field exists).
- **One CONFIRMATORY smoke after build** (standard before/after observable-state check, NOT new RE): after the
  PRE clear, the prompt/recognition re-appears the next frame (the trace tick re-writes lookAtActor) with no
  flicker, and no other consumer reads lookAtActor between the clear and the re-write that frame.

## 3b. Proxy-necessity verdict (RE 2026-06-25) — X' partially holds: per-eid SUPPRESS, not wholesale delete

The "is the proxy still needed?" RE settled the (X)-suppress vs (X')-delete question:

- **For SAVE-LOADED piles the proxy carries NOTHING beyond identity+interaction** that the bound native +
  existing channels don't already cover: the held-pose drive resolves by eid (`ResolveLiveActorByEid` →
  generic-root physics, class-agnostic, `remote_prop.cpp:298-320`) so the host's `PropPose` drives the bound
  native identically; GrabIntent arbitration is eid-based (`trash_channel.cpp:311-338`), not proxy-based;
  PropDestroy/PropConvert run the standard mirror teardown + spawn-rebind on a bound native.
- **MULTI-CLIENT visibility is DECISIVE**: every peer independently requests + loads the FULL host save
  (`save_transfer.cpp` per-slot stream → native `loadObjects`), so **each peer already has its OWN native** for
  every save pile. There is no "host broadcasts proxies to peers lacking the native" mechanism — cross-peer
  visibility for save-loaded piles is each-peer-loads-the-same-save. **Deleting the save-loaded proxy loses
  visibility on NO peer.** (The proxy exists today only because the client DESTROYS its native via
  `TryDestroyTwin` and substitutes the host's mirror.)
- **BUT the proxy is IRREDUCIBLE for RUNTIME / host-only piles + carried clumps**: a pile the host creates
  after the client joined (re-pile `OnHostConvert`→`BroadcastConvert`; a mid-session dump) has NO native on
  the client → `OnConvert` spawns a proxy fresh; a client-grabbed clump in flight is driven via
  `trash_clump_pose_stream`→`ProxyActorForEid` (proxy-map only). These have no native to bind.

**Verdict: the user's (X') instinct is right that for save-loaded piles the proxy is REDUNDANT — but the
mechanism is a per-eid SUPPRESS (don't materialize a proxy when a bound native exists), NOT a wholesale delete
of `trash_proxy` (runtime/host-only piles + clumps still need it).** That is exactly the (X) item-4 guards
below. Two non-blocking GAPS the RE surfaced, folded into the design:
- **(gap a) aim recognition** — the client-grab aim currently scans the proxy map (`EidForAimedPileProxy`). A
  bound native IS a real `lookAtActor`, and the host already reads native piles via `ReadMainPlayerLookAtActor`
  — so the client switches to `lookAtActor` recognition (this is item 2/3's "native trace replaces the cone").
  **RESOLVES the aim half** of the camera-cone retirement; the grab-SUPPRESS half (item 3) is still the one
  open probe.
- **(gap b) morph re-spawn vs re-skin** — a bound native that morphs (pile→clump on a host grab) takes
  `OnConvert`'s legacy spawn-new-rebind-destroy-old branch (the clump is a runtime proxy), re-spawning per
  morph rather than re-skinning one actor. Functional (the existing pre-proxy morph model); note it — the
  native-authoritative guarantee is for the INITIAL save-loaded state, and a grab/morph hands the eid to the
  runtime-proxy clump path (which is correct: a carried clump is host-runtime, not a save-loaded native).

## 4. Proxy-suppression — the CORE fix (2 per-eid guards + RetireProxy)

Keyed entirely on the existing `prop_element_tracker::IsBoundMirrorNative(actor)` predicate:

- **(a) Skip the proxy spawn.** In `remote_prop_spawn::OnSpawn`, before the trash-proxy branch's
  `trash_proxy::SpawnProxy` (`:323-327`): if the incoming eid E resolves in the Registry to a **live
  bound-mirror native**, the native IS the mirror — `return` without spawning a proxy.
- **(b) Skip the twin-destroy.** The `pile_reconcile::TryDestroyTwin` call (`remote_prop_spawn.cpp:375`) must
  not run for a bound eid (or `TryDestroyTwin` exempts `IsBoundMirrorNative(native)` before `UnmarkAndDestroy`)
  — never destroy the bound native.
- **(c) RetireProxy(E) for the proxy-before-bind race (case ii).** If a proxy spawned before the bind ran, the
  bind (`BindLocalNativeToHostEid_` case-ii path) calls `trash_proxy::RetireProxy(E)` in addition to its
  existing `rebindInPlace`, so no stale proxy survives alongside the bound native.

Cleanly separable: only the bound-eid path changes; `g_proxies` dedup, ReskinProxy, RetireProxy, NearestPile
all stay intact for genuinely-unbound (derived/gameplay/host-only) piles.

## 5. Blast radius — NIL (confirmed by sub-question RE)

- **kerfur + instant-world** don't touch the pile-proxy mechanism (orthogonal channels).
- **L1 join-window pile sync** IS the non-bound proxy flow — left intact for unbound piles; the bound path
  *supersedes* it for save-loaded piles (the stated Phase-4 intent), not regresses it.
- The **divergence sweep** already exempts bound mirrors (`remote_prop_spawn.cpp:1081 if (pr.mirror) continue`)
  + the floor is unchanged.
- All guards gate on `IsBoundMirrorNative` → unbound piles see byte-identical behavior.

## 6. Unify with the off-kerfur (one identity mechanism for both keyless families)

- Off-kerfurs are **already native-authoritative** — they bind on the separate kerfur path (no proxy,
  `kerfur_entity[client]: kerfur prop mirror bound at host-range eid=…`), survive, and keep full native
  interaction. The Path-A smoke bound 4/4 case(i).
- After (X), chipPiles are ALSO native-authoritative (proxy suppressed). `save_identity_bind` becomes the
  single bind for both keyless families; the trash proxy is retired for save-loaded piles. The chipPile grab
  routes via `GrabIntent` (item 3); the off-kerfur grab already routes host-auth on its own channel.

## 7. Tests (the re-smoke + the hands-on split — identity is engine-truth, interaction is rendering-blind)

Autonomous re-smoke (engine-truth, gated [dev]):
- **Persist through the purge:** bound chipPiles SURVIVE (no proxy replacement) → `totalLiveNatives ≈ 870`
  (the killer was `TryDestroyTwin`; suppressing it keeps the natives). off-kerfurs survive + bound.
- **No proxy for bound piles:** the `[PILE] DESTROY native twin` lines are GONE for bound eids; no
  `trash_proxy` spawned for them.
- **Grab host-auth, no dup:** a scripted client grab of a bound pile → `GrabIntent` → host `playerGrabbed` →
  no second pile.
- **Regressions:** 3 instances (L1 unbound flow / kerfur / instant-world) + floor A/B intact; sweep doom set
  unchanged for unbound.

Hands-on (rendering/interaction-blind autonomously — the user's hands-on):
- **Native interaction:** aim at a pile → it IS `lookAtActor` (a HUD/interact prompt appears); collision
  blocks walking through it; aiming through a wall does NOT grab it (occlusion-correct, the cone's through-wall
  bug gone); native grab → host (no dup).
- **jUuC off-kerfur:** appears + poses (the case the proxy/key/scope-A can't reach).

## Retirements (RULE 2, on (X) landing)

- The camera-cone grab (`EidForAimedPileProxy`) for bound piles → replaced by native `lookAtActor`.
- The trash proxy spawn + `TryDestroyTwin` for bound (save-loaded) piles → suppressed; the native is the mirror.
- Path A's objectsData/primitivesData parsing → **KEPT** (the spawn-time bind works); retired only if the
  quiescence-pivot (item 1) is adopted.

## Open items needing a probe before/within the build

1. **The native-grab-suppress mechanism** (item 3) — the one unproven piece; probe before coding.
2. **Whether the quiescence-pivot is needed** (item 1) — determined by the proxy-suppression-only re-smoke
   (the evidence says no; verify).

## Source map

`trash_proxy.{h,cpp}` (proxy = bare AStaticMeshActor, NoCollision, camera-cone) · `trash_collect_sync.cpp`
(`OnPileGrabPre`, `EidForAimedPileProxy`, the client-grab branch) · `trash_channel.cpp:341-361` (host-auth
`playerGrabbed` on the puppet) · `remote_prop_spawn.cpp:323-382` (trash branch / SpawnProxy / TryDestroyTwin
call), `:1081` (sweep mirror-exempt) · `pile_reconcile.cpp:121-200` (`TryDestroyTwin`) ·
`prop_element_tracker.cpp` (`IsBoundMirrorNative`, `MarkBoundMirrorNative`) · `save_identity_bind.{h,cpp}` (the
bind; case-ii `RetireProxy`) · `remote_prop_spawn.cpp:1393-1432` (quiescence gate, if item 1 adopted) ·
research/findings/votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md (kept native = fully grabbable) ·
votv-increment2-clientgrab-FULL-CHAIN-AS-BUILT-2026-06-23.md (camera-cone, the proxy workaround).
