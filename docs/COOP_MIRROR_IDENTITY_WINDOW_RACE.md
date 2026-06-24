# Mirror-identity JOIN-WINDOW race — a PROBLEM CLASS (not one object's bug)

**Recognized 2026-06-24 (user architectural instinct, confirmed across 2 instances: piles + kerfur).**
This is a cross-cutting class doc. The fix discipline: **make each instance WORK first, generalize into a
shared mirror-identity layer AFTER N>=3** -- do NOT abstract on N=2 (see "Why not generalize now" below).

## The class

Any entity that reaches a joining client via **TWO channels at once** -- (a) the transferred SAVE the client
loads, and (b) the host's connect-replay BROADCAST -- AND lacks a **stable cross-peer identity key** to tie
the two together, exhibits the SAME three symptoms when the host mutates it DURING the join window
[save taken ... client load-100%]:

1. **WINDOW DUP** -- the save-channel delivers one state, the broadcast delivers another; with no exact key
   to reconcile them, the client keeps BOTH.
2. **MATERIALIZE-FAIL variants** -- the broadcast form may partially spawn (e.g. the kerfur fresh-spawn
   floating-camera) because the in-window adopt path can't run (the save twin is the wrong form / not ready).
3. **IDENTITY-COLLISION** -- with no exact key, binding falls to POSITION-FUZZY (nearest same-class within a
   window); in a cluster the WRONG instance claims another's binding -> an orphan that never re-binds.

**The cure (proven on piles, L1 Path 1c):** a **save-time EXACT-position key** -- both peers derive the same
identity from the one transferred save -> exact match (no fuzzy), retried at post-quiescence (the load-tail
race), with a >50% valve. Exact position is unique per instance -> kills the dup (1) AND the cluster
collision (3). (Materialize-fail (2) is a separate, per-entity spawn-path concern.)

## Instances (N=2)

| | piles (`actorChipPile_C`) | kerfur (`kerfurOmega_C` <-> `prop_kerfurOmega_C`) |
|---|---|---|
| cross-peer key | KEYLESS -> save-time position | random per-peer `Aprop_Key` + synthetic `coopkerfur#eid` -> save-time position |
| form across convert | stays one class (eid stable) | **CHANGES class** active<->object (eid not stable; KerfurId bridges) |
| secondary tie-break | `chipType` | none (pure position) |
| reconcile | `pile_reconcile::SweepReconcileSaveTimeTwins` (BUILT, L1) | `kerfur_reconcile` (DESIGN `docs/kerfur/03`, not built) |
| status | FIXED + hands-on-verified 2026-06-24 | diagnosed + designed; not built |

## Shared seam (what a future layer extracts) vs per-entity (what stays)

- **SHARED (extract later):** blob-instant save-time position capture (class-predicate param), `matchX/Y/Z` on
  the broadcast, the world-ready twin-destroy + post-quiescence sweep + >50% valve, the position-uniqueness
  anti-collision. `pile_reconcile`'s sweep/valve is the strongest reuse candidate -- parameterize the class
  predicate + the secondary tie-break field.
- **PER-ENTITY (stays):** the form/class model (single-class pile vs class-changing kerfur via KerfurId), the
  key shape (keyless / Aprop_Key / synthetic), and the materialize path (symptom-2 variants).

## Why not generalize NOW (rule of three)

Two instances already DIVERGE (kerfur changes class, pile doesn't; kerfur has a BP-key + a synthetic key, pile
is keyless; pile has a chipType tie-break, kerfur has none). An abstraction built on N=2 risks encoding the
wrong axes and not fitting the 3rd mirror object. So: **build kerfur_reconcile reusing pile_reconcile's
mechanism PARAMETERIZED where it fits cleanly; record where it does NOT fit (the class-survival seam) as the
signal; extract the shared layer when a 3rd instance arrives** with three real divergence sets to abstract
over. Make it work -> make it right -> make it reusable, in that order.

## Watch-list (future mirror objects likely in this class)
Any save-persisted, host-mutable, broadcast-mirrored entity without a stable cross-peer key: other buyable
props with state, placeable devices, future NPC variants that save. When one appears with these 3 symptoms in
the join window, it is the 3rd instance -> time to extract the shared layer.
