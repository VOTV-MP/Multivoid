# Kerfur sync — the definitive RCA + the build-once redesign (2026-06-16)

Status: **DESIGN, approved-pending.** Built from 4 parallel deep-dives (architecture map,
failure taxonomy, game RE/identity, MTA-grounded redesign) that independently CONVERGED on one
root cause. Bytecode-verified where load-bearing. This is the spec we build from; we do not patch
kerfur sync again after this.

---

## 1. THE ROOT CAUSE (one broken invariant)

**A kerfur has no stable cross-peer identity, and our two sync pipelines each mint their own
throwaway identity for it — so every conversion and every client touch creates a new, unmatchable
id.** Concretely:

- A kerfur is the ONLY entity that is both a host-authoritative AI **NPC** (`kerfurOmega_C`,
  ACharacter) and a grabbable **prop** (`prop_kerfurOmega_C`, Aprop), with a player turn_off
  (NPC→prop) / turn_on (prop→NPC) conversion. The conversion spawns are `EX_CallMath`
  (ProcessEvent-invisible), so we detect them with a 5 Hz death-watch poll.
- The NPC pipeline (`npc_sync`/`npc_mirror`/`npc_adoption`) identifies a kerfur by a **host-range
  eid only** (no key). The prop pipeline (`remote_prop_spawn`/`prop_element_tracker`/
  `trash_collect_sync`) identifies it by **key + eid**, and on a CLIENT mints a **peer-range** eid
  (`MarkPropElement` → `AllocLocalId`, range ≥32768).
- **The kerfur's save Key is random per peer** — bytecode-proven: `AkerfurOmega_C::loadData`
  overrides the base and NEVER restores `Key` (it restores only hasFloppy/makeSentient/floppyType),
  so the gamemode mints a fresh `generateRandomKey` on every load on every machine. (Base
  `Aprop_C::loadData` DOES restore `self.key=data.key`, so an ordinary prop is stable — the kerfur
  NPC override is the entire defect.) → a kerfur prop **never key-matches across peers**.
- So: a client that grabs/touches an untracked kerfur prop mints a **peer-range** eid for it
  (`EnsureHeldItemBroadcast` → `MarkPropElement`). But conversion is **host-authoritative and
  host-range-only**: `kerfur_convert::OnConvertRequest` gates on `IsAllowedHostAllocatedEid` and
  DROPS any eid ≥32768 (`"request eid=X outside the host range -- dropped"`). The host's redundant
  poll then fires a parallel conversion, and `IngestSpawnedConversionProps` re-ingests the client's
  mirrored prop as a FRESH host entity (it trusts the reverse map `g_actorToPropElementId`, which is
  blind to wire mirrors, instead of `MirrorManager::IsMirror()`). → a self-amplifying
  **dupe-and-drop loop.**

Every conversion is a **destroy-old + create-new-with-a-different-eid** across two disjoint eid
spaces. Identity is re-derived from scratch (class + nearest pose) every time, because there is no
stable token to carry. That re-derivation is where every prior fix lived — and why each held for one
path and broke through another.

## 2. THE SEVEN FAILURES (all one root)

