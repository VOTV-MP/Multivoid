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

---

## 10. CORRECTED DESIGN (2026-06-16, post-RE + 3-agent synthesis) — READ THIS, it supersedes §4.2.4

Before implementation, one RE agent (bytecode + runtime-log census) + two independent architects
re-derived the design. The RE agent's verdict is **definitive and corrects §4** of this doc:

### 10.1 The interceptor does NOT fire on the real menu — KEEP claim/adopt (the §4.2.4 reversal)

`mainPlayer_C::useSelectedAction` invokes `actionName`/`actionOptionIndex` via **`EX_LocalVirtualFunction`**
(proven from raw cooked bytecode `research/bp_reflection/mainPlayer.json`), which UE4.27 routes through
`ProcessInternal`/`ProcessLocalScriptFunction` — *beneath* the single `ProcessEvent` frame our one MinHook
detours. Runtime logs confirm: the interceptor-cancel lines have **never** fired in any real session
(even after the `NameEquals` case-insensitivity fix — that fixed *resolution*, not *reachability*); the
death-watch POLL carries 100% of detections. **The client's local conversion is therefore UNAVOIDABLE.**
§4.2.4's "interceptor cancels → no ghost → retire claim/adopt" is **struck**. A true pre-cancel would
need a second hook engine on `ProcessInternal` — rejected: `ProcessInternal` fires on *every* internal
script call (orders of magnitude hotter than `ProcessEvent`); a detour there is the FPS-killing hot-path
pattern CLAUDE.md forbids, for the marginal benefit of eliminating a ghost that adopts cleanly.
**Accept-and-adopt anchored to the stable id is the correct architecture (optimistic-local + authoritative-reconcile — standard netcode, MTA included).**

### 10.2 Registry model — host-side authority, kerfur stays a real Npc/Prop mirror (NO surgery)

`Registry::m_byId[id]` is single-valued, so the kerfur cannot be a `KerfurEntity` element AND an Npc/Prop
mirror at one id. Resolution (both architects converged): **`KerfurEntity` is a HOST-side authority element
(`ElementType::Kerfur=4`, `AllocHostId` reserves the stable `KerfurId K`), held in a host-only table — NOT
in `MirrorManager<Npc/Prop>`.** The kerfur's *rendered* form is a normal **Npc mirror (NPC form, eid N)**
or **Prop mirror (prop form, eid P)** at its own independent host-range eid, in the existing managers —
so `npc_pose_host`/`npc_pose_drive` + the prop pose/physics pipelines are **unchanged** (no regression,
full held/thrown-prop fidelity). `K` is the durable handle; `N`/`P` are the per-form wire eids. At any
instant only `K` + the current-form eid are live (old-form eid freed on conversion).

### 10.3 `KerfurConvert` (host→all) is the SOLE conversion-transition packet

It REPLACES the EntityDestroy+PropSpawn choreography *for conversions* (initial spawns still use
EntitySpawn/PropSpawn). Payload: `{kerfurId K, toForm, newEid, locX/Y/Z, rotP/Y/R, newClassName[64],
rejected, isReply}`. Host on conversion: run the verb → register the verb's output actor via the normal
Npc/Prop pipeline (host-range `newEid`) → `BindFormActor(K)` updates the host table → broadcast
`KerfurConvert`. Every client's handler: `rejected` → no-op (mirror stays; fixes #7); else → destroy the
old-form mirror (at the `K→oldEid` map) + **adopt a claimed local ghost to `newEid` if present (initiator),
else materialize a fresh mirror at `newEid`** (synthetic EntitySpawn/PropSpawn → existing OnEntitySpawn/
OnPropSpawn) + update `K↔eid`. `newEid` is host-allocated → never peer-range → never rejected.
`KerfurConvertRequest` (slot 63, extended) stays client→host, carrying `K + toProp`.

### 10.4 Net delta vs the current shipped code (what actually changes)

KEEP: the POLL detection, `ClaimConversionGhosts`/`FindParkedGhostNpcNear`/`CleanupParkedGhosts` (the
ghost adopt — RE-mandated), the Npc/Prop pose pipelines, the connect-edge save-load class-match adoption.
ADD: `coop/kerfur_entity.{h,cpp}` (`ElementType::Kerfur`, the host table, `AllocKerfurId`/`GetKerfurById`/
`GetKerfurByActor`/`IsKerfurEid`/`BindFormActor`/`ApplyFormChange`), the `KerfurConvert` broadcast +
`KerfurHoldRequest=74` + `kFlagKerfur=0x40` (proto **v78**), the **class-gate** in `MarkPropElement` +
`EnsureHeldItemBroadcast`. DELETE (RULE 2): `IngestSpawnedConversionProps`, the request-vs-converge dual
driver. The ghost-adopt target flips from a fuzzy Gap-I-1 race to the authoritative `newEid` from
`KerfurConvert` — that determinism, plus the no-mint class-gate + the stable `K`, is the root fix.

