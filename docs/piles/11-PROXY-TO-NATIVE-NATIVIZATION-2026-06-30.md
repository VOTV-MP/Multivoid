# 11 — Pile mirror: proxy → NATIVE nativization (2026-06-30 → 2026-07-01)

**Status: AS-BUILT + VERIFIED (hands-on). The pile form is now a rooted real `actorChipPile_C` native.**
Committed `abfaaed8` (inc 1) · `dabf84de` (inc 2 + chipType) · `3b72aba0` (rotation) · `fa8bc344` (join-dup
fix). Deployed `1C242F82` (= HEAD `fa8bc344`), 6 commits ahead of `origin/main`. Living doc; the one open
axis is SOUND-events (see NEXT). [[project-pile-nativization-2026-06-30]]

## The decision (root-cause, user-driven)
The client's mirror of a host-authoritative trash PILE was a bare `AStaticMeshActor` PROXY
(`coop/props/trash_proxy`) — it can NEVER be the game's `lookAtActor` (the `mainPlayer::LookAtFunction`
sphere-trace HARD-filters on `DoesImplementInterface(hit, int_player_C)`) and carries no collision, so the
client got **no native hover GUI, no movement-block, no occlusion-correct aim, wrong rotation**. Root fix
(RULE 1): remove the legacy proxy for the pile form; make the mirror a real rooted `actorChipPile_C`
native, which IS `int_player_C` by construction → GUI + collision + rotation + occlusion all return for
free. The **in-hand / flying CLUMP stays a bare proxy** (`prop_garbageClump_C` has a LifeSpan @0x0258 +
autonomous re-pile-on-contact → too live to keep native; it's the kinematic carrier).

### Why the proxy existed (and why that's obsolete)
Built 2026-06-21 because a client-SPAWNED real `actorChipPile_C` mirror "went not-live within ~10 s on its
own" → orphan → dup. The 2026-06-30 inertness probe **disambiguated that death: it was GC (the mirror was
UNROOTED)**, NOT BP autonomy. `AddToRoot` (which the proxy already used) stops it.

## PROVEN — the inertness probe (hands-on, `coop/dev/native_pile_inert_probe`, ini-gated, disarmed)
A rooted runtime `actorChipPile_C` + the inert recipe stayed live + inert for **60 s with COLLISION ON**
(`[INERT-PROBE] VERDICT GO`), and the user confirmed the **native hover GUI on aim** ("gui was there").
**[V hands-on]** → a rooted runtime native is a safe, fully-native mirror; the live ubergraph does NOT
self-destruct/self-morph when left alone. [[lesson-runtime-staticmeshactor-must-be-movable]]

## The inert recipe (`native_pile_mirror::Materialize`)
`AddToRoot` + `SetActorTickEnabled(false)` + `SetActorSimulatePhysics(false)` + `SetActorRootMovable` +
`SkinPileNative` (chipType + scale + host rotation, below). A materialized native is BOUND + MARKED
save-native (`Element::SetSaveNative(true)`) so it rides the SAME proven machinery as a save-loaded bound
native (`IsBoundMirrorNative`): pose drive (`ResolveLiveActorByEid`) [V]; b3 pos-correction [V]; the grab
route (`trash_collect_sync.cpp` reads `lookAtActor` + `IsBoundMirrorNative` + `SendGrabIntent`) [V]; the
morph hand-off pile→clump on a GRAB [V]; the divergence-sweep exemption (`pr.mirror`) [V]; retire (with the
rooted-native un-root) [V].

## AS-BUILT — the increments (all committed + hands-on VERIFIED)

### Increment 1 — steady-state spawn seam (`abfaaed8`) [V no-regression hands-on]
`native_pile_mirror::Materialize`; `remote_prop_spawn` routes steady-state (`!IsClaimTrackingActive`)
PILE → native, clump + in-bracket → the bare proxy (scoped `!bracket` so native-spawn and the in-bracket
`TryDestroyTwin` are mutually exclusive → no native+twin dup by construction). **LEAK FIX:** a rooted
runtime native must `RemoveFromRoot` before `K2_DestroyActor` (a rooted PendingKill leaks its slot) — added
in `remote_prop_destroy::DestroyResolvedLocalActor_` + the morph hand-off; no-op on unrooted save-loaded
natives. `pile_hover_gui` DELETED (native GUI is the real fix) + its only consumer `trash_proxy::HasAnyProxy`
removed (RULE 2).

