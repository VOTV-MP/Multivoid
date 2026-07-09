# email_sync authority-direction audit — peer-symmetric vs host-authoritative (DESIGN, 2026-07-09)

**Trigger:** user — "email_sync is probably breaking project rules, should it be asymmetric? It needs to be
mirrored FROM host, not the other way around. Probably need to check other files too for breaking same rules."
**Method:** `/qf` QUESTION+DESIGN pass, 8 rounds to convergence ("that holds") + a full authority-direction
survey of all 24 `world/` + `interactables/` sync modules. **Status: DESIGN — no build this pass.**

## TL;DR (the reversal)
The user's instinct is CORRECT: `email_sync` peer-symmetric APPEND is a real anomaly — email is host-owned
world state, and every other world sync is host-authoritative. **But the RULE-1 root fix is NOT a
distribution-layer host-auth gate on `EmailAppend`.** That gate is a **principle-4 "broad filter of orphans"
crutch at the wrong layer**, and it's incomplete. The **root fix is the project's own established
mirror-pattern step 3**: source-kill the client's autonomous email-authoring world-sims (as `serverbox_sync`
already does for `ticker_serverBreaker`). **`email_sync` itself needs no change.**

## Why the append-gate is a crutch (the decisive chain)
1. **Census (`docs/notifications/TOAST_CATALOG.md`):** all 26 `addEmail` sites are world/story/system-authored;
   **zero** are client player-actions. So a client's entire email-author set = **{false emails from diverged
   world-sims}**.
2. Therefore a `role()==Host` gate on the client's email broadcast is **indistinguishable from "filter the
   client's false emails"** — it suppresses exactly the source bug's symptom. Principle 4 forbids "a broad
   filter of our orphans"; RULE-1 forbids symptom-patching.
3. **It's also incomplete** (`email_sync.cpp:206`, `CompleteAssembly`): `AddEmail` is called
   **unconditionally** — no existence-dedup. `g_applied` only echo-proofs (marks the local *shadow* row
   sent); it does not gate `AddEmail` on an existing local content match. So a host+client **co-authored**
   email **duplicates** (client keeps its own copy + the host's = 2 rows). The append-gate does NOT fix this
   (the client still locally authors + receives). **Only source-kill collapses it** — same root, second face.

## The root fix
The established mirror pattern (`memory/lesson_votv_world_system_sync_mirror_state_not_verb`) **step 3**: the
client **neutralizes its own autonomous world-sim mutators**. Precedents: `serverbox_sync` disables
`ticker_serverBreaker` tick; `event_fire_sync` zeroes `allEvents.Num`; weather suppresses the local scheduler.
When the email-authoring sim is source-killed on the client, the client **neither authors nor forwards** the
email → both the SHARED pollution and the co-author DUPLICATION dissolve at the root. `EmailDelete` stays a
symmetric content-hash CRDT (authoring = a world-sim event, host-owned; deletion = a player curating a read
inbox — coherent to have different authority).

## Cross-file survey — authority direction of all 24 world+interactables syncs
The genuine divergence-bug class of "world-owned state broadcast peer-symmetrically" is **N=1 (email append
alone)**. Everything else is host-auth, host-mediated, mitigated, or symmetric-by-design:
- **Already host-authoritative (A):** weather_sync, time_sync, sky_sync, weather_lightning, weather_redsky,
  weather_fog, balance_sync, event_fire_sync, event_cue_sync, event_active_sync, turbine_sync, drone_sync,
  serverbox_sync, garbage_sync.
- **Host-mediated (C/D — client sends only to host, host re-authors/relays):** alarm_sync, device_occupancy,
  signal_catch_sync, interactable_sync's door leg, atv_sync (occupant-authoritative).
- **PARTIAL peer-symmetric but mitigated:** keypad_sync (apply-side host-auth on the power/door axis; digit
  buffer intentionally bidirectional), interactable_sync's 5 Symmetric channels (no-auto-revert → poll can't
  oscillate).