### 10.5 Phases (each builds + `mp.py kerfurtoggle` 10× smokes)
- **K-3** (no wire): `ElementType::Kerfur` + `kerfur_entity.{h,cpp}` table; populate it at kerfur
  registration (host `AllocKerfurId`) + client `RegisterClientKerfurId`. Conversion still runs the OLD
  path — the table is built but not yet driving. Behavior-unchanged; smoke must still PASS.
- **K-4** (v78, core): `KerfurConvert` packet + `BindFormActor`/`ApplyFormChange` + delete
  `IngestSpawnedConversionProps` + collapse the dual-driver. The new path REPLACES the old.
- **K-5**: class-gate (`MarkPropElement` + `EnsureHeldItemBroadcast`) + `KerfurHoldRequest` + `kFlagKerfur`
  join-snapshot + update the `kerfur_toggle` dev trigger to request (not call the verb directly).

---

## 11. AS-BUILT PROGRESS + the precise K-4b/K-5 plan (2026-06-16, session 19)

**DONE + verified (committed, pushed, smoke-PASS):**
- **K-0** `da13665a` — `npc_world_enum` extracted from `npc_sync` (behavior-preserving; audit CLEAN).
- **K-1** — `MidSessionConverge ⇒ savePersisted=0` preserved in the moved code (already shipped a25f636c).
- **K-3** `7a429abe` — `ElementType::Kerfur=4` + `coop/kerfur_entity.{h,cpp}`: the HOST-side KerfurId table.
  Verified live: `kerfur_entity: reserved KerfurId=2278 ... currentEid=2277` (K distinct from the per-form
  eid). `kerfurtoggle` smoke PASS (zero regression). Populated only via the world-enum/converge path so
  far (the fresh-spawn + the conversion driving land in K-4b).
- **K-4a** `36678977` — protocol **v78** wire definitions ONLY (build-clean, behavior-neutral):
  `KerfurConvert=74` (host->all, `KerfurConvertBroadcastPayload` 104B `{kerfurId, oldEid, newEid, toForm,
  rejected, transform, newClassName}`), `KerfurHoldRequest=75` (`KerfurHoldPayload` 8B), `kFlagKerfur=0x40`,
  `KerfurConvertPayload.elementId` now carries the stable KerfurId. NOT yet sent/handled.

**KEY SIMPLIFICATION (decided, not yet applied):** the CLIENT stays **eid-based** — it does NOT track
KerfurIds. So in K-4b **REMOVE the client maps + `RegisterClientKerfur`** from `kerfur_entity.{h,cpp}`
(landed in K-3 scaffolding, turned out unused). The client uses `IsKerfurClass(actor)` (the class-gate
predicate) + the `oldEid`/`newEid` carried in `KerfurConvert`. Only the HOST owns the KerfurId table.

### K-4b — the conversion rewrite (proto v78 goes live). File-by-file:

1. **Full host KerfurId population at FIRST sighting** (so every kerfur has a KerfurId BEFORE any
   conversion): add `AllocKerfurId` in `npc_sync.cpp`'s `NpcSpawn_POST` (fresh kerfur NPC: dev-spawn /
   purchase) gated on `kerfur_entity::IsKerfurActor` + in the host's kerfur-PROP first registration
   (prop_element_tracker / remote_prop host path) with `Form::Prop`. (world-enum already done in K-3.)
2. **`kerfur_entity` (host) `BindFormActor(kerfurId, newActor, newIdx, newEid, newForm, className)`** —
   the SOLE conversion signal. Update the record (K -> newForm/newEid/newActor) + broadcast
   `KerfurConvert(K, oldEid, newEid, newForm, transform, className)`. Reject path: actor still live ->
   broadcast `rejected=1` (no transform).
3. **`kerfur_entity` (client) `OnKerfurConvert(payload)`** — `rejected` -> no-op. Else: destroy the mirror
   at `oldEid` (`npc_mirror::DestroyMirrorForEid` for an NPC old-form / a new `remote_prop::DestroyPropMirrorForEid`
   for a prop old-form -- both = Take + K2), then at `newEid`: ADOPT the claimed local ghost (NPC:
   `kerfur_convert::FindParkedGhostNpcNear` -> `npc_mirror::AdoptExistingNpcAsMirror(ghost, newEid, cls)`;
   PROP: the existing Gap-I-1 frozen-ghost adopt, now keyed to the authoritative `newEid`) ELSE materialize
   fresh (`npc_mirror::SpawnFreshNpcMirror` / the remote_prop materialize-at-eid).
