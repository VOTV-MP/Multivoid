// coop/props/container_contents_sync.cpp -- see coop/props/container_contents_sync.h.

#include "coop/props/container_contents_sync.h"

#include "coop/element/registry.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/inventory.h"     // ResolveSaveSlot
#include "ue_wrap/actors/prop.h"          // WalksToBase
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/component_calls.h"  // CallParamless
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/vm_dispatch.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace coop::props::container_contents_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;
namespace vm = ue_wrap::vm_dispatch;

using coop::element::LivePropActor;

constexpr uint8_t kOpContents = 0;

// One verb id: both watched verbs share one callback (mark-dirty), so they need no distinction.
constexpr int kVerbDirty = 1;

// The sweep is the drain of an edge-driven set, not a poll -- it does no work when nothing was
// dispatched. 250 ms keeps a burst of addLoot/addObject calls (a loot roll fires addObject x4)
// coalesced into ONE broadcast instead of four.
constexpr uint64_t kSweepMs = 250;

// A container with more records than this is not shipped: the blob would approach the chunk
// transport ceiling and a truncated blob is a silent lie. Real containers hold single digits.
constexpr size_t kMaxRecordsPerContainer = 512;

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_verbsRegistered = false;
bool g_announced = false;
uint64_t g_nextSweep = 0;
uint32_t g_nextSeq = 1;

coop::blob_chunks::Assembler g_asm;

// Containers marked dirty by the 0x45 edge, drained on the next sweep, keyed by ELEMENT ID.
// NOT by the raw component pointer: `IsLive` cannot detect an address recycled by a NEW UObject
// between the edge and the drain (reflection.cpp says so outright, and registry.h requires
// IsLiveByIndex for any pointer held across ticks). An eid is a stable identity, so the drain
// re-resolves the actor forward and a destroyed container simply stops resolving.
std::set<uint32_t> g_dirty;

// HOST: the content this host most recently PUBLISHED for an eid by ANY route -- fan-out or a
// targeted connect seed. This, not g_sentHash, is the compare-and-swap baseline: it answers "what
// did I tell that peer the world looked like", which is the only thing a client's edit can
// honestly be judged against. g_sentHash answers a DIFFERENT question ("may I skip the next
// fan-out"), and a targeted send must not answer yes to it.
std::map<uint32_t, uint64_t> g_publishedHash;

// Per-eid content hash of the last blob we SENT / APPLIED. Skipping an unchanged blob is what
// keeps the orphaned-buffer cost bounded: a steady-state re-broadcast applies nothing and
// allocates nothing (the raw-write path deliberately orphans the old arrays, so an apply that
// changes nothing must not happen at all).
std::map<uint32_t, uint64_t> g_sentHash;
std::map<uint32_t, uint64_t> g_appliedHash;

// CLIENT: the last HOST truth this peer applied for an eid -- the base it declares when it
// authors. Deliberately NOT the same map as g_appliedHash even though both are written at the same
// moment, because they answer questions that diverge the instant this peer mutates locally:
//   g_appliedHash = "does an incoming blob change anything for me?"   -> local mutation CLEARS it
//                                                                        (else a corrective
//                                                                        re-publish is skipped as
//                                                                        a duplicate and the peer
//                                                                        never converges)
//   g_baseHash    = "which host truth was I editing?"                 -> local mutation KEEPS it
//                                                                        (that IS the edit's base;
//                                                                        clearing it makes the peer
//                                                                        declare base 0 and the
//                                                                        host refuse every write)
// Fusing them produced BOTH failures in turn, one per smoke.
std::map<uint32_t, uint64_t> g_baseHash;

// name -> UClass* memo. R::FindClass is a FULL GUObjectArray walk; calling it per record per
// broadcast is the exact per-frame-full-scan pattern the project's post-ship audit rule bans.
std::map<std::wstring, void*> g_classMemo;

// Containers whose broadcast was refused by the transport (I-3) -- retried by the sweep.
std::set<uint32_t> g_retry;

// Per-eid timestamp of THIS peer's own last verb edge. The host uses it to detect a client write
// that raced a host-side change; the client uses nothing but its own bookkeeping.
std::map<uint32_t, uint64_t> g_localChangeMs;

// A client write whose baseHash is stale is rejected within this window of a host-side change.
constexpr uint64_t kConflictWindowMs = 1500;

// The rollback-need instrument (R11b): how many client writes the host has refused. The design
// deliberately ships WITHOUT a rollback shape -- a refused write is corrected by the host
// re-publishing its own truth, and the loser simply sees the container snap back. Whether that is
// ever noticeable is an EMPIRICAL question, and this counter is how it gets answered. Do not
// build a rollback until this number is non-trivial in real play.
uint64_t g_conflictRejects = 0;

// Inbound blobs for an eid not yet resolvable (birth skew / mid-activity join, principle 8).
struct Parked { std::vector<uint8_t> blob; std::chrono::steady_clock::time_point at; };
std::map<uint32_t, Parked> g_parked;
constexpr int kParkTtlSec = 30;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// ---- reflected offsets (resolved once, then cached) -----------------------------------------

