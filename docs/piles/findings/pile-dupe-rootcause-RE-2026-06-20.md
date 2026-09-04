# Pile/Kerfur Dupe — Root-Cause RE & Direction Memo (2026-06-20)

> **`[corr 2026-09-04: docs census sweep]` DURABLE RECORD, SUPERSEDED IN OUTCOME.** Its measurements
> stand as of its date. `[V]` What it was written to inform has since been replaced: the pile mirror
> went PROXY -> NATIVE on 2026-06-30 (`docs/piles/11`, "nativization AS-BUILT + VERIFIED (hands-on)
> ... a rooted real `actorChipPile_C` native"), and the arc then ran on into crutch **C2** and the
> 2026-09-01 rejoin-crash fix. `[V]` Its file paths are pre-refactor -- `prop_adoption.cpp`,
> `pile_handle.cpp`, `pile_claim.cpp`, `pile_seed.cpp`, `prop_spawn_core.cpp` and `freeze_probe.cpp`
> are gone from the tree. Read it as evidence from June 2026, never as current design.

**Synthesis lead memo.** Decisive feasibility + direction call on the recurring ambient-pile (and kerfur) JOIN dupe, drawn from 6 structured RE/diagnosis findings (failure-log diagnosis, .sav-format RE, joiner-load-path RE, current-apparatus map, MTA precedent, kerfur RCA). Every load-bearing claim below is cited to a file:line, an SDK struct, or a real log line. Guesses are explicitly flagged.

---

## 1. The locked diagnosis (confirmed)

1. **The dupe is STRUCTURAL double-sourcing, not a transient bug.** On a join the joiner loads all ~870 piles natively from the shared captured save (correct, "nothing to reconcile" intent — `save_capture.h:14-16`) AND the coop layer ALSO streams those same piles as host-range mirror actors. Real client log: connect burst `[15:36:17-20]` = **870 "spawned NEW" actorChipPile_C + 513 "already aligned (d=0.00cm)"** — the 513 are save-loaded originals the stream matched onto, the 870 are fresh mirrors created beside them. Save-original + streamed mirror coexist = 2 piles.

2. **Piles are KEYLESS — there is NO stable cross-peer identity.** Host log `10948`: `870 keyless chipPile element(s)`; every re-stream OnSpawn logs `key=''` → `StringToFName('') → NAME_None`. The only identity a pile has is a host-minted host-range eid bound by world position, which ROTS when the join shadow-drain frees + recreates the save-loaded actors. This is why host grabs cannot address the joiner's copy → **"old piles don't morph."**

3. **The current thin-client "doom" CANNOT WORK and wedged in the real run.** Its §5 gate (`prop_adoption.cpp:762`) defers while `liveMirrors < expected`. The numerator (client-spawned-and-registered mirrors) and the denominator (host live-catalog `g_hostPileCatalog`, `prop_adoption.cpp:754-757`) are computed from **disjoint populations that never reconcile after a shadow-drain reshuffles actor identity**. Final real log line (client `25316`, `15:37:24`): `liveMirrors=758 < expected=868 … retry next tick`. **ZERO "PILE DOOM FIRING" lines anywhere** (the fire path is `prop_adoption.cpp:773`). The doom never fired → save-originals never destroyed → dupes persisted. (One caveat: session shut down ~21s into the re-stream; but the gate math gives no path to convergence, so "never fires" is the correct steady-state read, not a shutdown artifact.)

4. **A 37-second drain→re-stream gap is inherent to streaming piles the joiner already loaded.** Mass-purge first detected `15:36:24` (client `20280`); `PileResyncRequest + doom armed` not until `15:37:01` (client `20811`). Even a *working* doom leaves ~37s of visible duplicate piles every join. The re-stream then recreated **781 brand-new keyless actors** (only 3 "already aligned") — i.e. fresh keyless identity, so the SAME ambiguity recurs on the NEXT drain. This is the "patch LEVEL is wrong / architectural recurrence" signal.

5. **The doom VIOLATES the ADOPT-DON'T-DESTROY rule.** It is a destroy-and-recreate sweep (`DoomNonMirrorChipPiles_`, `prop_adoption.cpp:598-628`) gated on a cross-population count. The keyed divergence sweep at client `20315` explicitly destroyed ZERO local save piles ("0 unclaimed locals destroyed") — piles are routed to the separate pile-doom, which wedged. So the client's save-loaded piles were removed by neither mechanism.

6. **MTA has ZERO equivalent of this double-sourcing.** MTA's client has NO local map parser (`find Client -iname '*MapManager*'` → nothing); the server streams every entity once via `CEntityAddPacket` with a server-stamped `ElementID` (`CEntityAddPacket.cpp:103` writes `pElement->GetID()`; client READS it and never mints — `CPacketHandler.cpp:2860-2868, 3023-3025`). Exactly ONE authority-owned instance per entity, keyed by an id injected at load. Our dual-source pile + position-keyed mirror is the anti-pattern.

---

## 2. Candidate architectures

### A — SAVE-STRIP (write-time selective populate, NOT byte surgery)

**Verdict: FEASIBLE (low-risk mechanism), but architecturally INFERIOR.**

- **Strongest supporting fact:** an engine-native write-time opt-out already exists and is fully under our control. `actorChipPile_C` implements `Iint_save_C` (`actorChipPile.hpp:17-22,40,130,140`), which exposes `setIgnoreSave(bool)` / `ignoreSave(bool&)` (`int_save.hpp:6-13`). `saveObjects` skips any actor whose `ignoreSave()` returns true with NO array mutation (`votv-save-path-RE-2026-05-30.md:64-69`). A `setIgnoreSave(true)` sweep over piles inserted **before the `saveObjects` Call** (`save_capture.cpp:90`, verified the exact dispatch site) makes the game's OWN walk exclude piles — no GVAS parsing, no offset/count fixups, no checksum risk. (Byte surgery on the ~19MB opaque GVAS blob — two arrays `trashPilesData@0x818` + `primitivesData@0xE30`, the latter variable-length json, photos inlined — is RISKY and must be AVOIDED; we have zero GVAS parser today, `save_transfer.cpp:50,116-132,209-229` handles the blob as opaque CRC'd bytes.)
- **Biggest risk:** stripping piles makes the coop **stream the SOLE source of all ~870 piles as host-range mirrors**, re-loading the exact streaming+mirror+position-eid apparatus that is failing now — and host grabs STILL address each mirror by a host-minted eid that ROTS on shadow-drain (`project_session24_position_keyed_pile_2026-06-18.md:33-75`). A sidesteps the dupe but does **not** solve "no cross-peer identity"; it also keeps the 37s gap and the missing-pile failure mode (a stream gap loses piles — worse than a dupe).

