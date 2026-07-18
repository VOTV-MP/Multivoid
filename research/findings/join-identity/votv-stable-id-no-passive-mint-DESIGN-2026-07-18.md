# Stable-ID root fix: no passive client mint for keyed props + host authority gate

**Status: AS-BUILT 2026-07-18 s22 (NOT hands-on).** /qf "that holds" R8/15; user rule-1 green light.
Thread: scratchpad `qf_thread_stableid.md`. Proto 121 unchanged; DLL `06b9e2d23c84037f` x4
hash-verified.

**BUILD OUTCOME (section 5 acceptance — MET, two reconnect cycles, the second on the final DLL):**
1. Rack under ONE host-band eid=6102 on the reconnect-with-rack-in-save join (baseline: 45223+6103
   double); drive_rack set/take ops SINGLE (baseline doubled); both frozen digest circles cross-peer
   (client 2f57cabc / host c4b0a701 / empty cea1940d — s21b instrument values exactly).
2. Sweep universe 2236 -> 1 (the surviving 1 = the per-player inventory container, per-player-skipped
   as before); adopt count 2232-2233 vs 2236 baseline (run variance, adoption itself unchanged);
   keyed index-half + claim counters live.
3. ZERO A'-dissolve / A'-wall / H-handback / peer-reject / 1:1-reject logs across both clean cycles
   (every guard is LOUD by design). NOTE: the dissolve/handback conflict flows are therefore NOT
   dynamically exercised (reachable via express-if-unknown straggler + container-extract conflict
   only); mechanics reuse the proven UnmarkKnownKeyedProp drain shape. The baseline's doomed-1
   trashBitsPile did not reoccur (run variance) — the keyed doom path is counter-live but likewise
   not exercised this run.
4. Health A/B: every warn class byte-count-identical to baseline (81x KEY-UNIQUENESS host, BIND
   SUMMARY identical); perf netPumpTick 0.16-0.19 ms/fr; no crash/hang/OOM (the "crash" greps were
   GUID-substring false positives, counts identical to baseline).
Audits: perf 0 CRITICAL / 0 WARN (function table; census-branch cost = one leaf-mutex hash write on
an already-paid key read; sweep cold-path); correctness 0 CRITICAL, IMPORTANT-1 FIXED in-session
(dead element-less key-index entries had no reaper -> DrainDeadKeyIndexEntries at the post-purge
re-seed edge + ReconcileIndexThrottled + the sweep dead-branch evict), MINOR-2 addressed (kerfur-walk
label comment). Build-tooling note: the first build's link error was eaten by a grep+tail filter —
caught by the mandatory build-vs-deployed hash compare; never trust filtered build output.
Supersedes the s21b hint "dissolve the provisional at adopt-by-key" (the dissolve is one branch of
the design, not the fix). Extends `docs/COOP_STABLE_ID_SIDECAR.md` (this IS the stable-ID thread's
join-window root for KEYED props; the sidecar covered the KEYLESS families).

## 1. The measured disease (all [V], citations from this pass)

One actor under TWO element rows, systematically, on every client join:

- `rackB_reconnect_CLIENT_phase2.log`: re-seed minted **2356** keyed client locals at 18:45:10,
  `ClientWorldReady` the same second, then **2236** `resolves to live actor` keyed adopts at
  18:45:11 -- each stacking a host-eid MIRROR row over the just-minted client-band LOCAL row.
  The s21b rack (45223 local + 6103 mirror, both emitting ops) was one instrumented instance of
  ~2200 per join.
- Local-first is STRUCTURAL: net_pump re-seeds, then announces world-ready, then the snapshot
  arrives (net_pump.cpp:662-681). MarkPropElement's reverse early-out (:335) protects only the
  mirror-first order.
- The KEYLESS pile adopt path retires the client-local identity (remote_prop_spawn.cpp:437, audit
  CRITICAL-1 2026-06-10); the KEYED path (:398) never did.
- `CreateOrAdoptPropMirror` consults only `Registry.Get(eid)`, never `EidForActor(actor)`;
  `Registry::RegisterMirror` OVERWRITES the reverse (registry.cpp:222). Consequences: the zombie
  local persists as long as the actor lives (the reaper evicts it only at death,
  prop_element_tracker.cpp:563-618); every census-based consumer (SnapshotActorsByType lanes:
  drive/rack/floppybox/laptop, the sweep, the selftest) sees the actor TWICE; client lanes derive
  doomed ops under the dead-band eid.