int32_t g_offInvIndex  = -2;  // propInventory_C.Index
int32_t g_offInvPlayer = -2;  // propInventory_C.Player  -- the world-vs-PERSONAL discriminator
int32_t g_offInvOwner  = -2;  // propInventory_C.Owner   -- the Aprop_container_C
int32_t g_offGObjStack = -2;  // saveSlot_C.GObjStack
int32_t g_offPropInv   = -2;  // prop_container_C.propInventory
void* g_containerCls = nullptr;

// The two re-derive verbs, resolved ONCE from the class that DECLARES each -- never from the
// instance's class. See ResolveRederiveFns for why the per-instance-class cache this replaces
// could never resolve anything.
void* g_fnUpdateVol  = nullptr;   // Aprop_container_C::updateVolumesAndMass
void* g_fnRecalcName = nullptr;   // UpropInventory_C::recalculateNames
bool  g_rederiveResolved = false;

// Resolve an offset once; -1 means "looked and failed" (never retried, never guessed).
int32_t CachedOffset(int32_t& slot, void* cls, const wchar_t* name) {
    if (slot == -2) {
        slot = cls ? R::FindPropertyOffset(cls, name) : -1;
        if (slot < 0)
            UE_LOGW("container_contents: could not resolve %ls -- lane inert for it", name);
    }
    return slot;
}

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

// The propInventory component of a container actor, or null.
void* InventoryOf(void* containerActor) {
    if (!containerActor) return nullptr;
    if (CachedOffset(g_offPropInv, R::ClassOf(containerActor), L"propInventory") < 0) return nullptr;
    void* inv = ReadAt<void*>(containerActor, g_offPropInv);
    return (inv && R::IsLive(inv)) ? inv : nullptr;
}

// The owning Aprop_container_C of a propInventory component, or null.
void* OwnerOf(void* inv) {
    if (!inv) return nullptr;
    if (CachedOffset(g_offInvOwner, R::ClassOf(inv), L"Owner") < 0) return nullptr;
    void* owner = ReadAt<void*>(inv, g_offInvOwner);
    return (owner && R::IsLive(owner)) ? owner : nullptr;
}

// BOUNDARY 1, fail-closed: true iff this component is a WORLD container we may author.
// `Player` true = personal inventory (mainPlayer / ui_playerInventory share the SAME global
// GObjStack) -- authoring it from the host would WIPE that peer's inventory. An UNRESOLVABLE
// offset is treated exactly like Player==true: we refuse rather than guess.
//
// WHAT IS BEHIND THE REFUSAL -- now measured, 2026-07-24. This guard was written to fence off
// something whose contents nobody had characterised; the refusal was correct then and is
// UNCHANGED now, but the far side is no longer unknown:
//   GObjStack[0] IS the local player's inventory, by construction. Aprop_inventoryContainer_player_C
//   carries a component template `propInventory_GEN_VARIABLE` serializing index=0, player=True,
//   customVolume=50000 (the base prop_container_C's template has NO overrides), so the personal
//   slot is baked at construction rather than restored -- which is also why that class's loadData
//   override is an empty stub. Both live write paths land there: the world pickup
//   (mainPlayer::putObjectInventory2 -> playerContainer.propInventory.addObject) and the container
//   slot press. Full RE: votv-player-inventory-two-layer-RE-2026-07-24.md SS4.2/SS5.4.
//
// So `Player != 0` now serves TWO lanes from opposite sides of one boundary, and that is
// deliberate, not an inconsistency:
//   - HERE it is a REFUSAL   -- this lane authors world containers and must never author a peer's
//                               personal store (the wipe above);
//   - in ue_wrap::inventory::ReadLivePersonalStore it is the ADDRESS ASSERTION -- that reader must
//     read the personal store and nothing else, and refuses when the flag is 0.
// Both are fail-closed in their own direction. This lane's behaviour is untouched by that reader,
// which is READ-ONLY: nothing wires, persists or applies the live store (gated on the open scope
// questions in votv-per-player-inventory-scope-BRIEF-2026-07-24.md).
//
// KNOWN DUPLICATION, deliberately NOT resolved here (single-axis discipline): GObjStackSlot() below
// and that reader each resolve GObjStack/Index themselves. Folding them into one ue_wrap primitive
// is a behaviour-preserving refactor of a SHIPPED lane and belongs in its own arc with its own
// equivalence proof -- not bundled into a read-path change.
bool IsWorldContainerInventory(void* inv) {
    if (!inv) return false;
    if (CachedOffset(g_offInvPlayer, R::ClassOf(inv), L"Player") < 0) return false;
    return ReadAt<uint8_t>(inv, g_offInvPlayer) == 0;
}

// The live TArray<Fstruct_save> slot for this component's contents inside the global GObjStack,
// as {slot base pointer}. Null if the saveSlot / offset / index is not resolvable or in range.
// The contents array lives at +0 of the struct_mObject element (its single field).
uint8_t* GObjStackSlot(void* inv) {
    if (!inv) return nullptr;
    void* save = ue_wrap::inventory::ResolveSaveSlot();
    if (!save) return nullptr;
    if (CachedOffset(g_offGObjStack, R::ClassOf(save), L"GObjStack") < 0) return nullptr;
    if (CachedOffset(g_offInvIndex, R::ClassOf(inv), L"Index") < 0) return nullptr;
    const int32_t idx = ReadAt<int32_t>(inv, g_offInvIndex);
    if (idx < 0) return nullptr;  // -1 = never initialised; nothing to ship or apply
    const SR::Arr stack = SR::ReadArr(save, g_offGObjStack);
    if (idx >= stack.num) return nullptr;
    return const_cast<uint8_t*>(stack.data) + static_cast<size_t>(idx) * SR::kMxStride;
}