### A' — KEEP-SAVE-PILES + SHARED DETERMINISTIC/INJECTED IDENTITY (no pile streaming)

**Verdict: FEASIBLE-WITH-CAVEATS (RISKY on identity determinism), and architecturally CORRECT.**

- **Strongest supporting fact:** both peers load the IDENTICAL blob, so the save record is a deterministic shared structure — `Fstruct_save{class@0x00, transform@0x10, key@0x40}` (`struct_save.hpp:4-21`), `loadData` restores the EXACT saved transform (`actorChipPile.hpp:19,21`). The host **retains `saveSlot.objectsData` in memory** (it just rebuilt it — `save_capture.cpp:97-106` verifies repopulate-not-append), so it can re-derive the same `{record → pile-actor}` map for its own live piles. A canonical save-record identity (re-resolved to the LIVE actor AFTER load-tail quiescence, re-runnable on every drain) defeats the s23-24 stale-pointer rot (which bound a transient pointer ONCE, pre-quiescence). This is strictly LESS machinery than A and far less than the failing thin-client stream — and it is the literal MTA shape (one authority-owned instance, addressed by an id injected at load), honoring ADOPT-DON'T-DESTROY (no doom at all).
- **Biggest risk:** the cross-peer identity is **UNSOLVED and its determinism is UNVERIFIED**. The existing key-mint is useless cross-peer (`prop_synth_key.cpp:66-68` mints `cs_<hostPtrLow32>_<counter>` from the host actor pointer). A' needs a NEW key both peers derive identically — either (a) save-enumeration index (`GetAllActorsWithInterface` order, NOT runtime-proven stable across save→reload), or (b) quantized position (collides if two piles share a cell / a pile moved between capture and load), or (c) host-injected via `setKey` before capture (still needs a deterministic seed to re-mint the SAME key per reconnect, and may change save ROUTING — see open questions). If determinism fails, the host grab morphs the WRONG joiner pile. **This is THE gating unknown and is currently a guess.**

---

## 3. RECOMMENDATION — A' (keep save piles + authority-stamped shared identity), no pile streaming

A' is the most root-cause, least-crutch, most-MTA-faithful option, and it generalizes cleanly to kerfurs. Justification:

- **RULE 1 (root-cause):** A' attacks the actual root — "piles have no cross-peer identity" — rather than papering over the symptom by force-deleting one of two sources (A) or count-gating a destroy sweep (current doom). It removes the double-source entirely: the save IS the single source.
- **ADOPT-DON'T-DESTROY:** A' has NO doom. The joiner's locally-loaded pile IS the one instance; the host just acquires a handle to it. This is exactly the rule, and it is already the SHIPPED, CORRECT shape for kerfurs (`npc_adoption.cpp:123-145` class+pose adopts the local twin AS the host mirror — adopt, never destroy; `kerfur_entity.h:43-131` is the KerfurId stable-identity layer). A' makes piles consistent with the kerfur architecture instead of inventing a parallel doom.
- **RULE 2 (delete the band-aid stack):** A' RETIRES the entire pile-streaming + doom + catalog + pending-remove + resync apparatus fully (no parallel old+new path). That is a large, correct simplification — not new risk.
- **MTA fidelity:** A' is MTA's invariant (one authority-owned instance, id injected at load, addressed by that id) — PROVIDED the id is authority-stamped/deterministic, NOT position-derived (position is the rotting key MTA never uses).
- **Failure history:** the recurrence is precisely because every prior fix kept double-sourcing + a fragile position/pointer key. A' is the only candidate that removes both at once.

**The one structural subtlety A' MUST solve (call it out, do not hand-wave):** the joiner's pile and the host's pile must agree on the same id. Because the host's live world may drift from its own point-in-time captured snapshot during the ~15s transfer (the blob-vs-live divergence the codebase already fights), the id must be **derivable on demand from the live actor's intrinsic, stable, shared property** (class + resting transform from the save record), so host-actor↔id and joiner-actor↔id agree without a frozen pointer table. Prefer on-demand derivation over a one-shot bound table.

