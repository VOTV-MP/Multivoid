# VOTV pile PICKUP + LAND sound RE — what actually sounds, and the receiver-side synthesis — 2026-07-01

Follow-on to `votv-puppet-sounds-RE-2026-06-11.md` (the grab `use`/soft + throw `swing`
receiver-side synthesis) and `votv-clump-ball-to-pile-conversion-RE-and-event-fix-2026-06-08.md`
(the clump→pile land trigger). Closes the "impact dust+sound is a deferred polish (needs the
correct verb)" TODO left at `remote_prop_spawn.cpp:898` when the catastrophic `turnToPile`-as-land
call was removed (v52).

RE sources (all re-runnable): `docs/piles/re-artifacts/actorChipPile.json` (+ `_erie`),
`research/bp_reflection/prop_garbageClump.json`, disassembled via
`research/bp_reflection/_disasm.py`.

---

## 0. TL;DR — the chipPile/clump BP plays NO dedicated pickup or land sound

Searched every `actorChipPile` + `prop_garbageClump` function for `PlaySound2D` /
`PlaySoundAtLocation` / `*_Cue`. The ONLY chipPile sound calls are:

| Sound asset | Call | Function | What it actually is |
|---|---|---|---|
| `shovelDig_Cue` | `PlaySound2D(self, .., 1.0, 1.0)` | `turnIntoScrap` | The **recycle-into-scrap** action (E-hold that spawns `prop_C`/`prop_food_C` scrap items from the pile's chipType, then `K2_DestroyActor`). NOT a pickup. |
| `flesh_impact_Cue` | `PlaySoundAtLocation(self, .., ImpactPoint+Normal, 1.0, 1.0, att_default)` | `ExecuteUbergraph` (a SphereTrace + BreakHitResult + cast-to-ChipPile + add-HitActor-to-`ignore`) | A **damage / hit reaction** (a wet-splat when the pile is struck/gibbed). NOT a land. |

The **clump→pile conversion** (`ExecuteUbergraph_prop_garbageClump` @27: `BeginDeferredActorSpawnFromClass(pile)`
→ `SetBytePropertyByName(chipType)` → `FinishSpawningActor` → `SetLifeSpan`) plays **NO sound**.
The pile's `ReceiveBeginPlay` + `init` (export 80) play **NO sound**. So there is no explicit
"pile forms / lands" `PlaySound` anywhere in the pair.

**CORRECTION of a stale doc:** `votv-chippile-clump-morph-RE-2026-05-27.md` §2.1/§6.3 reconstructed
`turnToPile` as the chipPile's "I just landed from a clump" entry that "fires impact dust+sound".
That was PSEUDO (reconstructed from field shapes), and the later disassembly disproved it
(`remote_prop_spawn.cpp:898-907`): **`turnToPile` is the GRAB morph** — it spawns a CLUMP, sets
`holdPlayer`, applies velocity, and `K2_DestroyActor`s self. It is NOT a land verb and plays no
land sound. Do not use `turnToPile` for anything land-related.

## 1. So what ARE the native pickup + land sounds? — the physics-material `physSound` table

The sounds the player hears are the GENERIC physics-material system, not chipPile BP code:
`lib_C::physSound(physmat, WorldCtx) -> Fstruct_physSound { step@0, impact@8, soft_30@0x10 }`
(the same lookup `votv-puppet-sounds-RE-2026-06-11.md` documented for grab).

- **Pickup** = the mainPlayer `use` click (2D, grabber-only) **+** `physSound.soft` (the per-material
  pickup cue). Both run only in the LOCAL grabber's `useAction` input chain. Already synthesized on
  the observe path by `prop_sound::PlayUseClick` + `PlayGrabSound` (via `ResolveAndStartDrive` when a
  peer's grabbed-clump drive starts).
- **Land** = `physSound.impact` — the material's IMPACT cue, the SIBLING row of the grab's `.soft`.
  (`prop_C` plays its landing impact via its `physicsImpact` component reading this same row; the
  clump is a plain actor, so on the wire the receiver must synthesize it.)

`impact` is the correct, RE-grounded land asset: it is the game's own data-defined "impact sound for
this surface material," not a guess. If a given material has no `impact` row the lookup's `return`
bool is false → silent (native parity with the `@100067` row-miss gate, same as the grab soft-miss).

## 2. The gaps (why the user heard nothing) + the fix

Two receiver-side gaps, both fixed 2026-07-01 (this increment):

1. **Client's OWN grab is silent.** The client-grab direction SUPPRESSES the native local grab
   (`trash_collect_sync.cpp` OnPileGrabPre nulls `lookAtActor` → `playerGrabbed` skipped) and routes a
   `GrabIntent` to the host. So the grabbing client's own `use`+soft never play (the observe path
   `ResolveAndStartDrive` only covers the OTHER peer watching). **Fix:** play `PlayUseClick` +
   `PlayGrabSound` locally at the aimed pile at BOTH client GrabIntent seams (bound-native +
   camera-cone proxy). The host still hears it natively (it runs `playerGrabbed` on the puppet);
   observers hear it via `ResolveAndStartDrive`.
2. **The LAND was silent everywhere on the client.** The nativized pile materializes inert (no
   physics, no BP land sound), the frozen proxy never simulates a landing, and the clump→pile
   conversion plays no sound. **Fix:** new `prop_sound::PlayLandSound(pileActor)` = `physSound.impact`
   at the landed pile (vol 1.0 / pitch 1.0 / att_default — the `flesh_impact` PlaySoundAtLocation
   params), fired on the host-authoritative `kToPile` LAND convert at every GENUINE-land edge in
   `remote_prop::OnConvert` (native nativize, proxy re-skin fallback, `RepositionBoundNative` claim,
   convert-beat-spawn) — never on the idempotent-echo return, never on a ToClump grab. The land is a
   host-authored EVENT delivered by the convert, so it fires uniformly whether the host or a client
   threw the pile.

## 3. Honesty / open

- `physSound.impact` is the faithful native asset by the game's own data model, but I did NOT find the
  exact native call site that plays it on a *clump* landing (the clump has no `physicsImpact`
  component; `prop_C` does). If hands-on reveals silence (a trash-material `impact` row-miss — the log
  prints `prop_sound: land thud SKIP -- no impact cue`) or the wrong cue, switch the row/asset then.
  The wiring (edge + spatialization) is correct regardless of the chosen asset.
- Files/offsets: `PhysSoundHead { void* step; void* impact; void* soft }` — impact@8, soft@0x10 (x64).
  `CueFromPhysMat(physmat, worldCtx, PhysCue::kImpact|kSoft)` in `prop_sound.cpp`.