- **Peer-symmetric but CONVERGENT-BY-DESIGN (not bugs):** window_sync + grime_sync (min-wins monotone),
  signal_sync (content-hash CRDT), firefly_sync (cosmetic ambient), comp_sync (single-simulator split-brain
  guard), console_state_sync desk/dish/log substreams (device-claim-owner).
- **`power_sync` ADJUDICATED CLEAR** (the survey's flagged "email twin"): its 5 bits are **physical
  player-hand breakers** (`session_lanes.h:135` marks `PowerControlState` *deliberately* symmetric-relay;
  `ApplyMask` idempotent at `power_sync.cpp:129/141`). A host-only gate would **regress the client's breaker
  pull.** Not a bug.

**So "check other files" yields NO new required change** beyond the per-sim source-kill roadmap.

## NAMED-OPEN (measurements — nothing built blind)
1. **THE gating measurement:** enumerate the 26 `addEmail` call sites → each site's authoring world-sim +
   whether it still fires on a client TODAY (post-`serverbox` source-kill). Drives the source-kill roadmap.
   A client-triggered / host-wouldn't-author *shared* email would falsify "source-kill sufficient" and force
   a **targeted client→host email intent** for that one site (not a blanket gate).
2. delete-verb census (who calls the game's email-delete; any client *sim* deleter?) + delete shared-vs-per-
   player semantics — the same measure-before-gate discipline; left untouched this pass.
3. **Probe before any build:** has live email pollution been observed at all? `serverbox_sync` likely already
   source-killed the only known case (the false "SERVER X down" email). PROBE-DON'T-GUESS.
4. Game-side `laptop.addEmail` dedup behavior — unmeasured (our code adds unconditionally).
5. Pre-existing, orthogonal: half-(ii) local false-email display; the join-window save-snapshot→`connected()`
   gap for a host email authored mid-handshake.

## USER PRODUCT DECISION (2026-07-09, post-audit) — resolves the open axes
User: *"I want emails only from the host game instance, and clients only see the host's emails, zero of their
own. And clients can delete emails, which means they delete the host's email, and it's all good."*

This is a clean product invariant that closes two named-open items:
- **APPEND: host-only. The client inbox is a PURE MIRROR of the host's — the client authors/displays ZERO of
  its own.** This makes "client authors zero" a REQUIREMENT, not the deferred half-(ii). It does NOT resurrect
  the append-SEND gate (still a crutch/belt-and-suspenders — a client that authors zero has nothing to send);
  the real work is ENFORCING client-authors-zero.
- **DELETE: symmetric / shared — CONFIRMED.** A client delete removes the host's email from the shared inbox
  for everyone, and that is intended. So `EmailDelete` stays as-is; the delete-verb census is no longer a gate
  (shared removal is the desired semantic). The append(host) / delete(shared) split is the user's explicit model.

### The mechanism wrinkle for "client authors zero" (implementation fork — needs the impl /qf)
`addEmail` (`mainGamemode.addEmail -> laptop.addEmail`) dispatches **`EX_LocalVirtualFunction` -> INVISIBLE to
both our ProcessEvent detour AND the Func-patch** (notifications RE). So we CANNOT hook "client addEmail =
no-op" at the sink. The two proven-shaped enforcement options:
- **(A) Per-sim source-kill** (mirror step 3, serverbox pattern): neutralize each client world-sim that
  authors email so nothing CALLS addEmail. Proven, but piecemeal (26-site roadmap) and can be over-broad if a
  sim must run on the client for another reason while only its email side-effect should be suppressed.
- **(B) Host-authoritative inbox RECONCILE:** host sends its authoritative email set; the client's array is
  reconciled to it (apply host rows; a client-authored row absent from the host set is stripped). The DIRECT
  expression of "pure mirror" — not a per-instance false-email filter but a whole-inbox authority reconcile
  (shape precedent: the join-window position/prop reconciles). Consistent with shared delete (a client delete
  propagates to the host first, so the host set no longer holds it and the reconcile keeps it removed).
- **The impl /qf must pick A vs B** (or A-for-now + B-as-general), measure addEmail's client-liveness per site,
  and confirm the host-forward + shared-delete interplay under each.

## IMPLEMENTATION /qf (7 rounds, converged 2026-07-09) — build-ready plan
The impl pass de-braided the mirror into **two independent holes** and produced a one-line build-ready increment:

- **HOLE 1 = SHARED pollution** — the client's 1 Hz poll BROADCASTS a locally-authored false email to the HOST
  (permanent shared-inbox row). **FIX = INCREMENT 1 (build now, one line):** host-gate the append send —
  `email_sync.cpp:392` `if (s->connected())` -> `if (s->connected() && s->role()==Role::Host)`. This is the
  authority-direction model (email host-owned; the client is not a distribution authority), parity with
  weather/time/serverbox — **NOT a crutch** (the design-pass "send-gate is a filter" verdict was WRONG: the
  same logic would condemn weather's host-gate). Also closes the startup transient before serverbox's
  source-kill latches. **MEASURED-SAFE:** g_applied + prime + shadow + diff ALL stay (load-bearing for the
  client mirror's apply + echo-proof — `:167` gates `ApplyDeleteByHash` on `g_primed`, `:175` erases shadow,
  `:211/:348` echo-proof appends); only the `SendRow` at `:392` is gated out. role() is immutable per running
  session (`session_start.cpp:98` sets `cfg_` once, no setter; rehost = a fresh Start while not running). The
  delete send **stays symmetric** (client USER-deletes must propagate; echo-proofed by the synchronous
  `g_shadow` erase at `:175`). **Acceptance signal:** join+play log shows the HOST inbox receives ZERO
  client-authored `EmailAppend`.
- **HOLE 2 = LOCAL display** — the client SEES its own false email locally. Already closed for the ONE known
  case (server-down) by `serverbox_sync`'s source-kill (log-pending). **FIX-A** = per-sim source-kill
  (zero-flash, restores the visual via the host mirror) for future visual-critical sims. **FIX-B** = client
  strips locally-authored (non-wire) rows at the poll's local-append detection point — a **LOCAL-ONLY removal
  EXCLUDED from the `removed`/`EmailDelete` path** (the "fourth hole": else the client authors a wire delete
  for a never-shared row), ordering **apply-inbound -> strip-local -> emit-user-delete**, ~1s flash. Build B
  **only** when the hands-on observes an uncaught leaker OR the user weights the hard guarantee over the flash.

**DELETE:** user deletes symmetric/shared (the user blessed this); a client *sim* delete would pollute ->
delete-verb census (does any client sim call DelEmail?) — unmeasured, flagged, lower-risk.

**Why not A-incremental alone:** it can never assert "guaranteed zero" (only "zero of the N sims enumerated");
the user's *guarantee* + the one-owner principle put the completeness owner at the email layer (INCREMENT 1 +
FIX-B), not N scattered source-kills. **Why not zero-flash B-structural now:** email_sync's only client-side
observation point is the 1 Hz poll (after-the-write); a native zero-flash chokepoint inside `addEmail` is
unmeasured (optional `laptop.addEmail` disasm) and only matters if a visual-critical leaker needs it and A
can't reach it.

**Measurements before any 2nd build:** (1) static enumeration of every `addEmail`/`makeEmail`/`DelEmail` BP
call site -> client-reachable vs host-only (doable now, off-PC — narrows both the leaker and sim-delete
classes); (2) the join+play hands-on (host inbox clean = Hole 1; client inbox clean + serverbox sever = Hole
2); (3) optional `laptop.addEmail` disasm.

## Impact on the notifications Inc-2 direction
This **supersedes** the notifications Inc-2 note that assumed "forward = reuse email_sync (symmetric)" as the
free half. The correct framing: email needs no forward-gate; the client's false-email authoring is removed by
source-killing its world-sims (Inc-2's real work), which `serverbox_sync` began. See
`research/findings/votv-notifications-suppress-mirror-DESIGN-2026-07-09.md`.
