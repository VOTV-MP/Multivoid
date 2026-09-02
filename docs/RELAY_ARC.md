[withdrawn at the author's request]

[withdrawn at the author's request]
[withdrawn at the author's request]
[withdrawn at the author's request]
> (local-only, like every research citation; §8 = pass-1 adoptions, §12 = pass-2, §12.8 = the
> adversarial audit that found no CRITICAL). The study's verdicts stand behind this arc and are
[withdrawn at the author's request]
> architecture; what we take from it are the pieces where its formulation is better than what we
> currently hold, each as a RULE-1 root fix, credited to Relay as precedent the same way we credit
> MTA files.
>
> **User green light 2026-09-02, verbatim: «Да на всё да, но в следующей сессии»** (the pass-1
> queue), followed the same evening by the instruction this doc answers: *«теперь сделаем план и
> горячий документ что в итоге адаптируем в наш мод. что сделает его лучше»*.
>
[withdrawn at the author's request]
> all-rights-reserved). We adopt SHAPES measured from its behavior/strings/bytecode — never
> bytes, never decompiled text. Same discipline as the MTA/RE-UE4SS porting rules; each shipped
> piece carries a one-line precedent comment (Relay + the study section), like MTA citations.

---

## 0. The queue at a glance

Order = the user's green-lit pass-1 order with the pass-2 items inserted where they fit; WP-2's
slot is the one insertion made on recommendation (it is independent of everything else and
attacks a live field-defect class). Statuses: ☐ not started · ◐ in progress · ☑ done.

| WP | What | Status | Entry gate | Standing item it fixes |
|---|---|---|---|---|
| 0 | PUSH main (pre-arc housekeeping, not an adoption) | ☐ | 5-axis leak audit first; user-approved push | the un-pushed `50826d53..` chain |
| 1 | **The gate seam** — dispatch-ladder tier 4, our own build | ☐ | census of gateable-vs-ubergraph verbs → **/qf to convergence** | VISIBILITY:103 "only remaining invisible class"; C5; A54 enforcement points |
| 2 | **The listener seam** — `ue_wrap/core/object_listeners`, zero-import | ☐ | **/qf to convergence** | the dark-consumer `FindObjectByClass` class (136/~52 sites); CachedObjRef ABA |
| 3 | **Inventory NOW-invariants** — exit-ledger + equipment key-integrity | ☐ | short /qf (no architecture change) | container #4's silent-loss class, fail-loud |
| 4 | **Inventory base decision** — host-canonical per-peer `GObjStack` partitions | ☐ | **/qf to convergence** (foundation-first gate) | A4's root; the half-canonical split behind #4/#6 |
| 5 | **Declarative quiesce primitive** — `coop/element` park declarations | ☐ | folds into the power-chain /qf resume (R10) | WORLD_PROP_DIVERGENCE hand-rolled parks; power R10's bespoke watch |
| 6 | **Lease expiry + revision fencing** — SYNCER_MODEL amendment | ☐ | design-doc edit + /qf rider on the arbiter design | handoff tail races (the ATV wheel-spawn class) |
| 7 | **Smalls batch** (7a-7g below) | ☐ | per-item, most need none | observability, drill debts, UX |
| 8 | **Recorded-not-built** (8a-8d) — decisions parked ON PURPOSE | — | n/a | so nothing here is re-derived |
| 9 | **Declined whole** (9a-9g) | — | n/a | so nothing here is re-opened by accident |

Dependency edges that order the queue: WP-1's verdict feeds WP-4 and WP-5's consumers (several
suppression wishes become gates if WP-1 lands — the power decay kill, the `em_equip` class, C5's
destroy verb); WP-5 is consumed by the power-chain R10 resume (paused at R9, resumes WITH the
quiesce primitive as its mechanism); WP-4 is a foundation-first GATE on rebuilding container
facets #4/#6 and the personal-container fail-closed hole — do not rebuild those on the
half-canonical base before WP-4's decision; WP-6 is read by any syncer/lease work that resumes
(ATV arm b2 included). WP-2 is independent of all of them.

---

## 1. WP-1 — THE GATE SEAM (pass-1 table-turner; study §5 mechanism, §8.1 adoption case)

**What.** A fourth tier in the dispatch-seam ladder (`COOP_SYNC_DOCTRINE.md` step 3): an
in-memory Blueprint-bytecode prologue on a target SCRIPT function — `EX_JumpIfNot` + a call to
our own engine-resident zero-param marker — giving observe + full args + **CANCEL** on **every
dispatch route**, including the `EX_Local*` routes that are invisible to both our ProcessEvent
detour and the Func seam. Field-proven by Relay in production; our implementation is our own.

**Why it makes the mod better (concrete lanes it unblocks):**
- **C5 dies properly** (`CRUTCHES.md`): the coin-gun refused sale currently still costs the item
  because the client's destroy verb cannot be cancelled — gated, the act-as-host inversion is
  buildable in its correct form (suppress the producer, not the receive side).
- The laptop PC lane's press-seam mint (parked design leans on vm_dispatch fan-out + a Func
  proxy) gets its clean seam.
- The power lane's client-side decay kill becomes a gate on the roll instead of a 1 Hz
  assert-dead watch (interacts with WP-5 — R10 picks whichever the /qf verdicts say).
- The `em_equip`-class inventory verbs become suppressible at the verb (feeds WP-3/WP-4).
- Every future "client authored shared state at an EX_Local*-dispatched verb" finding (A54's
  enforcement points) has a closing tool instead of a per-site reconcile.
- Kills the on-disk Option-C fork: the A6-amendment question likely never needs to be asked
  (`COOP_VM_DISPATCH_PLAN.md` §6 addendum).

[withdrawn at the author's request]
bodies are NOT gateable (every event stub embeds the uber entry offset as a constant; the uber
re-enters via `EX_ComputedJump`) — so a per-verb `bp_cfg` census decides seam choice and
event-graph logic stays on tiers 1-3 + reconcile; restore-on-map-travel bookkeeping is mandatory
(our world-identity module owns the trigger); a FOREIGN prologue (a Relay-gated function) must be
REFUSED, never double-patched — coexistence both ways; the `Script` write is a heap swap of the
array data pointer, principle-1-legal (assets on disk untouched); jump-operand relocation covers
the `{0x06,0x07,0x4C,0x5B,0x69}` offset family; min-prologue ~17 bytes; `UStruct::Script` is a
field read at +0x60 on 4.27 (re-verify at /qf time — the layout-template source is not vendored).

**The marker fork (decided IN the /qf, both arms named now):** the marker must be an
engine-resident UFunction. Arm A = a `MultivoidLib.pak` stub (we already ship LogicMods paks;
[withdrawn at the author's request]
per world). Arm B = a runtime-minted UFunction (no new pak; construction cost + lifetime
questions). Arm A doubles as the seed of WP-8a's future mod API.

**Entry gate:** the gateable-vs-ubergraph census over our CURRENT suppression wishlist (C5 verb,
`em_equip` family, power roll, laptop press — `bp_cfg` each), then /qf to convergence. No code
before both.

**Acceptance:** a drill that gates a throwaway verb and shows RED (cancelled) and GREEN
(restored after travel); foreign-prologue refusal shown RED on a synthetic prologue; the C5
lane rebuilt on it as the first consumer with the two-peer smoke + audits per the pre-handoff
checklist.

## 2. WP-2 — THE LISTENER SEAM (pass-2 table-turner; study §12.4/§12.6.1)

**What.** `ue_wrap/core/object_listeners`: engine-direct FUObjectArray create/delete listeners
with ZERO UE4SS imports — registration is a TArray append on the engine's own GUObjectArray
(+0x68 create / +0x78 delete, delete ops under the engine's own +0x88 CRITICAL_SECTION, which
UE4SS itself skips — we take it), listeners are plain 3-virtual classes (vtable slots proven in
VOTV's binary).

**Why it makes the mod better:**
- **Kills the dark-consumer `FindObjectByClass` class at the root** — canon census 136 call
  sites, ~52 world-scoped (`world_identity.h`), the mechanism behind the 2026-09-01 field bugs
  (skins / coin gun / empty-world joiner). A listener-maintained per-class live index replaces
  stale linear walks AND centralizes resolution so the world-stamp check lives in exactly one
  place. The index+stamp PAIR closes the class; the listener alone does not (world currency
  stays with `world_identity` — delete events arrive at GC purge, not at death).
- **Performance:** the remaining full-GUObjectArray walks (~1.1-1.6 ms each, `[WALK-TIME]`
  class) collapse into map lookups.
- **Closes `CachedObjRef`'s filed ABA-impostor residual** (delete events give the slot-death
  edge before recycling → per-index local generation).
- The prop lane's 20-second spawn-census fallback becomes event-driven.

**What it does NOT fix (recorded so nobody oversells it):** world currency (the stamp stays);
prompt death edges (`IsLiveByIndex` sees PendingKill within a tick; delete events arrive at
purge — `TickWatchedProps` stays).

**Constraints already charted:** create callbacks fire DURING UObjectBase construction (only
Class/Name/Outer/Flags/Index valid; possibly on the async loading thread) → enqueue-only,
thread-agnostic, drain on the pump (the `host_spawn_watcher` shape); delete callbacks run UNDER
the engine's +0x88 lock → map-ops only, never block, no engine calls; self-remove + latch in
`OnUObjectArrayShutdown` (the engine fatal-logs non-empty listener arrays at shutdown); register
once at boot, pre-reserve the create array (it has NO lock — element-before-Num ordering is the
only protection); grow with the engine allocator (the engine's dtor frees the array).

**Entry gate:** /qf to convergence (thread model, index shape, retirement order of the 52 sites,
interaction with `world_identity`/`cached_obj_ref`). **Acceptance:** the index shown consistent
against a forced GC + travel drill; a differential FPS/walk-time measurement before/after
retiring the hot sites; the 09-01 bug class's regression scenarios green.

## 3. WP-3 — INVENTORY NOW-INVARIANTS (study §8.5 tier 1)

**What.** Two cheap fail-loud invariants, no architecture change: **(a) the exit-ledger
conservation check** — every item leaving a hand/container/world slot must arrive somewhere the
lane knows, else the op is refused LOUDLY (wired into `container_contents_sync` +
`prop_drop_intent`); **(b) the equipment key-integrity refusal** — the equipment blob apply
cross-checks slot key vs payload. Plus verify `begin_equipment` reconciles idempotently on
rejoin rather than re-granting.

**Why better:** the #4 container-extract class becomes a build-time/loud failure instead of
[withdrawn at the author's request]
not reaching the ledger"). Item loss is the single most trust-destroying defect class a coop can
ship. **Entry gate:** short /qf. Independent of WP-4 (invariants stay valid under either base).

## 4. WP-4 — THE INVENTORY BASE DECISION (study §8.5 tier 2; foundation-first)

**What.** /qf-to-convergence on the designed direction: **host-canonical per-peer inventory
partitions inside the game's own `GObjStack` array** (the SP player is row 0; a per-peer row
routes per-player INSIDE the SP structure — arguably more principle-6 than our parallel
`coop_players/<guid>.json` sidecar). Client rows become a mirror; mutations become intents.

**Why better, if it converges:** the joiner's inventory rides the existing save transfer FOR
FREE (their partition is inside the host save the client already downloads); the pre-world blob
push shrinks to a rebind message; **A4's root dissolves** (the host validates intents into a
structure it owns instead of accepting client blobs); the JSON sidecar + 1 Hz poll retire per
RULE 2; the container CAS lane becomes ordinary intents against host rows. **Costs to price
honestly in the /qf:** inventory verbs become host round-trips (menu-shaped ops tolerate it; an
[withdrawn at the author's request]
identity stays OUR Ed25519-proved guid (their host-minted PlayerKey is the weaker half — not
imported). **This WP is a GATE:** container #4/#6 and the personal-container fail-closed hole
are NOT rebuilt on the half-canonical base before this decision lands (foundation-first).

## 5. WP-5 — DECLARATIVE QUIESCE (study §8.2)

**What.** Promote brain-parking from per-lane hand code to ONE `coop/element` primitive: a
per-class parked-state declaration {restorable field latches — bool/int/float/name/string/
clear-object ONLY (the restore-typing rule: *a latch that cannot restore what it took is worse
than one that refuses*), scheduler functions to gate/assert-dead, re-assert on the game flipping
them back, restore on authority arrival}.

**Why better:** every mirror lane hand-rolls this today (kerfur park, weather holds, laptop park
rows, the power design's decay watch) — one audited primitive replaces N bespoke parks, and the
WORLD_PROP_DIVERGENCE trap gets a paved road instead of a warning. **Consumed by:** the
power-chain /qf resume (R10) folds this in as its mechanism — that is where this WP gets built,
not as a standalone arc. Existing parks migrate lane-by-lane afterwards (RULE 2: each migration
retires its bespoke copy in the same commit).

**Naming constraint (verified 2026-09-02):** the tree ALREADY has a "quiescence" concept —
`coop/element/quiescence_drain.{h,cpp}` is the join-window drain-ORDER owner ("quiescence" there
= load-tail quiescence of the world-load episode), referenced across ~60 files. It is a
DIFFERENT concept from this WP's declarative brain-park primitive. The R10 /qf must pick a
non-colliding name for the new primitive (e.g. `element_park` / `brain_park`), per the
folder-per-concept naming rule — do not name it "quiesce" in code.

## 6. WP-6 — LEASE EXPIRY + REVISION FENCING (study §8.3)

**What.** `COOP_SYNCER_MODEL.md`'s arbiter design adopts, by name: idle-expiry returning
authority to the declared owner; epoch-stamped grants; **grant-names-final-revision fencing** —
a lease grant names the final revision of the previous writer's stream, and the new writer's
stream is not applied before that tail is. Cite MTA (the spirit) + Relay (the crispest
formulation) in the doc.

**Why better:** closes the handoff tail-race class by construction — the shape behind the ATV
lane's act-as-host residuals (a wheel spawned by the old authority arriving after the new
authority's stream). Design-doc amendment now; enforced as the arbiter lanes get built; the ATV
b2 resume reads it.

## 7. WP-7 — THE SMALLS (study §8.4 + §12.5/§12.6)

- **7a. Rebuild-breaker diagnostic:** detect our own local-kill fights — an actor rebuilt N
  times because LOCAL logic keeps destroying a non-owned copy — stop, and SAY "look for local
  logic running on a non-authority" in the log + feed. Also the near-term answer to the
  "дружить с модами" question: a foreign world-mutating mod fighting the sync becomes a NAMED
  log line instead of silent drift.
- **7b. Delta-drop chaos knob** (`debug_drop_delta_pct` shape) for our pose/heartbeat repair
  lanes — deliberately discard N% and prove repair works, in mp.py scenarios.
- **7c. Singleton-construction refusal** in `remote_prop_spawn`/mirror creation: refuse to build
  a second instance of a world-singleton class, with the reasoning in the log ("building a
  second one re-runs that class's BeginPlay against a world that is already up").
- **7d. G13-shape checklist** when the puppet gains subclasses: a generic inherited-entrypoint
  guard LIST (singleton/input/save/lifecycle) instead of discovering each per-site fix again.
- **7e. TURN `RelayOnly` session drill** (pass-2): our coturn fallback is deployed and
  cred-verified but has NEVER carried a proven session — force `IceEnable::RelayOnly`, run the
  two-peer smoke over the relayed path, make the fallback observable in logs. Drill debt, not a
  build.
- **7f. `multivoid1-` invite code** (pass-2): one checksummed pasteable string folding
[withdrawn at the author's request]
  shape) — the artifact our A2 design implied for DIRECT/LAN joins that today hand out a raw
  `gen:` line. Rides the A2 lane whenever it next opens.
- **7g. Argon2id note for C4:** recorded preference IF the lobby-password entropy floor ever
  gets its proper fix (libsodium is a heavier dep than our current path — decision stays open;
  do not build speculatively).

## 8. RECORDED-NOT-BUILT (deliberate parks — re-derive nothing here)

[withdrawn at the author's request]
  author surface = a pak of inert BP-callable stubs bound by name at runtime + version functions
  returning 0 when the native half is absent. Our WP-1 marker (arm A) is the first such stub;
  generalized to N stubs it IS the modder-facing surface the ROADMAP's platform phases
  ("engine-level, BP/table-shaped") will need. Build NOTHING now; when the platform phases open,
  start from this note + the absence-tolerant version-probe pattern + RLY-*-style stable
  diagnostic codes + rules-as-data merge semantics (refcounted identical rules, explicit-beats-
  profile, conflict = a stable code) — all measured precedents in the study.
- **8b. Named-service publish, not a bus:** if we ever expose in-process interop, the minimum
[withdrawn at the author's request]
  ecosystem uses exactly that and NOTHING else (its tag bus has zero consumers including its own
  BPs).
- **8c. Per-mod compat adapters stay demand-driven:** the DebugMod arc is our worked example and
[withdrawn at the author's request]
  convention with no replication rule — weaker, not a model). New adapters happen when a
  concrete mod + a concrete user need names itself.
- **8d. Relay coexistence posture:** mechanically fine (loader-level coexistence measured at the
  import layer), semantically unsupported to run BOTH multiplayer layers in one lobby — the
  standing world-mutating-mod class from the 2026-07-26 coexistence FACTS doc. A Relay-gated
  function must be refused by our WP-1 installer (foreign-prologue detection) — that is the one
  hard interop requirement, already in WP-1's constraints.

## 9. DECLINED WHOLE (the study's negative space — do not re-open by accident)

9a. **Migration to Relay** (any form, half-migrations included) — RULE 2/3 + the product facts
(study §0.5). 9b. **UE4SS imports** — re-adjudicated NO on the strongest base yet (§12.4:
181/187 already owned; dead on stable AND experimental; the class-rename death mechanism);
capabilities are adopted engine-direct instead (WP-2 is exactly that). 9c. **The tag bus** —
zero consumers even in its own ecosystem. 9d. **Voice/keybind/shop/calendar/dream registration
APIs** — a different product axis. 9e. **Host-minted player identity** (their PlayerKey) — ours
is client-proved Ed25519, stronger; not traded. 9f. **Stale-save transfer semantics** (publish
the on-disk file, lean on resync) — ours transfers torn-read-guarded CURRENT bytes; keep. 9g.
**Host-run grab with RTT feel** — their validation ladder is the enforcement precedent for our
movement ledger (that part is queued via A-findings), but the authority model stays holder-local
per MTA precedent + the host-may-cheat threat model.

---

## 10. Evidence discipline for this arc

Every WP follows the standing rules — they are restated here once so this doc is self-carrying:
/qf to convergence BEFORE implementation where the table says so; the pre-handoff checklist with
pasted evidence for anything deployed; post-ship audit agents on every shipped WP (the standing
exception to no-unasked-agents); every detector/gate shown RED before it is trusted; wire
changes bump `kProtocolVersion` in the same commit; new ReliableKinds walk the router checklist;
every touched lane keeps its mid-activity join row current (principle 8). Progress lands in this
doc's §0 table (status + commit + evidence pointer per WP), the way UE4SS_ARC tracks its WPs.