| # | Failure | Mechanism (file) | Root invariant violated |
|---|---|---|---|
| 1 | Client conversion request dropped "outside host range" | client poll sends its prop mirror's PEER-range eid; host `OnConvertRequest` host-range gate drops it (`kerfur_convert.cpp`) | conversion target must be host-allocated |
| 2 | Host re-ingests its own mirror of a client's held kerfur prop as a fresh host prop (the "ingested 2 from one turn_off") | `IngestSpawnedConversionProps` trusts `g_actorToPropElementId` (blind to mirrors) not `IsMirror()` | IsMirror() is the authority, not the reverse map |
| 3 | "express-if-unknown" mints a client identity for a save kerfur prop the client just picks up | `EnsureHeldItemBroadcast` `:192-211` self-heal + client-range `MarkPropElement` | a save prop has one deterministic id (false for kerfur) |
| 4 | Carried kerfur prop vanishes from hands after 4 s | `CleanupParkedGhosts` orphan-reaps because the host dropped the request (#1) | "un-confirmed ghost ⇒ destroy" conflated with a real reject |
| 5 | Host poll fires a redundant conversion in parallel with the dropped client request (dual-driver, two eids one actor) | `PollKerfurConversions` host branch converges while the request path also runs | one physical kerfur ↔ one eid |
| 6 | Grab-of-a-host-mirror re-broadcasts it client-owned → host re-mirrors (the cascade) | same tracker-blindness as #2/#3, client side | a client must never re-own a host-mirrored prop |
| 7 | Sentient host-reject → client mirror missing until reconnect | client cancels locally + requests; host silently no-ops; ghost reaped (#4) | host must always echo the result (act or reject) |

## 3. GAME RE — is there a stable id? **NO. The host must synthesize one.** (bytecode-verified)

- NPC `loadData` discards the saved key (random mint every load) — no cross-peer NPC identity.
- `dropKerfurProp` (turn_off) copies only `sentient` onto the new prop; `spawnKerfuro` (turn_on)
  copies NOTHING onto the new NPC. **The conversion transfers no identity in either direction** — not
  Key, not name, not a save struct. Even the prop's stable key is not handed to the NPC at turn_on.
- The save layer has no stable per-kerfur index/GUID (`Fstruct_save` = class + transform + the random
  key only; `objectsData` array position is not stable across saves).
- turn_off spawns **exactly ONE** `prop_kerfurOmega_C` body (+ optional floppy gated on `hasFloppy`).
  The "2nd body prop" seen in logs is a sync artifact (Failure #2), not the BP.
- **The two deterministic anchors the game DOES give us:** (a) at world-load, every peer spawns each
  kerfur at the same saved `(class, transform)` — the basis for connect-edge class+pose adoption
  (keep it); (b) **positional continuity across conversion** — both verbs spawn the replacement at the
  original actor's location, so the host can bridge "NPC vanished here ↔ prop appeared here" = same
  logical kerfur. And the host OWNS the `Key` field (the BP only ever writes the random mint), so the
  host may stamp a synthetic GUID onto both forms' Key without the BP clobbering it.

**Conclusion:** the host is the sole authority for kerfur identity. It mints one stable id per logical
kerfur and carries it across the BP's identity-less conversion via position continuity (the poll/verb
already give the host the before/after actors).

## 4. TARGET ARCHITECTURE — one stable KerfurId, in-place form-mutate (the MTA shape)

MTA precedent (`reference/mtasa-blue/.../rpc/CElementRPCs.cpp` `SetElementModel`; `CElementIDs::
GetElement`): ONE server-assigned stable element ID per logical entity; a model/state change MUTATES
the type on the existing element (`pPed->SetModel(...)`), never destroy+recreate; the client obeys
create/destroy/mutate by that ID and never invents identity.

Ported to the kerfur:

1. **`KerfurId` = one host-allocated, host-range id per logical kerfur, spanning BOTH forms.**
   Allocated by the host only (`Registry::AllocHostId`), at first sight (world-enum for an NPC-form
   kerfur; the conversion-ingest for a prop-form; the connect snapshot for a save prop). Carried on
   the wire in the existing `elementId` field of EntitySpawn (NPC form) and PropSpawn (prop form) — no
   new id field needed.

2. **A new element type `KerfurEntity` (`ElementType::Kerfur`)** owning `{m_actor, m_form
   (Npc|Prop), m_npcClassName, m_propClassName}`, in new `coop/kerfur_entity.{h,cpp}`. It exposes
   `AllocKerfurId` (host), `GetKerfurById`, `GetKerfurByActor`, and the core operation
   **`BindFormActor(kerfurId, newActor, form, className)`** — the `SetElementModel` equivalent:
   destroy the old-form actor, spawn the new-form actor, rebind the SAME element/id in place.

3. **Clients NEVER mint a kerfur identity.** Class-gate both entry points: `MarkPropElement` and
   `EnsureHeldItemBroadcast` early-return for kerfur classes (`IsKerfurClass`). A kerfur prop is a
   host-owned entity a client may merely HOLD (like holding an NPC), not an untracked prop to express.

4. **The host is the sole authority AND the only peer that runs the BP conversion verb.** Clients
   never convert locally → **no client ghosts** → the entire parked-ghost claim/adopt + 4 s
   orphan-reap machinery is RETIRED.

### The conversion flow (both initiation paths, no client ghosts)

- **Client initiates** (radial menu on a kerfur mirror): the `actionName`/`actionOptionIndex` PRE
  interceptor CANCELS the local dispatch (so the mirror is NOT converted locally — no ghost) and sends
  `KerfurConvertRequest(kerfurId, toProp)`. The host runs the real verb (`CallFunction`) on its
  authoritative actor (KerfurId→actor), reads the new actor (the verb spawns synchronously), calls
  `BindFormActor`, and broadcasts `KerfurConvert(kerfurId, toForm, transform, newClassName)`. Every
  client (incl. the initiator) applies it as an in-place mutation. On a host REJECT (sentient/kill),
  the host echoes `KerfurConvert(kerfurId, rejected)` so the initiator restores its mirror (fixes #7).
- **Host initiates** (host's own radial menu): the host runs the verb natively; the poll (backstop for
  the EX_CallMath-invisible spawn) detects the old actor died + finds the new actor by position
  continuity → `BindFormActor` + broadcast `KerfurConvert`. (Or the host interceptor runs the verb
  explicitly to skip the search — implementation choice in K-4.)
- A client holding a kerfur prop streams its pose by `KerfurId` after a `KerfurHoldRequest(kerfurId,
  held)` to the host — never minting a peer-range eid.

This makes the conversion an idempotent, identity-preserving mutation: replaying the same
`KerfurConvert` K times converges to the same single kerfur at the same KerfurId.

## 5. PROTOCOL (bump to v78)

- `KerfurConvert` (repurpose slot 63, was `KerfurConvertRequest`): now ALSO the host→all broadcast.
  Payload extended: `kerfurId, toForm, rejected, locX/Y/Z, rotP/Y/R, newClassName[64]`.
- `KerfurConvertRequest` keeps the client→host direction (carries `kerfurId, toProp`) — the host
  runs the verb; the result rides `KerfurConvert`. (Do NOT delete the request — it's required for
  client-initiated conversion; only the host-side dispatch CHANGES to run-verb-then-broadcast.)
- `KerfurHoldRequest` (new): `kerfurId, held` — client→host, held-kerfur ownership.
- Kerfur prop join-snapshot rides PropSpawn with a new `kFlagKerfur` physFlags bit + `kerfurId` in
  `elementId` (route to `BindFormActor(Prop)` not the generic prop mirror). No new ReliableKind.
- `EntitySpawn`/`EntityDestroy`/`PropSpawn`/`PropDestroy` unchanged on the wire; the kerfur just owns
  its `elementId` in the `KerfurEntity` table instead of the Npc/Prop tables.

## 6. DELETED per RULE 2 (no stubs, no flags, no parallel paths)

- `IngestSpawnedConversionProps` (reverse-map-blind re-ingest) — Failure #2/#6 source.
- The peer-range kerfur-prop minting (kerfur class-gate in `MarkPropElement` + `EnsureHeldItemBroadcast`).
- The parked-ghost claim/adopt + `CleanupParkedGhosts` 4 s orphan reap (no client ghosts now). NOTE:
  `FindParkedGhostNpcNear`/`AdoptExistingNpcAsMirror` for the NPC join-adoption stay (that's the
  save-load class-match path, a different mechanism).
- The poll's request-vs-converge **dual driver** collapses to: poll = host-initiated detection only;
  client-initiated = request→host-verb→broadcast.
- savePersisted class-match adoption is NOT used for the conversion path anymore (only connect-edge
  save load). The `NpcEnumOrigin::MidSessionConverge ⇒ savePersisted=0` rule (already specified) stays.
- `kerfur_command` (the v74 menu-verb relay) — re-evaluate; delete if unused (already flagged debt).

## 7. PHASED PLAN (each phase independently buildable + a NET improvement + smoke-tested)

Ordering is chosen so no phase regresses held-kerfur sync (the minting-block needs the hold path).

- **K-0 (prereq, behavior-preserving):** extract `npc_world_enum.{h,cpp}` from `npc_sync.cpp`
  (942 LOC, over cap). No wire change. Smoke: connect-edge adoption still logs `npc-adopt: bound ...`.
- **K-1 (isolated relief):** `NpcEnumOrigin::MidSessionConverge ⇒ savePersisted=0` (the per-cycle 8 s
  pop-in / class-false-bind). ~6 lines, no protocol. Smoke: `mp.py kerfurtoggle` 10× — no `npc-adopt:
  armed` for conversion eids, kerfur appears immediately on turn-on.
- **K-3 then K-4 then K-5 (the unification):** introduce `KerfurEntity` + `AllocKerfurId` (K-3, no
  wire); the `KerfurConvert` host-broadcast + `BindFormActor` in-place mutation + delete the
  dual-driver/ingest (K-4, v78); the `KerfurHoldRequest` + `kFlagKerfur` join-snapshot + the
  client-mint class-gate (K-5 — the gate lands WITH the hold path so held kerfurs still sync). Smoke
  each: same single `KerfurId` across all 10 toggle cycles in both logs; grab a kerfur prop → no
  PropSpawn with a peer-range eid; join with a prop-form kerfur → one mirror, no dupe.

(K-2 "class-gate" from the raw plan is folded into K-5 so it can't regress held-prop pose before the
hold path exists.)

## 8. RISKS + VERIFICATION

- **K-4 race** (host broadcasts before the initiator's mirror is ready): benign — the initiator
  cancelled locally, so it just applies `KerfurConvert` when it arrives; reliable-lane ordering keeps
  the initial spawn before any convert. **Verify:** no `MirrorManager::Install duplicate-eid` warns.
- **Dev-test path:** `kerfur_toggle` calls `dropKerfurProp` directly (bypasses `actionName`), so it
  would convert locally. Update the test trigger to send a `KerfurConvertRequest` instead (match real
  radial behavior) so the no-client-ghost invariant holds in the autonomous test too.
- **Connect-edge regression guard:** a 2nd client joining mid-session must still adopt a save kerfur
  by class+pose (`savePersisted=1` at `ConnectEdge`). Keep + smoke.
- The whole thing is gated behind the autonomous `mp.py kerfurtoggle` (10×) + a new `kerfurgrab`
  probe + a save-transfer-join-with-prop-kerfur smoke, before any hands-on.

## 9. WHY THIS CANNOT BREAK AGAIN

One host-allocated `KerfurId` per logical kerfur, O(1)-resolved, that never changes across form or
ownership. Clients cannot mint a kerfur identity (both entry points gated). Conversion is one
`BindFormActor` mutation carrying the same id into the new form — no destroy+create across a pipeline
boundary, no per-cycle savePersisted decision, no class-only adoption guess, no reverse-map ingest, no
dual driver, no client ghosts. The identity re-derivation that every prior fix lived in is gone,
because identity is never re-derived.
