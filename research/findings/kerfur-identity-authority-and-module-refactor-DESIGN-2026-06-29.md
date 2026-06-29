# Kerfur identity authority + module-strictness refactor — DESIGN (2026-06-29)

Status: **DESIGN** (three architecture agents converged; NO code written yet). This is the canonical
plan for the next build. Born from the **10:30 hands-on** (2026-06-29) which proved the prior kerfur
"hang fix" (`150da133`) did NOT resolve kerfurs — it exposed two deeper roots and a whole-codebase
structural defect.

Reading order: this doc → `docs/kerfur/README.md` (kerfur knowledge base) →
`docs/COOP_ENTITY_EXPRESSION_MAP.md` (the dupe matrix) → `docs/COOP_SYNC_MAP.md` (sync layer map).
Related memory: [[project-kerfur-identity-authority-refactor-2026-06-29]],
[[feedback-recurring-bug-is-architectural]], [[feedback-follow-mta-architecture]].

## SCOPE — TOTAL, STRICT, NO EXCEPTIONS (user mandate 2026-06-29)

This refactor touches the **WHOLE project — every file** — and that is **strict**. It is NOT a partial
pass and no file is exempt:
- **Part 3 (taxonomy):** EVERY file under `src/votv-coop/` is placed in a strict, mechanic-named module
  by what it DOES (read the code, never the filename). `coop/` flat root is fully dissolved; `social/`,
  `host/`, `devices/` are fully dissolved/renamed. The `ue_wrap/` engine-wrapper subtree is itself
  reviewed for strict per-engine-class placement (principle 7), and the loader/reflection/hook layer is
  checked for the same module discipline. No "leftover" or "misc" folder.
- **Part 2 (authority):** the passkey boundary is enforced **project-wide** — NO file outside
  `coop/sync` + `coop/element` may mutate identity (compile-time enforced). EVERY file that currently
  touches eid↔actor state is converted to an intent submitter. The audit must enumerate every such file
  and leave ZERO un-converted call sites (a single un-routed `RegisterPropMirror`/`Take`/`FreeId` is a
  failure of the pass).
- **Acceptance for "done":** a tree-wide grep proves no feature file calls the raw identity mutators,
  every file lives under a mechanic-named module, and the two dissolved-folder names no longer exist.
  Anything less is not the refactor the user asked for.

---

## 0. The 10:30 hands-on evidence (what triggered this)

Deployed `E3E6BEAB` (HEAD `3b4e9e02`, contains `150da133` the hang fix). User hands-on at 10:30,
6 save-loaded kerfurs. Result, verbatim:

- **#1 turn-on twitch = CLEARED** (gone; holds from 10:15).
- **#2 turn-off = kerfur creates a DOUBLE on the client.** (The hang fix's physics re-enable was a
  correct sub-fix but the convert path it lives in has a deeper defect.)
- **Join = 5 kerfurs instead of 6**; the 6th only appeared after the host toggled it off/on.

Logs read: host `Game_0.9.0n/...votv-coop.log`, client `Game_0.9.0n_copy/...votv-coop.log` (the 10:30
run). Two roots confirmed from these logs (line cites below).

---

## PART 1 — Kerfur live-fix (two roots; DECOUPLED from the big refactor)

Agent A's verified finding: the kerfur breakage does **not** require the architecture refactor. It is
two targeted roots. The refactor (Part 2) prevents the *next* instance.

### ROOT 1 — the turn-off DOUBLE (arg-slot bug, 2nd occurrence)

`src/votv-coop/src/coop/kerfur_convert.cpp:338`, in `MaterializeKerfurMirror`'s else-branch
(the "no eid-tagged parked ghost to adopt" fallback):

```cpp
coop::remote_prop_spawn::OnSpawn(sp, /*senderSlot=*/0, localPlayer, /*deferKerfur=*/false);
```

The signature (`include/coop/remote_prop_spawn.h:73`) is
`OnSpawn(payload, senderSlot, localPlayer, bool fromConvert=false, bool deferKerfur=true, ...)`.
The `false` lands in the **`fromConvert`** slot; **`deferKerfur` stays at its default `true`**. At
`remote_prop_spawn.cpp:763` the `deferKerfur && classW.find(L"prop_kerfurOmega")` gate then fires
`kerfur_prop_adoption::Arm` — the join-window FUZZY adopter — instead of the inline adopt. That adopter
can't match the local conversion ghost (its fresh per-load key fails the anti-collision key gate at
`kerfur_prop_adoption.cpp:144`), so it **fresh-spawns a second prop** while the orphaned local ghost
remains → the visible double.