void* ContainerClass() {
    if (!g_containerCls) g_containerCls = R::FindClass(L"prop_container_C");
    return g_containerCls;
}

// BOUNDARY 2: does this record describe a container (whose ints[] would carry a GObjStack index
// meaningful only on the sender)?
void* ClassByName(const std::wstring& name) {
    if (name.empty()) return nullptr;
    auto it = g_classMemo.find(name);
    if (it != g_classMemo.end()) return it->second;
    void* cls = R::FindClass(name.c_str());
    g_classMemo.emplace(name, cls);   // a null result is memoized too -- one walk per name, ever
    return cls;
}

bool RecordIsNestedContainer(const SR::SaveRecord& r) {
    void* base = ContainerClass();
    if (!base) return false;
    void* cls = ClassByName(r.className);
    return cls && ue_wrap::prop::WalksToBase(cls, base);
}

// I-4/I-6: is this pointer actually a propInventory_C component? The 0x45 filter matches on the
// VERB NAME alone, so any class with an `addObject`/`takeObj` would arrive here; and the apply
// side must not read a cached component offset off an eid that resolved to something else.
void* InventoryClass() {
    static void* cls = nullptr;
    if (!cls) cls = R::FindClass(L"propInventory_C");
    return cls;
}
bool IsInventoryComponent(void* obj) {
    void* base = InventoryClass();
    return base && obj && ue_wrap::prop::WalksToBase(R::ClassOf(obj), base);
}
bool IsContainerActor(void* actor) {
    void* base = ContainerClass();
    return base && actor && ue_wrap::prop::WalksToBase(R::ClassOf(actor), base);
}

// BOUNDARY 2. A nested container's ints[0][0] is its own GObjStack index -- a slot number in the
// SENDER's array, meaningless on the receiver. It must NOT survive the wire.
//
// CLEARING ints[] IS WRONG, and the bytecode says so. prop_container::loadData statements 3-5 are
// three UNGUARDED statements -- Array_Get(data.ints[0]) -> Array_Get(that.ints[0]) ->
// Let propInventory.index -- with no Array_IsValidIndex and no branch anywhere in the function.
// UKismetArrayLibrary's Array_Get ZERO-FILLS its out param on an out-of-range read, so an empty
// ints[] yields index = 0, NOT "no index". propInventory::init then branches on `index >= 0`,
// which 0 PASSES -- so the container skips the fresh-append branch and REUSES GObjStack[0], a real
// slot owned by some other container. Clearing would have manufactured exactly the cross-slot
// corruption this lane exists to avoid.
//
// So: write the SENTINEL -1 into ints[0][0] instead, which is the value init's guard is written
// against (CDO Default__propInventory_C.index = -1). Every other ints entry is preserved -- we
// overwrite one slot rather than destroying state we did not measure.
void NeuterNestedIndex(SR::SaveRecord& r) {
    if (r.ints.empty()) r.ints.resize(1);
    if (r.ints[0].empty()) r.ints[0].resize(1);
    r.ints[0][0] = -1;
}

// ---- blob grammar ----------------------------------------------------------------------------

void AppU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

bool ReadContents(void* inv, std::vector<SR::SaveRecord>& out) {
    uint8_t* slot = GObjStackSlot(inv);
    if (!slot) return false;
    const SR::Arr objs = SR::ReadArr(slot, 0);  // struct_mObject.obj @ +0
    if (static_cast<size_t>(objs.num) > kMaxRecordsPerContainer) {
        UE_LOGW("container_contents: %d records exceeds the %zu cap -- refusing to ship a "
                "truncated slice", objs.num, kMaxRecordsPerContainer);
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(objs.num));
    for (int32_t i = 0; i < objs.num; ++i) {
        SR::SaveRecord r;
        SR::ReadSaveRecord(objs.data + static_cast<size_t>(i) * SR::kSaveStride, r);
        if (RecordIsNestedContainer(r)) NeuterNestedIndex(r);
        out.push_back(std::move(r));
    }
    return true;
}

void AppU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

bool RdU64(const std::vector<uint8_t>& b, size_t& o, uint64_t& v) {
    if (o + 8 > b.size()) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[o + i]) << (i * 8);
    o += 8;
    return true;
}

// `baseHash` = the last truth for this eid that the AUTHOR had applied from the host (0 when the
// author is the host itself, or when the author has never applied anything for this eid). It is
// what lets the host distinguish "the client edited the world I published" from "the client
// edited a world that has since moved on" -- without it a full-slice write from a stale author
// would silently erase a host addition the author had not yet received.
std::vector<uint8_t> PackContents(uint32_t eid, uint64_t baseHash,
                                  const std::vector<SR::SaveRecord>& recs) {
    std::vector<uint8_t> b;
    b.push_back(kOpContents);
    W::AppU32(b, eid);
    AppU64(b, baseHash);
    AppU16(b, static_cast<uint16_t>(recs.size()));
    for (const auto& r : recs) W::SerSave(b, r);
    return b;
}