- **Host-side identity theft door (worse):** container extract (prop_container_extract.cpp:62-169)
  and held-item express-if-unknown (trash_collect_sync.cpp:293-345) are role-ungated -- a client
  PropSpawn can resolve onto the HOST's own authoritative local. The exact-key path then stacks a
  client-band mirror over it + steals the reverse (GetPropElementIdForActor(hostActor) -> Invalid:
  destroy observer silenced, R2 baselines lose it). The FUZZY path is worse still: it REKEYS the
  host's actor to the client's key (:577-597) BEFORE any bind -- the IDENTITY-STEAL gate protects
  only wire-mirror-bound actors (wireMirrorOnly=true) and the anti-collision gate only kerfurs.
  **Today's ghost-twin cure (2026-06-10) literally works BY this corruption** -- the stacked row is
  what makes the straggler's pose stream resolve host-side.

## 2. The root (the reframe, user-approved after round 1)

Identity for KEYED props is host-authored by construction: save-loaded props get their identity by
key-adopt from the host; client-born props mint at EXPLICIT express seams that mint-and-BROADCAST
atomically (container extract, held-item express, F2 birth intents -- all three also claim their
actor at mint, :171/:400/:359). The client's PASSIVE census walk is the one place that mints an
identity NOBODY IS EVER TOLD ABOUT ("the re-seed still mints local eids on a client ... but
broadcasts nothing", net_pump.cpp:729) -- that silent mint is the zombie factory. MTA shape: clients
never mint element IDs for server-world entities.

Enumerated consumers of the silent client keyed local (round 1): the only load-bearing one was the
:311 decline predicate -- and rounds 2-3 proved even that needs no replacement (flows converge via
the H handback). Everything else sends doomed ops the host already drops.

## 3. The design (B + S + A' + H)

**(B) Client passive census: keyed -> key-index only.** `MarkPropElement` gains a source
discriminator (`kExpressSeam` | `kPassiveCensus`), explicit at every call site. Branch (after the
existing IsBoundMirrorNative / IsChildActor / reverse-early-out / K-5 gates): role==Client + keyed +
kPassiveCensus -> `IndexKeyForActor_` (refresh: actor, key, internalIdx) + return, NO Element row.
Keyless chipPile minting unchanged (the trash domain legitimately ships client-band eids). The key
read is already paid by the walk (prop_census.cpp:166) -- the refresh adds one leaf-mutex hash
write, zero new dispatches [V].

**(S) Sweep keyed-half moves to the key index** (the same membership B maintains). Measured basis:
the claim sweep dooms unclaimed KEYED locals too (join_membership_sweep.cpp:273), and R2
(save_transfer.cpp:784 `SendBlobDivergenceDeletes` -- explicit PropDestroy BY KEY, pre-bracket) is
the authoritative delete lane with the sweep as its :787 stale-fallback. Under B keyed props leave
the row universe, so: walk the keyed index entries; skip claimed (g_claimedActors, actor-keyed),
per-player classes, and actors with a live element row (that half belongs to the row walk); validate
IsLiveByIndex; **re-validate the live actor's CURRENT key against the entry** (mismatch = recycled
impostor or un-reindexed rekey -> skip + evict entry; the actor re-enters at the next walk's
re-index; safe side meanwhile = not doomable); doom under the same per-class completeness floor,
with claimed/doomed per-class accounting fed from the index walk (claimed actors counted there so
the floor's `claimed[C] >= manifest[C]` gate keeps its semantics for keyed classes). Piles stay
row-based. KEYED CHURN RE-BIND branch unchanged (its UnmarkKnownKeyedProp no-ops when no local
element exists).

**(A') Funnel invariant** in `CreateOrAdoptPropMirror`, on the fall-through-to-Install path:
`prior = EidForActor(actor)`; if `prior != eid` and `Get(prior)` is live:
- prior NON-mirror + role **Host** -> REJECT the install + LOUD log (the wall; the theft door is
  closed earlier by H, this catches any other path).
- prior NON-mirror + role **Client** + senderSlot==0 -> **DISSOLVE the provisional**: Take(prior)
  from PropMirrors() + ElementDeleter enqueue (wire-silent + reverse-safe BY CONSTRUCTION: no
  broadcast exists in Enqueue/Flush/~Element, and ~Element clears the reverse only under the
  ownership gate registry.cpp:262 `it->second == id`, so the mirror's overwritten reverse survives
  the deferred destructor [V]); key index KEPT; then Install; then `NotifyPropEidRebound` fanout
  (local_streams re-resolves its held-eid cache -- measured held-EDGE-cached -- if this actor is
  currently held). LOUD log with cls+key+loc.
- prior NON-mirror + Client + senderSlot!=0 -> REJECT + log.
- prior IS a mirror (different eid) -> REJECT + LOUD log (2-peer unreachable; 3+-peer rebind
  deferred).
The dissolve is NOT a dead guard: two reachable triggers [V] -- (i) post-quiescence
express-if-unknown straggler (client mints E_c at the express seam; H re-expresses E_h; the client
fuzzy-resolves its OWN actor); (ii) container-extract conflict (identical saved key cross-peer;
exact-key resolve). Clean joins log ZERO dissolves (acceptance-asserted).

**(H) Host authority gate** in remote_prop_spawn OnSpawn: ONE helper at the TWO resolution points
(entry of the exact-key `existing` block ~:270 -- its three RegisterPropMirror branches are one
block [V]; and the fuzzy-survivor point ~:526), BEFORE any rekey/converge/claim/bind: on role Host +
senderSlot!=0 + resolved actor is a live NON-mirror local OR untracked world actor -> the actor is
host-authoritative: enroll-if-untracked (MarkPropElement, host-range, kExpressSeam) + re-express via
`ExpressIncrementalSpawn` (Host-gated; kerfur-excluded internally -- for a kerfur: enroll + skip
re-express + log; no client kerfur provisional can exist, K-5) + return. A client-band MIRROR prior
(client-born prop) falls through to the normal idempotent adopt. Closes both the stack door and the
:577 rekey-theft door. The client converges: adopts E_h by key/fuzzy onto its actor; any provisional
dissolves via A'-client.

**(C) deleted** -- the trash_collect_sync:311 decline predicate stays tracker-known-only. The client
express is already quiescence-gated (:284-291, :260 [V]) so the E_c window is post-quiescence and
narrow; a pre-adopt touch-express converges via H.

Wire format unchanged -> **proto stays 121**. No new ReliableKind.

## 4. Accepted bounded residuals (documented, not built around)

- Fuzzy mis-bind among >=2 same-class stragglers within 30cm with per-peer keys: pre-existing
  Gap-I-1 limitation, logged; today's variant ends in host corruption, the new one in a logged
  mis-pair.
- Lane baselines keyed by a dissolved E_c: bounded-stale (drive lane self-cleans via the keep-rebuild
  drive_sync.cpp:270 [V]; others evict at OnDisconnect); pre-existing class shared with every
  UnmarkKnownKeyedProp.
- Pre-adopt carried-prop poses: window-dropped (host has no row) until the handback lands + the
  held-cache fanout / re-grab re-resolves -- replaces today's instantly-working-but-corrupting
  stacked row. PropDestroy is key-routed (dp.key [V]) so destroys propagate regardless.
- Side-data authored under E_c in a conflict flow correctly LOSES to the host's own authoritative
  rows (the v119 unmatched-eid strays measured this exact drop shape).
- Client keyed runtime spawns (Q-menu/toolgun) become identity-less instead of silently-locally-
  tracked -- equivalent (they were invisible cross-peer either way); the routed-through-host plan
  (prop_snapshot.cpp:616-618 "phase 2") stays consistent with B.

## 5. Acceptance (numeric A/B vs the archived s21b baseline)

1. drive_selftest reconnect scenario re-run (host seeds rack; client reconnects with the rack in its
   save): client resolves the rack under ONE eid (host-band); the frozen digest circle passes
   cross-peer (instrument unchanged from s21b).
2. Client log: keyed re-seed adds drop to keyless-piles-only (2356 -> ~869); `resolves to live
   actor` count unchanged (~2236); ZERO A'-dissolve / A'-reject / H-handback logs in a clean join.
3. Sweep verdicts vs baseline: doomed counts unchanged; completeness floor accounting present.
4. Registry shape post-join: client Prop rows == mirrors + keyless piles (keyed locals ~= 0); host
   unchanged.
Plus the standard pre-handoff checklist (perf/correctness audits, deploy hash-verify, 30s+ smoke,
log diff). Hands-on stays PENDING (honest status).

## 6. Round map (8 rounds; full Q/A in scratchpad qf_thread_stableid.md)

R1 consumers enumeration -> REFRAME (B), user green-lit. R2 ghost-twin-cure-is-the-corruption
discovered -> H born, C dropped; sweep-dooms-keyed + R2-delete-lane measured. R3 fuzzy rekey-theft
door measured -> H moved before the rekey; rebound fanout; E_c op-set enumerated. R4 kerfur
reachability + delivery gates + client-born re-bracket survival measured (snapshot walk has no
mirror filter). R5 deleter wire-silence/reverse-safety measured; doom key-revalidation added;
acceptance defined. R6 dissolve reachability named; census grep-measured. R7 four edges closed
(cost measured zero-new-dispatch; doom bounds; authority-correct side-data). R8 "that holds"
(critic re-verified :166 and :262 itself).