### Increment 2 — re-pile LAND → native (`dabf84de`) [V hands-on] — the observable win
`OnConvert kToPile` on a CLUMP proxy → `RetireProxyActorOnly` (destroy the proxy ACTOR, KEEP the Element)
+ `Materialize(rebindInPlace)` at the landed rest. It's the exact INVERSE of the ToClump morph hand-off:
rebind the Element onto the native IN PLACE first, then destroy only the old proxy actor — the Element never
leaves the manager (the destroy-before-load hazard). New `trash_proxy::RetireProxyActorOnly` = the actor-only
teardown (contrast `RetireProxy`, which also deletes the Element).

### chipType ROOT-FIX (`dabf84de`) [V hands-on] — RE'd, not guessed
Hands-on found the landed native showed the WRONG type/texture. RE (`actorChipPile` bytecode): the pile builds
its mesh in **`init()`** (export 80) — `getChipPileType(chipType) → SetStaticMesh` on **BOTH** its `StaticMesh`
AND `Collision` components; `init` reads `chipType` directly (no random roll) and is called by
UserConstructionScript with the DEFAULT `chipType=0`. So a bare `SpawnActor` skinned a type-0 pile, and an
external single `SetStaticMesh` can never match (two mesh components; `GetStaticMeshComponent` is ambiguous).
Fix = `ue_wrap::prop::SetChipTypeAndRebuild`: write `chipType`, then dispatch the pile's own `init()` — exactly
the `loadData`/`loadPrimitiveData` idiom. The host's chipType already rides the wire (`GetChipType(clump)` at
the re-pile → `PropConvert` payload, the same field the proxy consumed correctly); Materialize now consumes it.

### Rotation sync (`3b72aba0`) [V hands-on, 15:25 clean-join run]
The native ignored the host's delivered rotation and kept its own random UCS roll → host/client diverged
~50% per pile. Fix (host→client, SAME delivery axis as chipType): consume the host's
`GetVisibleMeshWorldRotation` (delivered by `f79bbe84`) and apply it to the visible StaticMesh **COMPONENT's
WORLD rotation** (`SetComponentWorldRotation`, symmetric with the capture) — NOT the actor root, which would
compound on the component's UCS roll = a double-rotate. Threaded through both `Materialize` call sites.
**Authority traced + confirmed HOST-authoritative:** the client sends a throw INTENT (`SendThrowIntent`:
eid+mode+dir, no rotation value); the host executes the throw, settles the physics, broadcasts the rest
rotation; the client applies it. **No client→host inversion.**

> **CORRECTION (2026-07-01): `f79bbe84` is NOT retirable.** The prior version of this doc listed it for
> RULE-2 retirement ("the native does its own rotation"). WRONG: the native must **consume** f79bbe84's
> captured mesh-world rotation to match the host — f79bbe84 is the delivery half. It stays, load-bearing.

### Held-clump-at-join DUP fix (`fa8bc344`) [V hands-on, 15:25] — create-edge claim
A pile the host **moved/carried in the join window** duplicated on the client. Characterized from the 14:49
log as **TWO ACTORS on ONE eid** (identity axis): (a) the client adopts its save-loaded native as eid E's
bound mirror @save-pos (`CreateOrAdoptPropMirror`); (b) the host's re-pile LAND convert-beat-spawn SPAWNED a
second actor because that branch only retired the bound native on a GRAB (`morphBoundNative`), never on a LAND
— and `RegisterPropMirror(rebindInPlace=false)` then REJECTED against the still-bound native
(`identity_create.cpp:111`), so the proxy went into `g_proxies` while the Element stayed bound to the native
= a **split-tracked dup**. The `SweepReconcileSaveTimeTwins` safety net is **structurally blind** to it (it
skips `IsBoundMirrorNative`, and the leaked half IS the bound mirror → `0-of-N` forever; its "dup fixed" log
was a lie for weeks).