// THE hash every gate and every compare-and-swap uses. It is taken over a pack with baseHash
// ZEROED, so it names the CONTENTS ALONE: the same records must hash the same no matter which
// peer authored them or what base that author edited from. Hashing the raw blob instead would
// make an author's private bookkeeping part of the content identity, and the host's CAS would
// then compare two quantities that can never be equal.
uint64_t ContentHash(uint32_t eid, const std::vector<SR::SaveRecord>& recs) {
    return coop::blob_chunks::Fnv64(PackContents(eid, 0, recs));
}

// ---- host: broadcast one container -----------------------------------------------------------

// Returns false if the send was refused (caller arms the retry).
//
// v125: run by BOTH peers. On the host `toSlot < 0` fans out to everyone; on a client the same
// call reaches the host alone (a client's only peer), which is exactly the author->arbiter edge.
bool BroadcastContainer(coop::net::Session* s, uint32_t eid, void* inv, int toSlot, bool force) {
    std::vector<SR::SaveRecord> recs;
    if (!ReadContents(inv, recs)) return true;  // nothing resolvable -- not a transport failure
    // The base we are editing from: for a client, the last host truth it applied. The host
    // authors from its own state and sends 0 (its word IS the base).
    uint64_t baseHash = 0;
    if (!IsHost()) {
        auto it = g_baseHash.find(eid);
        if (it != g_baseHash.end()) baseHash = it->second;
    }
    const std::vector<uint8_t> blob = PackContents(eid, baseHash, recs);
    const uint64_t h = ContentHash(eid, recs);
    if (!force) {
        auto it = g_sentHash.find(eid);
        if (it != g_sentHash.end() && it->second == h) return true;  // unchanged -- say nothing
    }
    if (blob.size() > coop::blob_chunks::MaxBlobBytes()) {
        UE_LOGW("container_contents: eid=%u blob %zu B exceeds the transport ceiling -- dropped "
                "(contents stay diverged; increment 2 owes a bulk path)", eid, blob.size());
        return true;
    }
    const bool ok = (toSlot < 0)
        ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::ContainerContents,
                                      g_nextSeq++, blob)
        : coop::blob_chunks::SendBlobToSlot(s, toSlot, coop::net::ReliableKind::ContainerContents,
                                            g_nextSeq++, blob);
    if (ok) {
        if (toSlot < 0) g_sentHash[eid] = h;  // only a FAN-OUT establishes what every peer has
        // ...but ANY publication -- fan-out or a targeted connect seed -- establishes what the
        // RECEIVER was told, and that is the baseline a later client write must be judged against.
        // Fusing the two into one map is what made the host refuse every client write that followed
        // a join: the seed is targeted, so g_sentHash stayed empty, the CAS compared against 0, and
        // "authored from base N but the host published 0" fired for every container in the world.
        if (IsHost()) g_publishedHash[eid] = h;
        UE_LOGI("container_contents: eid=%u shipped %zu records (%zu B)%s%s",
                eid, recs.size(), blob.size(),
                toSlot < 0 ? "" : " [targeted]",
                IsHost() ? "" : " [client-authored]");
    }
    return ok;
}

// HOST: pass an accepted client-authored slice on to every OTHER peer. The author is EXCLUDED --
// echoing a peer's own state back to it reverts a newer local value and primes the baseline over
// it, silently eating that player's next action (the eaten-scroll race,
// [[lesson-presser-authored-state-not-intent-for-invisible-verbs]]).
void RelayToOthers(coop::net::Session* s, uint8_t authorSlot, const std::vector<uint8_t>& blob) {
    if (!s || !IsHost()) return;
    size_t sent = 0;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (slot == static_cast<int>(authorSlot)) continue;   // NEVER back to the author
        if (!s->IsSlotConnected(slot)) continue;
        if (coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::ContainerContents,
                                              g_nextSeq++, blob)) {
            ++sent;
        }
    }
    if (sent) {
        UE_LOGI("container_contents: relayed slot-%u authored slice to %zu other peer(s)",
                static_cast<unsigned>(authorSlot), sent);
    }
}

// ---- the dirty drain (host) --------------------------------------------------------------------

void DrainDirty(coop::net::Session* s) {
    // Transport-refusal retries fold into the SAME set rather than being drained a second time
    // in this pass -- a refused send re-enters the next sweep 250 ms later instead of re-running
    // the whole read twice inside one tick.
    if (!g_retry.empty()) {
        g_dirty.insert(g_retry.begin(), g_retry.end());
        g_retry.clear();
    }
    if (g_dirty.empty()) return;

    std::set<uint32_t> dirty;
    dirty.swap(g_dirty);

    for (uint32_t eid : dirty) {
        // Resolve FORWARD from the stable eid every sweep: a container destroyed since the edge
        // simply stops resolving, so there is no stale pointer to deref.
        void* actor = LivePropActor(eid);
        if (!actor || !IsContainerActor(actor)) continue;
        void* inv = InventoryOf(actor);
        if (!inv || !IsWorldContainerInventory(inv)) continue;   // BOUNDARY 1 (fail-closed)
        if (!BroadcastContainer(s, eid, inv, -1, /*force=*/false)) g_retry.insert(eid);
    }
}

// ---- apply (client) ---------------------------------------------------------------------------