**Kerfurs:** do NOT apply A (strip) to kerfurs — it would REGRESS the camera-safe local-twin adoption (`npc_adoption.cpp:64-67`; fresh-spawn reintroduces the v74 floating-camera bug). The kerfur dupe is a DISTINCT staleness/form-mismatch race (host streams ON-NPC, joiner's twin loaded OFF-prop → exact-UClass match at `npc_adoption.cpp:127` fails → fresh-spawn a 2nd actor at `:146-165` beside the surviving OFF twin). The A'-consistent kerfur fix: adopt the local twin **regardless of current local form** (match on host EntitySpawn eid/pose, NOT the save Key — `kerfur_entity.h:7-8` says `loadData` MINTS A RANDOM KEY PER PEER, so Key is NOT a cross-peer match key), then drive the existing host-authoritative `KerfurConvert` to flip the adopted twin to the host's form. Plus harden/remove the stale on-disk fallback (`save_transfer.cpp:361-364`, RULE 2 — documented as the original kerfur-dupe cause).

**Net:** A' for piles (authority-stamped, on-demand-derived shared identity) + the form-agnostic adopt-then-convert fix for kerfurs. Both are the same architecture — single source = the shared save twin, rebound to host identity, never destroyed.

---

## 4. RULE-2 DELETES vs KEPT (under A')

**DELETE (fully — no parallel path):**

| Component | Site |
|---|---|
| P1 client-mint gate — chipPile half REVERSED (client MUST mint a shared id for its save pile); kerfur half stays | `prop_element_tracker.cpp:336-346` |
| `g_hostPileCatalog` + `CataloguePileMirror` / `ForgetPileCatalogEntry` | `prop_adoption.cpp:92,557,563` |
| PILE DOOM — `DoomNonMirrorChipPiles_`, `TickPileReconcile`, `ArmPileDoom`/`ArmPileDoomForConnect`, the §5 `liveMirrors<expected` gate | `prop_adoption.cpp:598-782` |
| `g_pendingRemoveEids` + `EnqueuePendingRemove` / `ConsumePendingRemove` | `prop_adoption.cpp:97,568,585` |
| `PileResyncRequest`/`PileResyncComplete` + `EnqueuePileReStream` / `DrainPileReStreamChunk` | `protocol.h:1762-1771`; `prop_snapshot.cpp:564-668` |
| OnSpawn pile-catalog + pending-remove block | `remote_prop_spawn.cpp:797-818` |
| OnDestroy pending-remove ordering | `remote_prop.cpp:920-949` |
| net_pump drain-edge `PileResyncRequest + ArmPileDoom`; connect `ArmPileDoomForConnect` | `net_pump.cpp:525,558`; `event_feed.cpp:414` |
| host pile re-stream handlers | `event_dispatch_state.cpp:749-783` |
| SeedWalk_ keyless-pile host-eid mint (replaced by the canonical-identity mint) | `prop_element_tracker.cpp:585-604` |
| stale on-disk save-transfer fallback (kerfur dupe source) | `save_transfer.cpp:361-364` |

**KEEP:**

- The native save-transfer + native `loadObjects` path (the proven single-source loader — `save_capture.cpp:35-145` untouched; A' adds an identity layer, not a load change).
- The `pile_handle.cpp` grab/throw RELAY **shape** (host-authoritative spawn-clump-and-broadcast is correct) — but REPOINT its identity resolver from `ResolveMirrorEidByActor` / host-range-eid (`pile_handle.cpp:216-252`, `RelayClientGrab:456`) to the new canonical shared-save id.
- The mass-purge drain-edge DETECTION (`net_pump.cpp:498`, `keyedReaped>0` co-condition) — still distinguishes a real shadow-drain from interaction churn; A' RE-RESOLVES pile identity on this edge instead of re-streaming mirrors.
- The kerfur adoption + KerfurConvert machinery (`npc_adoption.cpp`, `kerfur_convert.cpp`, `kerfur_entity.h`) — EXTEND to be form-agnostic, do not delete.
- The keyed divergence sweep — but its quiescence counter `CountLoadTailUnsettled_` (`prop_adoption.cpp:414-449`) is chipPile-inclusive; deleting the pile doom must KEEP pile-inclusion for the keyed sweep (or re-derive a pile-aware quiescence) or the keyed sweep may fire before the pile load tail settles.

---

## 5. OPEN RE QUESTIONS (must be answered before implementation — currently guessed)

1. **[DECISIVE] Determinism of the canonical pile identity.** Does `GetAllActorsWithInterface(int_objects_C)` return chipPiles in byte-identical relative order on the host's `saveObjects` capture-walk vs the joiner's post-quiescence live world? Asserted from UE4 GUObjectArray index-order semantics but NOT runtime-proven. **Probe:** log each live chipPile's matched `objectsData` index + transform on BOTH peers and diff. If identical → save-index is the id. If not → fall back to position+class with an epsilon tie-break for co-located piles (collision rate must be measured over the real ~870).

2. **Position/transform stability as the fallback key.** If enumeration order is non-deterministic, is quantized resting-transform collision-free across the real ~870 piles, and stable between capture and load? Unmeasured. Two piles within float-epsilon, or a pile that moved during the transfer window, break it.

3. **Does `setKey` (injection variant) change save ROUTING for a previously-keyless pile?** Keyless piles may route to `trashPilesData`/`primitivesData` BECAUSE `gatherDataFromKey` returns `gather=false` for no-key; adding a key might move them into `objectsData` and alter load behavior. Verify `gatherDataFromKey`'s gather-bool semantics for `actorChipPile_C` (`actorChipPile.hpp:19`) before choosing injection.

4. **Host re-derivation correctness across blob-vs-live drift.** Can the host bind ITS OWN live pile actors to the same per-record id it ships, given its live world may diverge from the captured snapshot during the ~15s transfer? Prefer on-demand derivation (class + resting transform) over a frozen `objectsData`-index table; quantify the drift at the connect edge. `Registry::SnapshotActorsByType` orders by ElementId (adoption order), NOT save-array order (`registry.cpp:264-275`) — so the host has no native save-index on a live actor today; this is new code to verify.

5. **`ignoreSave()` backing-field semantics (only if A's strip is ever pursued as fallback).** Does `actorChipPile_C.ignoreSave()` actually return the bool `setIgnoreSave()` writes (backing field, no subclass override)? Flagged tier-3 in `votv-save-path-RE-2026-05-30.md:70-72`. Needs a live probe.

6. **Kerfur EntitySpawn vs blob-capture ordering.** Does `RegisterExistingWorldNpcs(ConnectEdge)` (`subsystems.cpp:197`) run BEFORE or AFTER `save_transfer::OnRequest` blob capture (`save_transfer.cpp:328`) for the same joiner? If the host can toggle a kerfur ON between these two events, the blob (OFF) and the EntitySpawn (ON) disagree within a SINGLE join → guaranteed form-mismatch dupe even at zero latency. **Probe:** grep the host log for the relative order of "streaming LIVE host world" vs "npc-sync[world-enum]: registered" vs the kerfur EntitySpawn for the joining slot.

7. **GVAS byte-format (only if byte-surgery A is ever pursued — NOT recommended).** No `.sav` exists on disk in the repo to confirm the container is uncompressed/unchecksummed and pile records in `trashPilesData`/`primitivesData` are cleanly identifiable. Inferred from stock UE4.27 GVAS, not byte-verified this session.

8. **Observability gap.** No single "client pile census: save-origins=N mirrors=M" log line exists; the dupe count is INFERRED (~870 save-originals + 758 mirrors), not directly observed. Add a periodic census line so the fix is verifiable hands-on.

---

*Citations throughout are to the source tree (`src/votv-coop/...`), the SDK reflection dump (`CXXHeaderDump/*.hpp`), the MTA precedent (`reference/mtasa-blue/...`), the real join logs (`Game_0.9.0n[_copy]/.../votv-coop.log`), and prior findings under `research/findings/`. Items flagged "guess"/"UNVERIFIED"/"asserted but not runtime-proven" are exactly the open questions in §5 — do not treat them as settled before the listed probes run.*
