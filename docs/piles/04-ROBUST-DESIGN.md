# Robust, native-looking pile sync — the target design

> The design we are restoring to. Synthesized 2026-06-20 (session 32) from the full
> `docs/piles` knowledge base (the June-10 working-era RE, the morph round-trip RE, the
> bytecode facts, and the failure history). This is the "what good looks like" doc; the
> mechanical steps are in [03-RESTORATION-PLAN.md](03-RESTORATION-PLAN.md).
>
> **Still valid (2026-06-21) — its principles are realized by [08-HOST-AUTH-TRASH-CHANNEL.md](08-HOST-AUTH-TRASH-CHANNEL.md).**
> Principle 5 here ("client interactions relay, never author — the host does it + broadcasts; every peer
> mirrors") is exactly what the morph (07) VIOLATED (the client morphed locally → the dead direction + the
> dupe) and what 08 RESTORES (client suppress-native + `GrabIntent` → host executes). Read 08 for the
> concrete host-authoritative realization.

## The fact that reframes everything

**Piles are placed DETERMINISTICALLY.** `garbagePileSpawner_C` is a one-shot, **seeded**
RandomStream over map-baked arrays → every machine loading the same map spawns piles at the
**same positions** (`votv-snapshot-adoption-root-causes-2026-06-10.md` RC3; the RNG theory was
explicitly falsified). Cross-peer pile divergence is **host STATE DRIFT** (collected / converted
/ moved piles), not randomness. And with save-transfer the client loads the host's *current*
save → its piles sit at the host's *current* positions.

⇒ **Position is a reliable binding key for a pile at join time.** The entire recent detour
exists because we treated piles as having no usable identity and invented machinery to
compensate — when position (made exact by the deterministic spawner + save-transfer) was always
a good key. The host-minted eid is the durable identity; position is just how we (re)attach it.

## What we were doing wrong (the anti-pattern cascade)

Root mistake: **we stopped ADOPTING the client's own pile and started destroying it +
fresh-spawning a mirror + "dooming" the original.** Consequences:

1. **Destroy-and-recreate instead of adopt** → two actors where there should be one (the dupe).
   Violates the project's "adopt the local result, never destroy-and-recreate" rule. The working
   scheme bound the client's own pile onto the host eid (`RegisterPropMirror(eid, ownPile)`).
2. **Count-gated cleanup that can't converge** — the doom needs `liveMirrors >= expected` but the
   two counts are disjoint populations that diverge after the join shadow-drain → never fires
   (`758 < 868`, forever).
3. **A trigger gated on an edge that never fires** — the resync waits for the purge-episode EXIT,
   which never happens under churn → freed mirrors never restored.
4. **One-shot binding that rots** — the adopt was made once at the bracket; the shadow-drain
   frees+recreates the piles → the bind dies, and we threw adopt away instead of re-running it.
5. **Stripping the save piles** — removed the actor the adopt binds onto → client had no piles.
6. **Inventing identity machinery** (PileSeed, g_hostPileCatalog, pending-remove, doom,
   PileResync, strip) to fight keylessness — each layer adding fragility — when position-adopt
   already worked.

## What robust, native-looking pile sync IS

**Principle: one real game actor per pile — the client's own loaded `actorChipPile_C` — adopted
onto the host's eid, its state driven by the host, its interactions relayed to the host.** It
looks native because it IS the native actor running native BP behavior; we only re-point its
identity and drive its state. Five pillars:

1. **Identity = host-minted eid; binding = position-adopt.** Host mints one stable eid per pile
   and streams it eid-only. Client binds its OWN nearest loaded pile (≤30 cm + same chipType +
   same class) onto that eid — no second actor. (HEAD `EnsurePileBindIndex`.)
2. **Re-run the adopt after the shadow-drain** — the one robustness gap the old scheme lacked.
   When the drain frees+recreates the client's piles (same positions), re-enumerate and re-bind
   onto the still-stable host eids. This REPLACES the doom/catalog/pending-remove/resync
   apparatus entirely: nothing to re-stream (client still has its piles), nothing to doom (the
   original IS the mirror).
3. **Host-authoritative morph, mirrored natively.** Grab → real BP `toClump()` (pile→clump),
   throw → `turnToPile()` (clump→pile). Three distinct UObjects, replicated by the Init-POST +
   destroy observers + a held-attach to the puppet hand. Host owns it; clients run the SAME
   native BP, so mesh/physics/variant (chipType + pile/clump TSubclassOf) are correct by
   construction. (`votv-chippile-clump-morph-RE-2026-05-27.md`.)
4. **Host-authoritative removal.** Death-watch broadcasts `PropDestroy(eid)` for piles the host
   collects/destroys; the quiescence-gated divergence sweep removes the client's orphan piles the
   host no longer has, guarded by the >50% world-wipe valve.
5. **Client interactions relay, never author.** Client E-press relays to the host; the host does
   the morph and broadcasts; every peer (incl. the presser) mirrors. Kills the E-spam local-clump
   dupe — the client never morphs its own pile, so there is no unsuppressed local morph.

### "Native-looking" polish (already RE'd, was working — keep it)
- Rest physics correctly: stamp `kSimulatePhysics` only when the root body is POSITIVELY awake,
  not from the save sleep flag (fixes piles/walls falling from the air on join).
- Correct mesh/variant: chipType + `propName` parity (fixes the white-cube identity theft).
- The morph is the real BP round-trip (round clump in hand → flat pile of the original variant on
  landing), driven host-authoritatively.
- No dupes/pops: a single adopted actor per pile.

## The single hard problem and its honest answer

The keyless identity DOES rot through the join shadow-drain. **Solve it by re-running the
position-adopt after the drain — not with more machinery.** The host eids are stable; the
client's re-created piles are at the same positions; re-match. Everything heavier than that
(seeds, dooms, resync, strip) is the over-engineering that has duped for 10 sessions.

## Restoration target (what the code should look like after)

- The adopt-by-position reconcile from HEAD `1272b0a3` (`EnsurePileBindIndex` in
  `remote_prop_spawn.cpp`), restored.
- The s23–s31 thin-client churn DELETED (prop_adoption.cpp, pile_handle, PileResync, the doom,
  the catalog, pending-remove, the P1 chipPile mint-gate).
- A NEW, small **re-run-the-adopt-after-drain** edge (the only new code).
- The committed death-watch (PART 1) + client host-authority gate (PART 2) kept.
- Non-pile uncommitted work (kerfur, email, events, harness smoke, HUD) preserved.

## Open verification item for the restore
Confirm the June working scheme used the **stale-save** path; determine whether the later
**live-capture** switch (done for kerfurs) destabilizes the pile adopt and must be off for piles,
or whether the adopt works with either save source. (`votv-livesave-transfer-RE-AND-ASBUILT`.)