// Re-derive the SETTER-MANAGED state through the engine's own verbs. We never raw-write currVol /
// Mass / the display names. Measured safe: updateVolumesAndMass calls only `Get Volume`; the
// EJECTOR checkObjectsVolume (which calls takeObj) is deliberately NOT called.
//
// RESOLVE FROM THE DECLARING CLASS, NOT THE INSTANCE'S CLASS. `R::FindFunction` matches on
// `OuterOf(fn) == owningClass` EXACTLY (reflection.cpp:427) and does NOT walk the superclass
// chain. `updateVolumesAndMass` is declared ONLY on Aprop_container_C (SDK prop_container.hpp:32)
// while every real container is a SUBCLASS (Aprop_inventoryContainer_drone_C : Aprop_container_C),
// so the previous per-instance-class cache resolved nullptr for EVERY container and the re-derive
// had never once run -- silently, because a null UFunction* was simply skipped. That is what the
// client's one-apply-stale currVol was (2026-07-22 census: applied 2 records -> vol 0.0; applied 0
// records -> vol 1495.7 -- always the value left by the last NATIVE mutation).
//
// A base-declared UFunction dispatched against a derived instance is correct: the subclasses do
// not override it (SDK: the name appears in prop_container.hpp only), so there is exactly one
// implementation and one property layout in play.
//
// The resolution is LOGGED, including failure. A re-derive verb that silently does not resolve is
// the same failure family as the vm_dispatch latch and as this very bug -- a mechanism that
// reports success and does nothing. It must be loud.
void ResolveRederiveFns() {
    if (g_rederiveResolved) return;
    void* contCls = ContainerClass();
    void* invCls  = InventoryClass();
    if (!contCls || !invCls) return;  // classes not loaded yet -- retry on the next apply
    g_rederiveResolved = true;
    g_fnUpdateVol  = R::FindFunction(contCls, L"updateVolumesAndMass");
    g_fnRecalcName = R::FindFunction(invCls,  L"recalculateNames");
    if (g_fnUpdateVol && g_fnRecalcName) {
        UE_LOGI("container_contents: re-derive verbs resolved (updateVolumesAndMass=%p on "
                "prop_container_C, recalculateNames=%p on propInventory_C)",
                g_fnUpdateVol, g_fnRecalcName);
    } else {
        UE_LOGW("container_contents: re-derive verb MISSING (updateVolumesAndMass=%p "
                "recalculateNames=%p) -- applied contents will show a STALE currVol / names",
                g_fnUpdateVol, g_fnRecalcName);
    }
}

void RederiveManagedState(void* owner, void* inv) {
    ResolveRederiveFns();
    if (owner && g_fnUpdateVol)  ue_wrap::component_calls::CallParamless(owner, g_fnUpdateVol);
    if (inv   && g_fnRecalcName) ue_wrap::component_calls::CallParamless(inv,   g_fnRecalcName);
}

// What an inbound blob actually DID to this peer. A bare bool cannot express it: "handled" and
// "changed something" are different facts, and the relay decision needs the second one.
//
// The audit caught this as a CRITICAL: OnContentsChunk gated RelayToOthers on a bool that was true
// for BOTH "the host accepted and applied a client write" AND "the host REFUSED it as stale". A
// refused write was therefore relayed to every other client -- arriving stamped as slot 0, i.e. as
// HOST TRUTH, at peers that run no CAS of their own (the arbitration branch is host-only) and so
// applied it unconditionally. The compare-and-swap protected the author and the host and nobody
// else.
enum class Ingest {
    Park,      // the eid does not resolve yet (birth skew / mid-join) -- park and retry
    Handled,   // dealt with and deliberately NOT applied: refused, malformed, non-container,
               // BOUNDARY 1, or a no-op duplicate. NEVER relayed.
    Applied,   // this peer's state actually changed. The only outcome the host may pass on.
};