Root fix (create-edge reconcile — **prevent, don't clean up**): the missing symmetric HALF of the GRAB morph
hand-off. On a LAND for an eid already bound to a save-loaded native, `native_pile_mirror::RepositionBoundNative`
**reuses** that native (reposition + re-skin + rotate to the landed transform) and **returns before
`SpawnProxy` is ever called** — the second actor is never born, so there is no split-track, nothing to clean,
no destroy-before-load window. GRAB (native→clump) + LAND (reuse native) now close the "convert-beat-spawn on
a bound eid" class as a symmetric pair on the one seam, sharing `SkinPileNative`. Also: honest `[PILE-1C]`
log (0-of-N states its real disjoint domain, no longer claims "dup fixed").

- **A/B proof it was pre-existing, NOT a nativization/rotation regression:** committed `dabf84de` rebuilt
  WITHOUT the rotation change (`35CABA63`), same held-clump join → reproduced the dup. Single variable (code);
  dup persisted → the rotation commit is innocent, the hole is structural (this branch never had the LAND half).

### The dup class — two-owner map (docs/piles/09 class)
- **bound-at-convert** (native already adopted when the convert lands) → the **create-edge LAND claim** (this fix).
- **late-load-unbound** (native async-loads AFTER the convert bound a proxy@new) → `SweepReconcileSaveTimeTwins`
  (disjoint domain; unverified in practice — `0-of-N` in every observed run, i.e. the late-load case didn't
  occur locally — but correctly owned). Full merge of the two owners is a later generalization (fix-then-generalize, N≥3).

## Sound-events — AS-BUILT 2026-07-01 (pending hands-on)
The pickup + land SOUNDS increment. **RE (`research/findings/votv-pile-pickup-land-sound-RE-2026-07-01.md`):
the chipPile/clump BP plays NO dedicated pickup or land sound** — `shovelDig_Cue` is the recycle-to-scrap
action, `flesh_impact_Cue` is a damage/hit reaction, and the clump→pile conversion + the pile's BeginPlay/init
are silent. The native sounds are the physics-material `lib_C::physSound` table: `.soft` = the grab pickup cue
(already synthesized), `.impact` = the land thud (the sibling row). Two receiver-side gaps closed:
- **Client's own grab** (native suppressed → routed to host) was silent for the grabber → play
  `PlayUseClick`+`PlayGrabSound` locally at the aimed pile at both client GrabIntent seams
  (`trash_collect_sync.cpp` OnPileGrabPre).
- **Pile LAND** was silent on the client → new `prop_sound::PlayLandSound` (= `physSound.impact` at the pile,
  vol 1.0/pitch 1.0/att_default) fired on the host-authoritative `kToPile` LAND convert at every genuine-land
  edge in `remote_prop::OnConvert` (native nativize, proxy fallback, `RepositionBoundNative` claim,
  convert-beat-spawn) — never the idempotent echo, never a ToClump grab. Closes the `remote_prop_spawn.cpp:898`
  "impact dust+sound is a deferred polish (needs the correct verb)" TODO.
Deployed `8b0d4576…` (4/4 hash-verified). Runbook: `research/handson_runbook_2026-07-01_pile_sounds.md`.
Open: if a trash material has no `impact` row the land is silent (logs `land thud SKIP`) → pick a fallback asset.

## RULE-2 / cleanup queue (after sound-events)
- Retire `EidForAimedPileProxy`-for-pile (the camera cone is dead once all resting piles are native).
- Convert the in-bracket join-window level-pile path from `TryDestroyTwin`-destroy to bind-the-loaded-native.
- `remote_prop.cpp` is ~1160 LOC (past the 800 soft cap) → extract `OnConvert` into `remote_prop_convert.cpp`.

## Git
`origin/main` `4028c571`; HEAD `fa8bc344` (6 ahead: `f79bbe84`, `530c2f7c`, `abfaaed8`, `dabf84de`, `3b72aba0`,
`fa8bc344`). Deployed `1C242F82` = HEAD, all hands-on verified. Not yet pushed (push pending user OK).
