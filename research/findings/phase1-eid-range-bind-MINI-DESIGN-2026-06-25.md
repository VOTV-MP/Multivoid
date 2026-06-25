# Phase 1 (b) — eid-range bind: install a HOST eid on a CLIENT save-loaded native — MINI-DESIGN (2026-06-25)

**ON REVIEW, ZERO code.** The one non-trivial registry interaction the Phase 1 build plan flagged: binding a
client LOCAL save-loaded native (chipPile | off-prop kerfur) to the HOST's `eid` (from the index->eid map)
WITHOUT colliding with the client's peer-eid space and keeping the Registry consistent. Decision (b) of the
build plan; build the bind only after this is reviewed.

## 1. The eid-space model (RE-confirmed, element.h:59-65)
- `kMaxElements = 65536`. **Host range [0, 32768)** (`AllocHostId`, authoritative side). **Peer range
  [32768, 65536)** (`AllocLocalId`, client-local elements). One element per eid; the Registry enforces it.
- A **LOCAL** element (`m_mirror=false`) is allocated in the range of its OWNER: on the client, a save-loaded
  native that `MarkPropElement` touched at seed holds a **peer-range** eid (`AllocLocalId`), tracked in
  `prop_element_tracker::g_actorToPropElementId` (native -> peer-eid).
- A **MIRROR** element (`m_mirror=true`) holds a SPECIFIC **host-range** eid the host minted, installed via
  `remote_prop::RegisterPropMirror(eid, actor, key, cls, senderSlot, rebindInPlace)` ->
  `MirrorManager<Prop>::Install(wireEid, ...)`. Its dtor routes `UnregisterMirror` (the host's PropDestroy
  owns it), NOT `FreeId`.

## 2. The model decision — a bound native is a MIRROR, not a re-ranged local
A save-loaded shared pile / off-kerfur IS a HOST-AUTHORITATIVE entity (docs/piles/08; the host owns its
identity). So its element SHOULD be a **MIRROR at the host eid E** -- the client native ACTOR is merely the
local RENDERING of the host's authoritative entity (principle 3, the parallel class hierarchy; exactly what
`RegisterPropMirror` already models for a host-EXPRESSED prop). The ONLY difference from a normal mirror is
that the actor PRE-EXISTS (loadObjects spawned it) instead of being fresh-spawned by the wire receiver. So
the bind is **"RegisterPropMirror onto the existing native"**, not a new mechanism.

Crucially you do NOT "change the native's eid from peer-range to host-range" -- an element's `m_id` is fixed
at install. The bind REPLACES the peer-range LOCAL element with a host-range MIRROR element bound to the same
actor. Two clean registry ops, no in-place re-ranging.

## 3. The bind operation — `BindLocalNativeToHostEid(native, E, key, cls)`
Game-thread, at load (driven by the §4.1 spawn-order counter: spawn #k of a family -> IdMap keyless-entry #k
-> its eid E). Two steps:

1. **Release the native's peer-range LOCAL element.** Resolve `g_actorToPropElementId[native]` (the peer
   eid); drop it via the existing local-element teardown (`prop_element_tracker` Unmark path / Take+
   ElementDeleter) -> the peer eid returns to the peer free stack, `g_actorToPropElementId[native]` is
   erased, the key-index entry is evicted. The peer-eid space is left clean (freed, not leaked) -> NO
   peer-space collision possible.
2. **Install the host-range MIRROR at E onto the native.** `RegisterPropMirror(E, native, key, cls,
   senderSlot=hostSlot0, rebindInPlace=<see S4>)`. Now `Registry::Get(E)` resolves to the native; a host
   `PropPose(E)` / `PropDestroy(E)` drives / removes it. The native's element is `m_mirror=true`.

After the bind the native has EXACTLY ONE element (the host-eid mirror); the peer element is gone. One
element per eid in BOTH ranges -> Registry consistent by construction.

## 4. Collision analysis (the user's worry, exhaustively)
"What if host-eid E is already occupied on the client?" Three cases:

- **(i) E unoccupied** (the host has not PropSpawn'd it yet -- the NORMAL case if the bind runs before the
  host's pile expression is processed): `RegisterPropMirror` installs fresh at E bound to the native. Clean.
- **(ii) E already a MIRROR** (a host `PropSpawn(E)` arrived BEFORE the bind and spawned a SEPARATE fresh
  actor -> a transient DUP): the bind calls `RegisterPropMirror(E, native, ..., rebindInPlace=true)` -> it
  RE-POINTS the existing E onto the native (SetActor) instead of HEAD-rejecting the duplicate, and the now-
  orphaned host-spawned actor is echo-destroyed (the same morph-rebind path `rebindInPlace` was built for,
  remote_prop.h:83). Net: one element at E, bound to the native; the redundant fresh actor is removed. NO
  dup survives.
- **(iii) E collides with a DIFFERENT object's E**: impossible -- the host mints each eid uniquely via
  `AllocHostId` (one element per eid at the source). Not a real case; no handling needed.

Peer-space collision: NONE -- step 3.1 frees the native's peer eid before step 3.2 touches the host range;
the two ranges are disjoint and each op is a single-element registry mutation.

## 5. Ordering — prefer bind-BEFORE host pile expression (makes (i) the norm, (ii) the rare race)
The bind should run as the natives load (the §4.1 spawn-order edge), which precedes SnapshotComplete's pile
PropSpawn drain in the normal flow. Then when the host's `PropSpawn(E)` arrives, `remote_prop::OnSpawn`'s
EXISTING connect-resnapshot dedup finds E already bound (to the native) -> rebinds in place / no fresh spawn
-> case (i) holds and (ii) is only the rare "host PropSpawn beat the bind" race, covered by `rebindInPlace`.
This reuses the dedup already shipped for the connect re-snapshot; no new dedup logic.

## 6. Two free wins from binding-as-mirror
- **The claim sweep can no longer doom a bound native.** `RunDivergenceSweep_` excludes mirrors at the SOURCE
  (`remote_prop_spawn.cpp:1067` `if (pr.mirror) continue`). So a bound pile/kerfur is automatically OUT of
  the doom set -- the over-destroy (docs/piles/10) cannot touch a bound native at all (the floor stays as the
  net for UNBOUND ones during migration).
- **K-5 consistency (kerfur).** The K-5 gate forbids a CLIENT minting a PEER-range eid for a kerfur (the
  grab-dupe root). The bind installs a HOST-range MIRROR (the host's eid), NOT a client mint -> it is exactly
  what K-5 wants (the host owns the kerfur eid; the client binds to it). The off-kerfur (jUuC) bind is
  K-5-clean by construction.

## 7. Registry-consistency invariants (the bind must preserve all)
1. One element per eid (both ranges) -- guaranteed: free peer-E' before install host-E; rebindInPlace never
   creates a second element at E.
2. One element per actor -- guaranteed: step 3.1 removes the native's local element before 3.2 installs the
   mirror; `g_actorToPropElementId` no longer names the native (mirrors aren't in it).
3. No peer-eid leak -- the freed peer eid returns to the peer free stack (the standard local-element drain).
4. No dangling reverse-map -- 3.1 erases `g_actorToPropElementId[native]` + the key index; the mirror is
   tracked only in the MirrorManager + remote_prop's mirror maps.
5. Mirror dtor routes UnregisterMirror (host owns lifecycle) -- correct for a host-authoritative pile.

## 8. Open sub-items for the bind build (after this review)
1. **rebindInPlace default for the bind:** S5 prefers bind-first (case i), so the bind passes
   `rebindInPlace=true` defensively for the (ii) race only. Confirm the OnSpawn dedup path already rebinds a
   host PropSpawn onto a bound native (it should -- it is the morph-rebind path).
2. **eid-lifetime trace (build plan S9.3 / design S9.3): DONE 2026-06-25, PASS.** The read-only host trace
   (`coop/dev/eid_lifetime_trace.{h,cpp}`, binary `F4A3F122`) recorded the capture-eid (Collect{Pile,Kerfur}
   Transforms) and compared it to the wire-eid (BuildPropSpawnPayload_) per actor: **captured=874
   (870 piles + 4 kerfurs), wire-checked=874, matched=874, mismatched=0 -> STABLE.** The host eid is
   identical capture->wire for every keyless native, so a bind keyed on the capture-eid matches the host's
   wire expression. The last correctness link is closed; the bind model is sound. (Pose-eid for a GRABBED
   kerfur is the same GetPropElementIdForActor resolution -> covered by the same stability; a hands-on grab
   would confirm the pose path directly if ever doubted.)
3. **Unbind on migration retirement (Phase 4 only):** when the position layer retires, nothing changes here
   (the bind is the eid path); noted only so Phase 4 does not re-introduce a peer-range local for a pile.
4. **Per-player exemption stays:** the bind is for shared piles/kerfurs only; per-player state props are never
   bound (they have no host eid in the map -- the map's families are chipPile + kerfurOff only).

## Source map
`element.h:59-65` (eid ranges) · `mirror_manager.h:85` (`Install(wireEid,...)`), `:190` (AllocHostId/
AllocLocalId) · `remote_prop.h:90` (`RegisterPropMirror`, `:83` rebindInPlace contract) ·
`prop_element_tracker` (`g_actorToPropElementId`, MarkPropElement peer-range local, UnmarkKnownKeyedProp
drain) · `remote_prop_spawn.cpp:1067` (sweep excludes mirrors) · K-5 gate (prop_element_tracker.cpp:329) ·
`docs/COOP_STABLE_ID_SIDECAR.md` S2.1/S3/S3.6 · Phase 1 build plan S9(b).