// Applied / Handled / Park -- see Ingest.
Ingest ApplyContents(uint32_t eid, const std::vector<SR::SaveRecord>& recs, uint64_t blobHash) {
    void* actor = LivePropActor(eid);
    if (!actor) return Ingest::Park;
    // The wire eid must name a CONTAINER before we read a cached component offset off it.
    if (!IsContainerActor(actor)) {
        UE_LOGW("container_contents: eid=%u does not resolve to a container -- refusing", eid);
        return Ingest::Handled;
    }
    void* inv = InventoryOf(actor);
    if (!inv || !IsInventoryComponent(inv)) return Ingest::Park;
    {   // Identical contents -> do nothing. The raw-write orphans the previous arrays, so a
        // no-op apply must not allocate at all (that is what bounds the leak).
        auto it = g_appliedHash.find(eid);
        if (it != g_appliedHash.end() && it->second == blobHash) return Ingest::Handled;
    }
    if (!IsWorldContainerInventory(inv)) {           // BOUNDARY 1 (fail-closed)
        UE_LOGW("container_contents: eid=%u resolves to a PERSONAL inventory (or an unresolvable "
                "Player flag) -- refusing to apply", eid);
        return Ingest::Handled;                       // resolved; deliberately not applied
    }
    uint8_t* slot = GObjStackSlot(inv);
    if (!slot) return Ingest::Park;

    // GMalloc pre-flight: without it we would silently write an EMPTY array over real contents.
    if (void* probe = R::EngineAlloc(16)) {
        R::EngineFree(probe);
    } else {
        UE_LOGW("container_contents: eid=%u -- EngineAlloc unavailable; refusing to write", eid);
        return Ingest::Handled;
    }

    const int32_t n = static_cast<int32_t>(recs.size());
    void* buf = SR::AllocZeroed(static_cast<size_t>(n), static_cast<size_t>(SR::kSaveStride));
    if (!buf && n > 0) {
        UE_LOGW("container_contents: eid=%u -- alloc of %d records failed; leaving contents", eid, n);
        return Ingest::Handled;
    }
    for (int32_t i = 0; i < n; ++i)
        SR::WriteSaveRecord(reinterpret_cast<uint8_t*>(buf) + static_cast<size_t>(i) * SR::kSaveStride,
                            recs[i]);
    // The previous buffer is intentionally orphaned -- recursively freeing the nested group
    // sub-arrays, minted FStrings and signal rows is far more crash-prone than leaking them, and
    // the engine never double-frees a buffer it has lost the pointer to.
    //
    // NOTE the difference from inventory::ApplyToSaveObject, whose identical orphaning is bounded
    // because it fires ONCE PER JOIN. This lane is steady-state, so the same contract would be
    // UNBOUNDED here. What bounds it is the content-hash gate above: an apply only happens when
    // the contents actually CHANGED, so the orphan count tracks real container mutations rather
    // than broadcast frequency.
    SR::WriteArrHeader(slot, 0, buf, n);

    g_appliedHash[eid] = blobHash;
    // The base a later local edit will declare. Set here and NEVER cleared by our own verb edge.
    if (!IsHost()) g_baseHash[eid] = blobHash;
    RederiveManagedState(OwnerOf(inv), inv);
    UE_LOGI("container_contents: eid=%u applied %d records", eid, n);
    return Ingest::Applied;
}

// HOST: may this client-authored slice be applied? The compare-and-swap that keeps a stale author
// from erasing a change it never saw. Returns false (and logs) when the write is refused.
bool HostAcceptsClientWrite(uint32_t eid, uint64_t baseHash, uint8_t authorSlot) {
    // What the host last PUBLISHED for this eid is what an up-to-date author must have edited
    // from. An author that never received anything for this eid sends 0 and cannot be validated,
    // so it is refused rather than trusted -- fail-closed, same posture as BOUNDARY 1.
    uint64_t published = 0;
    auto it = g_publishedHash.find(eid);
    if (it != g_publishedHash.end()) published = it->second;

    const bool baseMatches = (baseHash != 0 && baseHash == published);
    bool hostChangeInFlight = false;
    if (baseMatches) {
        // The author edited the world the host had published. Still refuse if the HOST changed
        // this container within the conflict window -- that change is in flight and the author
        // provably had not seen it.
        auto lc = g_localChangeMs.find(eid);
        hostChangeInFlight = (lc != g_localChangeMs.end() &&
                              NowMs() - lc->second <= kConflictWindowMs);
        if (!hostChangeInFlight) return true;
    }

    ++g_conflictRejects;
    // Name WHICH condition failed. Reporting both at once is how the first run of this CAS read as
    // "a host-side change raced it" when the truth was that the host had never recorded publishing
    // anything at all.
    UE_LOGW("container_contents: CONFLICT eid=%u slot %u -- %s (author base=%llu, host published=%llu). "
            "Write REFUSED; re-publishing host truth to the author. Total refused this session: %llu",
            eid, static_cast<unsigned>(authorSlot),
            hostChangeInFlight ? "a HOST-side change is in flight within the conflict window"
                               : "the author edited a state the host has not published (STALE BASE)",
            static_cast<unsigned long long>(baseHash),
            static_cast<unsigned long long>(published),
            static_cast<unsigned long long>(g_conflictRejects));
    return false;
}

Ingest ParseAndApply(const std::vector<uint8_t>& blob, uint32_t& outEid, uint8_t senderSlot) {
    size_t o = 0;
    uint8_t op = 0;
    if (!W::RdU8(blob, o, op) || op != kOpContents) return Ingest::Handled;  // unknown op
    if (!W::RdU32(blob, o, outEid)) return Ingest::Handled;
    uint64_t baseHash = 0;
    if (!RdU64(blob, o, baseHash)) return Ingest::Handled;
    // HOST arbitration: a client-authored slice is validated BEFORE it touches anything. A refusal
    // is not silent -- the host immediately re-publishes its own truth so the refused author
    // converges instead of sitting on a divergent view.
    if (IsHost() && senderSlot != 0 && !HostAcceptsClientWrite(outEid, baseHash, senderSlot)) {
        auto* s = g_session.load(std::memory_order_acquire);
        void* actor = LivePropActor(outEid);
        void* inv = actor && IsContainerActor(actor) ? InventoryOf(actor) : nullptr;
        if (s && inv && IsWorldContainerInventory(inv)) {
            BroadcastContainer(s, outEid, inv, static_cast<int>(senderSlot), /*force=*/true);
        }
        // Handled, NOT Applied: this must never be relayed onward. Third peers run no CAS.
        return Ingest::Handled;
    }
    if (o + 2 > blob.size()) return Ingest::Handled;
    const uint16_t n = static_cast<uint16_t>(blob[o] | (blob[o + 1] << 8));
    o += 2;
    if (n > kMaxRecordsPerContainer || !W::Feasible(n, blob, o)) {
        UE_LOGW("container_contents: eid=%u declares %u records -- rejected", outEid, n);
        return Ingest::Handled;
    }
    std::vector<SR::SaveRecord> recs(n);
    for (auto& r : recs) {
        if (!W::DeSave(blob, o, r)) {
            UE_LOGW("container_contents: eid=%u malformed record stream -- dropped", outEid);
            return Ingest::Handled;
        }
    }
    const uint64_t contentHash = ContentHash(outEid, recs);
    const Ingest outcome = ApplyContents(outEid, recs, contentHash);
    // HOST, client-authored + ACCEPTED: this content is now the host's published truth. Recording
    // it here (not at the chunk seam) is what keeps the host's own drain from re-broadcasting the
    // identical slice back out to everyone -- which would reach the author the long way round and
    // stomp whatever it had done since.
    if (outcome == Ingest::Applied && IsHost() && senderSlot != 0) g_sentHash[outEid] = contentHash;
    return outcome;
}