This is the **identical bug already fixed once** at `kerfur_prop_adoption.cpp:171-179` (the "OBS-2 ROOT
FIX" comment) — never fixed at this second call site.

**Log proof (client 10:30):** turn-off of NPC eid 5276 →
`remote_prop::OnSpawn key='coopkerfur#5283'` (the synthetic) → `kerfur-prop-adopt: armed deferred
adoption eid=5283` → `applied KerfurConvert oldEid=5276 -> newEid=5283` → `kerfur-prop-adopt: eid=5283
-- no local twin -> fresh-spawning a mirror`. Contrast the CLEAN eid 5274: `claimed 1 local conversion
ghost ... parked` → `adopted parked turn-off ghost as PROP mirror eid=5284 (by eid)` (no arm, no
double). 5274 worked only because its local POLL parked a ghost so adopt-by-eid bound the eid first and
`Arm()` bailed (it no-ops if the eid is already a mirror).

**FIX:** insert the missing positional arg →
`OnSpawn(sp, 0, localPlayer, /*fromConvert=*/false, /*deferKerfur=*/false);`. One line. Mirror the
OBS-2 comment. (Phase 3 below removes the synthetic `OnSpawn` entirely.)

### ROOT 2 — 5-of-6 on join (fire-once reliable convert dropped in the join window)

**Host log proof:** `SendReliable(KerfurConvert) failed K=5275 oldEid=3472 newEid=5274`
(`kerfur_entity.cpp:318-320`; no retry). The host turned a kerfur ON during the join window; the
reliable convert failed to send and was never retried ("host broadcasts each prop once; no retry") →
the client kept the stale off-prop (later retired by `kerfur_reconcile`, client log
`kerfur_reconcile: retired bound-mirror stale off-prop ... eid=3472 ... the failed mid-join
KerfurConvert left the off-prop alive`) and never received the on-form. Permanent divergence until a
manual re-toggle re-broadcast it. Kerfur form is NOT in the join snapshot (prop_snapshot excludes
kerfur actors; EntitySpawn carries only the NPC form), so nothing reconciles a missed convert.

**FIX (MTA-native):** fold kerfur form into the join handoff. Add
`kerfur_entity::ReplayKerfurFormsToJoiner(uint8_t joinerSlot)`: after `SnapshotComplete` is sent
(in `prop_snapshot.cpp`), iterate `g_byKerfurId`, and for every kerfur currently in NPC form (turned
ON since save) **unicast** a `KerfurConvert` to the joiner's slot. The joiner applies it inline during
drain exactly like a live convert — retires the stale off-prop mirror (minted by `save_identity_bind`)
and materializes the NPC. Reuses the existing `KerfurConvertBroadcastPayload` (no new packet type).
**MTA precedent:** `CUnoccupiedVehicleSync::ResyncForPlayer`
(`reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:160-168`) — full-state
resend to the specific joiner for every entity that could have diverged in transit.

Ordering invariant: the replay sends must arrive AFTER `SnapshotComplete` so the off-prop mirror
exists when `OnKerfurConvert` tears it down (both on the reliable ARQ lane, sent sequentially → drain
order guaranteed). Thread-safety: collect records under `g_mutex`, release before `SendReliableToSlot`
(no network I/O under a logic lock).

### Phase 3 — kill the synthetic re-entry (folds into Part 2)

Replace the `MaterializeKerfurMirror` else-branch's synthetic-`PropSpawnPayload`-then-`OnSpawn`
(lines 321-338) with a DIRECT `sync::CreateOrAdoptPropMirror(eid, newActor, key, cls, slot)` call
(spawn via `engine::SpawnActorDeferred` + `SetPropKey` + `FinishSpawningActor`, then bind, then
`SetActorSimulatePhysics(true)`). This removes the layering violation that *created* ROOT 1 (no
synthetic payload, no `OnSpawn` re-entry, no arg-slot risk) and deletes ~17 lines of dead payload
construction (RULE 2). Separate commit, after Part 1 is hands-on-verified.

### Adopter fate (RULE-2 accounting)

| Adopter | Verdict | Reason |
|---|---|---|
| `save_identity_bind` (cursor-ordinal join bind) | STAYS | correct eid-keyed join bind for save off-kerfurs; not fuzzy |
| `npc_adoption` (join NPC kerfurs, class+pose fuzzy) | STAYS | save-loaded NPC twins have only position as anchor |
| `kerfur_prop_adoption` (join off-props, class+pose fuzzy) | STAYS | legit join-window twin adoption; Part 1 fixes its quiescence fresh-spawn arg-slot |
| `MaterializeKerfurMirror` else-branch `OnSpawn` re-entry | **RETIRE (Phase 3)** | the re-entrant join-receiver abuse; replaced by direct keystone call |
| `PollKerfurConversions` death-watch | STAYS | only detector for EX_CallMath-invisible conversions |

---

## PART 2 — `coop/sync` as the single identity AUTHORITY + a watcher (the "strict modules and someone to watch them")

Agent B's diagnosis: VOTV **already faithfully has MTA's four identity primitives** —
`Registry` (=CElementIDs, host/local ID ranges), self-registering `Element` (=CClientEntity ctor/dtor
binds id↔table), per-type `MirrorManager<T>` (=CClientPedManager/ObjectManager master lists),
`ElementDeleter` (=CElementDeleter deferred-destroy funnel). The recurring bug class is **not** missing
primitives. It is three divergences from MTA discipline:

1. **No single lifecycle-decision funnel.** Four files independently DECIDE adopt-vs-spawn-vs-destroy
   for the same eid and race at the join edge: `remote_prop_spawn::OnSpawn` (3 adopt/spawn branches),
   `save_identity_bind` (3 bind entry points), `pile_reconcile` (twin-destroy / pos-correction /
   adopt-candidate), `kerfur_convert` (`MaterializeKerfurMirror` + its own `g_parkedGhosts`
   park/freeze/adopt state machine). MTA has exactly ONE create path (`Packet_EntityAdd` → manager
   ctor) and ONE destroy path (`CElementDeleter`). **Every** recurring VOTV identity bug (09:54
   re-seed-orphan, moved-in-window pile, kerfur-hang, kerfur-double) is "two deciders disagreed about
   one eid in one window."
2. **Scattered satellite reverse-maps** — copies of identity state that read stale vs the bind:
   `kerfur_entity` (`g_actorToKerfurId`/`g_eidToKerfurId`/`g_kerfurMirrorActorToEid`),
   `npc_sync` (`g_actorToNpcId`), `world_actor_sync` (`g_actorToWaId`),
   `prop_element_tracker` (`g_keyToActor`/`g_actorToKey`),
   `kerfur_convert` (`g_parkedGhosts`). The sync-refactor already folded two such maps
   (`g_actorToPropElementId` + `g_boundMirrorNatives`) into the registry's `EidForActor`/`m_saveNative`
   and named "stale reverse map" as the 15:01 pile-dup root. The rest are the same hazard.
3. **No watcher.** MTA's `CClientEntityRefManager` centrally NULLs every cached `CClientEntity**` at the
   single death moment (`~CClientEntity → OnEntityDelete`). VOTV instead litters
   `IsLive`/`IsBeingDeleted` re-checks across files.

### The fix (RULE-1, staged)

- **(a) One create funnel + one destroy funnel per type.** `sync::CreateOrAdoptPropMirror` becomes the
  ONLY site allowed to call `MirrorManager::Install`/`RegisterPropMirror`/`RebindLocalElementActor`;
  add the symmetric `sync::DestroyOrRetire(eid)` as the ONLY `Take`+`Enqueue` site. The
  **decision logic** (adopt-by-eid vs adopt-by-position vs fresh-spawn vs destroy-twin) moves INTO
  `sync` as one ordered resolver per eid. Feature files become **intent submitters**:
  `sync::SubmitSpawnIntent(eid,key,pose,source)` / `SubmitAdoptIntent(eid,candidate)` /
  `SubmitRetireIntent(eid)`; the authority arbitrates.
- **(b) Passkey-enforced boundary.** Only `coop/sync/*` + `coop/element/*` may call the raw
  bind/free mutators (passkey-idiom token; a stray `RegisterPropMirror` from a feature file fails to
  COMPILE). Everyone else holds an `ElementId` and goes through `Registry::Get(eid)` (resolve-by-ID,
  NULL/being-deleted → drop) + the `Submit*Intent` API. This IS "strict modules + a watcher."
- **(c) Fold satellite maps into `Registry`** (continue the refactor): `g_actorToNpcId`/`g_actorToWaId`
  → `Registry::EidForActor`; `g_keyToActor`/`g_actorToKey` → a `Registry` key→eid index;
  `kerfur_entity` maps → `MirrorManager<Kerfur>` record fields (form atomic with identity);
  `g_parkedGhosts` → a first-class `sync` "pending adopt-by-eid candidate" pool (unify with
  `pile_reconcile::FindAndConsumeAdoptCandidate`).
- **(d) Add the watcher.** `coop::element::EntityRefManager` (multimap<Element*, void**> + a
  `ScopedElementRef`/`ElementPtr<T>` smart wrapper); `~Element` calls `OnElementDelete(this)` to NULL
  every cached pointer at the single death moment. VOTV twist: the engine `AActor*` can be GC-purged
  independently of the `Element`, so the watcher guards the C++ `Element*`/cached-actor refs while
  engine-actor liveness stays an `IsLiveByIndex` check — but consolidated in ONE
  `sync::Resolve(eid) → {Element*, liveActor*}` helper, not scattered.

Files that violate the boundary today (route through `sync::` after the refactor): `kerfur_convert`,
`save_identity_bind`, `pile_reconcile`, `remote_prop_spawn`, `npc_sync`, `world_actor_sync`,
`prop_element_tracker`, `kerfur_entity` (see Agent B table).

---

## PART 3 — strict, behavior-derived module taxonomy (the reorg, redone by what each file DOES)

The prior reorg (`daa5c919`/`b5414c83`/`be0794a1`, into `coop/{world,social,host,devices}`) was
filename-guessing and is WRONG. Read by behavior:

- **`social/` is entirely dissolved** (11 files, all misfiled): `email_sync`→`world/` (story-bot
  narrative, not player chat); `wisp_attack_sync`/`wisp_tear_mirror`→`creatures/` (creature combat);
  `player_damage`/`item_activate`/`flashlight_click_sound`/`inventory_pickup_sync`→`player/`;
  `player_inventory_sync`/`inventory_wire`→`items/`; `chat_sync`/`chat_feed`→`comms/`.
- **`host/` is entirely dissolved** ("host" is a software role, not a mechanic): 8 files → `session/`;
  `garbage_sync`→`interactables/` (patches a container interactable, not a session concept).
- **`devices/` → `interactables/`** (the E-press objects), with 4 evicted: `sleep_sync`→`player/`
  (player activity); `order_sync`→`items/` (delivery economy); `atv_sync`/`drone_sync` STAY
  (interactable vehicles/fixtures).
- **`coop/` root flattened** — 42 loose files → `creatures/` (14), `props/` (20), `player/` (6),
  `session/` (15).
- **`interactables/` must NOT nest under `sync/`** — it uses a DIFFERENT identity primitive
  (save-Key strings, its own `key→actor` maps), not `ElementId`/`Registry`. Siblings, not child.

Final tree (every name a game mechanic/aspect): **element, sync, world, interactables, creatures,
props, player, items, comms, session** (+ voice, net, dev unchanged). The full per-file placement table
is in the Agent C output (reproduced in the memory topic file). The `include/coop/` header tree mirrors
`src/coop/` exactly; moves are `git mv` + include-path rewrites + CMakeLists source-list update +
`docs/COOP_SYNC_MAP.md` refresh. No LOC change (flag any file >800 LOC for a separate extraction;
`kerfur_convert.cpp` is at ~975, at the soft cap — add nothing bulky to it).

---

## Recommended build sequence (per "kerfurs first, not everything at once")

1. **Kerfur live-fix (Part 1, ROOT 1 + ROOT 2)** — the live break. Small, decoupled. Build → deploy →
   USER hands-on verify (turn-off = single off-prop that falls+rests; join all 6 materialize). Does NOT
   depend on the refactor.
2. **The reorg (Part 3)** — mechanical, low-logic-risk; establishes the strict folders. STASH the held
   L5 WIP first (`subsystems.cpp`/`interactable_channel.h`/`device_screen.cpp`) — the prior lesson
   (a tree-wide `git add -A` swept the L5 WIP into the first move commit).
3. **The authority + watcher (Part 2)** — the deep change that ends the bug class; kerfur Phase 3 folds
   in. Staged: single funnel → intents-by-eid → passkey boundary → fold satellite maps → watcher.

Honest framing: **the reorg makes it navigable; the authority makes it correct.** The reorg alone does
not stop files racing — Part 2 does. Both were requested ("strict modules AND someone to watch them").
</content>