4. **SILENT host element ops** (so `KerfurConvert` is the ONLY wire signal -- no redundant
   EntityDestroy/PropSpawn for a conversion; RULE 2): add `npc_sync::ReleaseNpcElementSilent(eid)` (Take
   from NpcMirrors + erase the reverse map, NO `SendEntityDestroy`) + a silent host prop register/release
   (AllocAndInstall + reverse-map, NO `SendPropSpawn`). `BindFormActor` calls the silent release (old form)
   + silent register (new form, -> newEid). [If the silent ops prove tricky, the lower-risk fallback is to
   keep the existing EntityDestroy/PropSpawn pipeline and let `KerfurConvert` PRE-EMPT the adopt -- the
   redundant spawn then hits the dup-eid drop. Prefer the silent ops (proper); fall back only if blocked.]
5. **`kerfur_convert.cpp`**: DELETE `IngestSpawnedConversionProps` (+ its `ConvergeAfterConversion` call) +
   the now-dead ghost-machinery includes if they become unused. Rewrite `ConvergeAfterConversion` to resolve
   the new-form actor + call `BindFormActor`. `OnConvertRequest`: `payload.elementId` is the KerfurId ->
   `kerfur_entity::GetKerfurById` -> current actor -> run verb -> `BindFormActor`. The client poll's
   `SendConvertRequestDirect`/`SendQueuedRequest` send the KerfurId (`GetKerfurIdForEid(mirrorEid)`).
   Collapse the dual-driver (host poll = host-initiated detection only; client = request only).
6. **Routing** (the 3-place rule): `event_feed.cpp` add `KerfurConvert` + `KerfurHoldRequest` to the
   `HandleStateEvent` family list (line ~441); `event_dispatch_state.cpp` add a `KerfurConvert` case
   (CLIENT-only, senderPeerSlot==0 gate -> `kerfur_entity::OnKerfurConvert`) + a `KerfurHoldRequest` case
   (HOST-only -> `kerfur_convert::OnHoldRequest`, K-5).
7. Build -> deploy x4 -> `mp.py kerfurtoggle` 10x. PASS = same KerfurId across all 10 cycles on both logs,
   NO `IngestSpawnedConversionProps`/`outside the host range`, the `KerfurConvert` lines present.

### K-5 — client-mint class-gate + held-kerfur (the user's #1 pain: grab-dupe):
1. `prop_element_tracker.cpp` `MarkPropElement`: early-return `if (kerfur_entity::IsKerfurClass(cls)) return;`
   (a kerfur prop's identity is host-managed; a client must NEVER mint a peer-range eid for it -- this kills
   Failures #1/#3/#6).
2. `trash_collect_sync.cpp` `EnsureHeldItemBroadcast`: `if (kerfur_entity::IsKerfurActor(heldActor)) {`
   send `KerfurHoldRequest(kerfurMirrorEid, held=1)` (the host-range eid the client's prop mirror already
   has) `; return false; }` -- the host's `OnHoldRequest` records the holder so its prop-pose authority cedes
   to the holding client (shared-entity, like the ATV). NO peer-range mint.
3. `kerfur_entity::OnHoldRequest(payload, slot)` (host): record slot<->kerfur; cede pose authority.
4. `remote_prop_spawn::OnSpawn`: `if (physFlags & kFlagKerfur)` route the prop-form join-snapshot through
   the kerfur path (the host stamps `kFlagKerfur` when it connect-broadcasts a prop-form kerfur).
5. Update `coop/dev/kerfur_toggle` to send a `KerfurConvertRequest` (not call `dropKerfurProp` directly) so
   the no-client-ghost-mint invariant holds in the autotest. Add a `kerfurgrab` smoke (client grabs a kerfur
   prop -> NO peer-range PropSpawn; `KerfurHoldRequest` on both logs).

KEEP throughout: the POLL detection, `ClaimConversionGhosts`/`FindParkedGhostNpcNear`/`CleanupParkedGhosts`
(RE-mandated -- the PE-invisible local conversion is unavoidable), the Npc/Prop pose pipelines, and the
connect-edge save-load class-match adoption (`AdoptExistingNpcAsMirror` via `npc_adoption`).