void SweepParked() {
    if (g_parked.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        uint32_t eid = 0;
        // A parked blob is always one a RECEIVER could not resolve yet; the host never parks a
        // client write (a refused one is answered immediately, an accepted one applies at once),
        // so slot 0 is the correct author for every replay here.
        if (ParseAndApply(it->second.blob, eid, /*senderSlot=*/0) != Ingest::Park) {
            it = g_parked.erase(it);
        } else if (now - it->second.at > std::chrono::seconds(kParkTtlSec)) {
            UE_LOGW("container_contents: parked eid=%u expired after %ds unresolved -- dropped",
                    it->first, kParkTtlSec);
            it = g_parked.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- the 0x45 edge ------------------------------------------------------------------------------

// Fires at the ENTRY of addObject / takeObj on the game thread. It carries no arguments by
// design; all it does is remember WHICH component was touched. That is what makes it correct for
// every caller in the firing set -- it is a change NOTICE, not an action.
//
// v125 (R11b): NO ROLE GATE. Whichever peer's verb fired authors that container -- the verb is
// EX_LocalVirtualFunction, so by the time we see it the item has ALREADY moved on this machine.
// An intent that the host could deny cannot exist here; presser-authored state is the only shape
// ([[lesson-presser-authored-state-not-intent-for-invisible-verbs]]). Before v125 this returned
// at IsHost() and the client's every extraction was dropped on the floor: the client took 1 of 2
// burgers, the host's slot stayed at 2, and the world gained a burger.
void OnVerbEntry(const vm::Bracket& br) {
    // EMPIRICAL GATE -- the FIRST statement, ahead of every filter and every role check.
    // "all N verb(s) resolved -- ARMED" proves an FName resolved; it does NOT prove this callback
    // ever runs. R11's two RED takes were exactly that: registration returned true, the banner
    // printed, and the callback was inert for the whole session
    // ([[lesson-late-registrant-inert-after-all-resolved-latch]]). If this line is absent from a
    // smoke log, the lane is dead no matter how green everything else looks.
    static bool sEntered = false;
    if (!sEntered) {
        sEntered = true;
        UE_LOGI("container_contents: 0x45 verb callback ENTERED for the first time on this peer "
                "(role=%s) -- the addObject/takeObj edge is LIVE",
                IsHost() ? "HOST" : "CLIENT");
    }
    if (!br.ctx) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    // The 0x45 filter matches on the VERB NAME alone, so discriminating the Context is OUR job
    // (vm_dispatch.h says exactly that). Without this gate the first non-propInventory ctx to
    // carry an addObject would poison the single offset cache for the whole session.
    if (!IsInventoryComponent(br.ctx)) return;
    void* owner = OwnerOf(br.ctx);
    if (!owner) return;
    const uint32_t eid =
        static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(owner));
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) return;
    g_dirty.insert(eid);   // resolve identity AT THE EDGE; deref nothing later
    // Stamp the LOCAL change so the host can tell a stale client write (an author that had not
    // yet seen the host's own newer change) from a clean one. This is the conflict instrument.
    g_localChangeMs[eid] = NowMs();
    // AND drop what we last APPLIED for it. g_appliedHash is used as "what I already have, so an
    // identical blob is a no-op" -- but that is only true while our state still EQUALS what we
    // applied. The instant our own verb mutates the container the proxy is false, and leaving it
    // in place makes a CORRECTIVE re-publish of the unchanged host truth look like a duplicate and
    // get skipped. Measured: a client whose write the host refused kept its own diverged contents
    // forever, because the host's correction hashed identically to the blob the client had applied
    // before it edited. A peer that mutates locally must be re-appliable.
    g_appliedHash.erase(eid);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    if (!g_verbsRegistered) {
        g_verbsRegistered =
            vm::RegisterVirtualVerb(L"addObject", kVerbDirty, &OnVerbEntry) &&
            vm::RegisterVirtualVerb(L"takeObj",   kVerbDirty, &OnVerbEntry);
        if (!g_verbsRegistered)
            UE_LOGW("container_contents: verb registration FAILED -- the lane is inert");
    }
    vm::TickResolvePending();
    // Own our own enable state rather than free-riding on another consumer's SetEnabled: if
    // kerfur_form_assembler / drive_sync were gated off or retired, registration would still
    // succeed, the banner would still print, and NO callback would ever fire -- silently.
    vm::SetEnabled(true);

    if (!g_announced) {
        g_announced = true;
        UE_LOGI("container_contents: installed (GObjStack slice lane, 0x45 addObject/takeObj edge)");
    }

    const uint64_t now = NowMs();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepMs;

    g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));

    // v125: BOTH peers drain. On the host the drain fans its own changes out; on a client the
    // same drain ships the container IT mutated to the host, which arbitrates and relays. A
    // client still sweeps its parked inbound blobs -- it is both an author and a receiver.
    DrainDirty(s);
    if (!IsHost()) SweepParked();
}

void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // v125 acceptance matrix, checked BEFORE the assembler so no peer can drive reassembly state
    // for a direction it may not send:
    //   HOST   accepts slot != 0 only  -- a client authoring the container it just mutated.
    //                                     Slot 0 would be the host's own broadcast coming back.
    //   CLIENT accepts slot 0 only     -- the host is the arbiter; peers never hear each other
    //                                     directly, so one authority speaks to each client.
    if (IsHost()) {
        if (senderSlot == 0) return;  // our own fan-out echoed back -- not a thing to apply
    } else if (senderSlot != 0) {
        UE_LOGW("container_contents: blob from non-host slot %u on a CLIENT -- REJECTED "
                "(the host is the only authority a client accepts)",
                static_cast<unsigned>(senderSlot));
        return;
    }
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;  // incomplete

    uint32_t eid = 0;
    const Ingest outcome = ParseAndApply(blob, eid, senderSlot);
    if (outcome == Ingest::Park) {
        // Birth skew / mid-activity join: the container's element is not bound yet. Park it
        // (latest wins per eid) and let the sweep retry until the TTL.
        g_parked[eid] = Parked{std::move(blob), std::chrono::steady_clock::now()};
        UE_LOGI("container_contents: eid=%u not resolvable yet -- parked (TTL %ds)", eid, kParkTtlSec);
        return;
    }
    // Relay ONLY what the host genuinely applied. `Handled` covers a CAS-refused write, a malformed
    // one, a non-container eid, a BOUNDARY 1 refusal and a no-op duplicate -- passing any of those
    // on would hand other peers a blob this host has already judged unfit, and they cannot judge it
    // themselves: the arbitration branch is host-only, and a relayed blob reaches them stamped
    // slot 0, i.e. indistinguishable from host truth. Never back to the author (eaten-scroll).
    if (outcome == Ingest::Applied && IsHost() && senderSlot != 0) RelayToOthers(s, senderSlot, blob);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost()) return;

    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    void* base = ContainerClass();
    if (!base) return;

    size_t sent = 0;
    for (const auto& pr : pairs) {
        // IsLiveByIndex, NOT IsLive -- the snapshot does not protect the actor pointer.
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!ue_wrap::prop::WalksToBase(R::ClassOf(pr.actor), base)) continue;
        void* inv = InventoryOf(pr.actor);
        if (!inv || !IsWorldContainerInventory(inv)) continue;   // BOUNDARY 1 (fail-closed)
        if (BroadcastContainer(s, static_cast<uint32_t>(pr.id), inv, peerSlot, /*force=*/true)) ++sent;
    }
    UE_LOGI("container_contents: connect seed -> slot %d: %zu world containers", peerSlot, sent);
}

// ---- dev-instrument seams (see the header) -----------------------------------------------------

size_t SnapshotWorldContainers(WorldContainer* out, size_t want) {
    if (!out || want == 0) return 0;
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    void* base = ContainerClass();
    if (!base) return 0;
    size_t n = 0;
    for (const auto& pr : pairs) {
        if (n >= want) break;
        // IsLiveByIndex, NOT IsLive -- the snapshot does not protect the actor pointer.
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!ue_wrap::prop::WalksToBase(R::ClassOf(pr.actor), base)) continue;
        void* inv = InventoryOf(pr.actor);
        if (!inv || !IsWorldContainerInventory(inv)) continue;   // BOUNDARY 1, the shipped one
        out[n++] = WorldContainer{static_cast<uint32_t>(pr.id), pr.actor, inv};
    }
    return n;
}

bool ContentsDigest(uint32_t eid, int32_t& outCount, float& outVol) {
    outCount = -1;
    outVol = 0.f;
    void* actor = LivePropActor(eid);
    if (!actor || !IsContainerActor(actor)) return false;
    void* inv = InventoryOf(actor);
    if (!inv || !IsWorldContainerInventory(inv)) return false;
    if (uint8_t* slot = GObjStackSlot(inv)) outCount = SR::ReadArr(slot, 0).num;
    static int32_t sOffCurrVol = -2;
    if (CachedOffset(sOffCurrVol, R::ClassOf(inv), L"currVol") >= 0)
        outVol = ReadAt<float>(inv, sOffCurrVol);
    return true;
}

void OnDisconnect() {
    g_dirty.clear();
    g_retry.clear();
    g_localChangeMs.clear();
    g_conflictRejects = 0;
    g_parked.clear();
    g_sentHash.clear();
    g_publishedHash.clear();
    g_baseHash.clear();
    g_appliedHash.clear();
    g_asm.Clear();
    g_nextSweep = 0;
    g_announced = false;
}

}  // namespace coop::props::container_contents_sync
