// coop/props/container_contents_sync.cpp -- world-container contents over the wire: the
// addObject/takeObj verb edge marks a container dirty, a 250 ms sweep ships its GObjStack slice as
// a blob, the receiver writes the slice back and re-derives the volume and names through the
// engine's own verbs; a client's edit is a compare-and-swap the host arbitrates and relays. See
// coop/props/container_contents_sync.h.

#include "coop/props/prop_save_data.h"
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

// Verb ids: addObject marks dirty; takeObj also arms the container-extraction birth latch that
// prop_drop_intent consumes (a client-extracted item's actor is admitted as a host-authoritative
// drop intent).
constexpr int kVerbDirty   = 1;   // addObject
constexpr int kVerbTakeObj = 2;   // takeObj

// The sweep drains an edge-driven set, not a poll; 250 ms coalesces a burst (a loot roll fires
// addObject four times) into one broadcast.
constexpr uint64_t kSweepMs = 250;

// A container with more records than this is not shipped: the blob would approach the transport
// ceiling, and a truncated blob is a silent lie. Real containers hold single digits.
constexpr size_t kMaxRecordsPerContainer = 512;

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_verbsRegistered = false;
bool g_announced = false;
uint64_t g_nextSweep = 0;
uint32_t g_nextSeq = 1;

coop::blob_chunks::Assembler g_asm;

// Containers marked dirty by the verb edge, drained on the next sweep, keyed by element id rather
// than the component pointer: IsLive cannot detect an address recycled by a new object between the
// edge and the drain, and an eid re-resolves forward, so a destroyed container stops resolving.
std::set<uint32_t> g_dirty;

// Host: the content most recently published for an eid by any route, fan-out or a targeted
// connect seed. This is the compare-and-swap baseline ("what did I tell that peer the world looked
// like"); g_sentHash answers a different question ("may I skip the next fan-out"), and a targeted
// send must not answer yes to that one.
std::map<uint32_t, uint64_t> g_publishedHash;

// The hash of the last blob sent and of the last applied, per eid. Skipping an unchanged blob is
// what bounds the orphaned-buffer cost: a steady-state re-broadcast applies and allocates nothing.
std::map<uint32_t, uint64_t> g_sentHash;
std::map<uint32_t, uint64_t> g_appliedHash;

// Client: the last host truth this peer applied for an eid, the base it declares when it authors.
// Not the same map as g_appliedHash, although both are written at the same moment: a local
// mutation clears g_appliedHash (or a corrective re-publish is skipped as a duplicate and the peer
// never converges) and keeps g_baseHash (that is the edit's base; cleared, the peer declares base 0
// and the host refuses every write). Fusing them produced both failures in turn.
std::map<uint32_t, uint64_t> g_baseHash;

// name to UClass memo: FindClass walks the whole GUObjectArray, and per record per broadcast that
// is the per-frame full scan.
std::map<std::wstring, void*> g_classMemo;

// Containers whose broadcast the transport refused; retried by the sweep.
std::set<uint32_t> g_retry;

// This peer's own last verb edge per eid; the host uses it to detect a client write that raced a
// host-side change.
std::map<uint32_t, uint64_t> g_localChangeMs;

// A client write is refused within this window of a host-side change.
constexpr uint64_t kConflictWindowMs = 1500;

// How many client writes the host has refused. There is no rollback: a refused write is corrected
// by the host re-publishing its truth and the loser sees the container snap back; whether that is
// ever noticeable is empirical, and this counter answers it.
uint64_t g_conflictRejects = 0;

// The takeObj-in-flight latch: armed at the verb's entry, consumed by prop_drop_intent at the
// extracted item's FinishSpawn enqueue (the item spawns inside the takeObj call, so the latch is
// live exactly then). Atomic exchange, so the consume is one-shot. A personal-inventory take arms
// it too, harmlessly; the drain's own gates filter those.
std::atomic<bool> g_takeObjInFlight{false};

// Inbound blobs for an eid not yet resolvable (birth skew, a mid-activity join).
struct Parked { std::vector<uint8_t> blob; std::chrono::steady_clock::time_point at; };
std::map<uint32_t, Parked> g_parked;
constexpr int kParkTtlSec = 30;
// Park aging is event-anchored, not wall clock from arrival: a contents slice rides the normal
// lane while its PropSpawn rides bulk, so under backpressure the contents systematically arrive
// first, and a slow link can hold the bulk stream past any fixed TTL with no wire loss. While our
// own join snapshot is in flight parks do not age; at Complete every park is re-stamped and the
// TTL runs from there as a leak guard (Complete is lane-ordered after every PropSpawn it brackets).
bool g_joinBracketOpen = false;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// Reflected offsets, resolved once and cached.

int32_t g_offInvIndex  = -2;  // propInventory_C.Index
int32_t g_offInvPlayer = -2;  // propInventory_C.Player  -- the world-vs-PERSONAL discriminator
int32_t g_offInvOwner  = -2;  // propInventory_C.Owner   -- the Aprop_container_C
int32_t g_offGObjStack = -2;  // saveSlot_C.GObjStack
int32_t g_offPropInv   = -2;  // prop_container_C.propInventory
void* g_containerCls = nullptr;

// The two re-derive verbs, resolved once from the class that declares each, never the instance's
// class (see ResolveRederiveFns).
void* g_fnUpdateVol  = nullptr;   // Aprop_container_C::updateVolumesAndMass
void* g_fnRecalcName = nullptr;   // UpropInventory_C::recalculateNames
bool  g_rederiveResolved = false;

// An offset resolved once; -1 means looked and failed, never retried, never guessed.
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

// Boundary 1, fail closed: true only for a world container this lane may author. Player true is a
// personal inventory (mainPlayer and ui_playerInventory share the same global GObjStack, and
// GObjStack[0] is the local player's inventory by construction, baked by the player container's
// component template), and authoring it from the host would wipe that peer's inventory; an
// unresolvable offset refuses too. The same flag is the address assertion in
// ue_wrap::inventory::ReadLivePersonalStore, fail-closed in the other direction. GObjStackSlot and
// that reader each resolve GObjStack and Index themselves; folding them is a refactor of a shipped
// lane for its own arc.
bool IsWorldContainerInventory(void* inv) {
    if (!inv) return false;
    if (CachedOffset(g_offInvPlayer, R::ClassOf(inv), L"Player") < 0) return false;
    return ReadAt<uint8_t>(inv, g_offInvPlayer) == 0;
}

// The live TArray<Fstruct_save> slot for this component's contents inside the global GObjStack;
// null if the save slot, the offsets or the index do not resolve. The contents array is the
// struct_mObject element's single field, at +0.
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

// A class by name, memoised (a null result too: one walk per name, ever).
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

// Is this a propInventory_C component, and is that a container actor: the verb filter matches on
// the verb name alone, so any class with an addObject would arrive, and the apply side must not
// read a cached component offset off an eid that resolved to something else.
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

// Boundary 2: a nested container's ints[0][0] is its own GObjStack index, a slot number in the
// sender's array that must not survive the wire. Clearing ints[] is wrong: prop_container::loadData
// reads ints[0][0] unguarded, Array_Get zero-fills an out-of-range read, so an empty array yields
// index 0, which propInventory::init's `index >= 0` guard passes, and the container reuses
// GObjStack[0], a slot owned by someone else. The sentinel -1 is what the guard is written against
// (the CDO's default); every other entry is preserved.
void NeuterNestedIndex(SR::SaveRecord& r) {
    if (r.ints.empty()) r.ints.resize(1);
    if (r.ints[0].empty()) r.ints[0].resize(1);
    r.ints[0][0] = -1;
}

// The blob grammar.

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

// baseHash is the last host truth the author had applied for this eid (0 for the host itself, or
// for an author that never applied anything): it lets the host tell "the client edited the world I
// published" from "the client edited a world that has moved on", without which a full-slice write
// from a stale author would silently erase a host addition the author had not received.
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

// The hash every gate and compare-and-swap uses, over a pack with baseHash zeroed, so it names the
// contents alone: the same records hash the same whichever peer authored them and whatever base
// they edited from.
uint64_t ContentHash(uint32_t eid, const std::vector<SR::SaveRecord>& recs) {
    return coop::blob_chunks::Fnv64(PackContents(eid, 0, recs));
}

// Broadcast one container.

// False if the send was refused (the caller arms the retry). Run by both peers: on the host toSlot
// < 0 fans out; on a client the same call reaches the host alone, the author-to-arbiter edge.
// RederiveManagedState is defined below.
void RederiveManagedState(void* owner, void* inv);

bool BroadcastContainer(coop::net::Session* s, uint32_t eid, void* inv, int toSlot, bool force) {
    std::vector<SR::SaveRecord> recs;
    if (!ReadContents(inv, recs)) return true;  // nothing resolvable -- not a transport failure
    // The base being edited from: for a client the last host truth it applied; the host authors
    // from its own state and sends 0.
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
                "(contents stay diverged; there is no bulk path)", eid, blob.size());
        return true;
    }
    const bool ok = (toSlot < 0)
        ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::ContainerContents,
                                      g_nextSeq++, blob)
        : coop::blob_chunks::SendBlobToSlot(s, toSlot, coop::net::ReliableKind::ContainerContents,
                                            g_nextSeq++, blob);
    if (ok) {
        if (toSlot < 0) g_sentHash[eid] = h;  // only a FAN-OUT establishes what every peer has
        // Any publication, fan-out or a targeted seed, establishes what the receiver was told, the
        // baseline a later client write is judged against. With the two maps fused the host refused
        // every client write after a join: the seed is targeted, g_sentHash stayed empty, and the
        // CAS compared against 0 for every container in the world.
        if (IsHost()) g_publishedHash[eid] = h;
        // The author of a mutation re-derives its own volume, mass and names here: the host
        // excludes the author from the relay, and the native take path does not call
        // updateVolumesAndMass, so the mutator's own displayed volume went stale while every other
        // peer converged.
        RederiveManagedState(OwnerOf(inv), inv);
        UE_LOGI("container_contents: eid=%u shipped %zu records (%zu B)%s%s",
                eid, recs.size(), blob.size(),
                toSlot < 0 ? "" : " [targeted]",
                IsHost() ? "" : " [client-authored]");
    }
    return ok;
}

// Host: an accepted client-authored slice on to every other peer, never back to the author:
// echoing a peer's own state reverts a newer local value and primes the baseline over it,
// silently eating that player's next action.
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

// The dirty drain.

void DrainDirty(coop::net::Session* s) {
    // Transport-refusal retries fold into the same set: a refused send re-enters the next sweep
    // rather than re-running the read twice in one tick.
    if (!g_retry.empty()) {
        g_dirty.insert(g_retry.begin(), g_retry.end());
        g_retry.clear();
    }
    if (g_dirty.empty()) return;

    std::set<uint32_t> dirty;
    dirty.swap(g_dirty);

    for (uint32_t eid : dirty) {
        // Resolved forward from the eid every sweep: a container destroyed since the edge stops
        // resolving.
        void* actor = LivePropActor(eid);
        if (!actor || !IsContainerActor(actor)) continue;
        void* inv = InventoryOf(actor);
        if (!inv || !IsWorldContainerInventory(inv)) continue;   // BOUNDARY 1 (fail-closed)
        if (!BroadcastContainer(s, eid, inv, -1, /*force=*/false)) g_retry.insert(eid);
    }
}

// The apply.

// The setter-managed state (currVol, Mass, the display names) is re-derived through the engine's
// own verbs, never raw-written; updateVolumesAndMass calls only Get Volume, and the ejector
// checkObjectsVolume (which calls takeObj) is not called. Resolved from the declaring class:
// FindFunction matches the exact owning class and does not climb the superclass chain, and
// updateVolumesAndMass is declared only on Aprop_container_C while every real container is a
// subclass, so a per-instance-class cache resolved null for every container and the re-derive had
// never run (a null UFunction was silently skipped and the applied volume stayed at whatever the
// last native mutation left). No subclass overrides it, so a base-declared UFunction dispatched
// against a derived instance is correct. The resolution is logged, including failure.
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

// What an inbound blob did to this peer; a bool cannot express it, since "handled" and "changed
// something" are different facts and the relay needs the second. Gated on a bool that was true
// for both an accepted and a refused client write, a refused write was relayed to every other
// client stamped as slot 0, host truth, which peers running no CAS applied unconditionally.
enum class Ingest {
    Park,      // the eid does not resolve yet (birth skew / mid-join) -- park and retry
    Handled,   // dealt with and deliberately NOT applied: refused, malformed, non-container,
               // BOUNDARY 1, or a no-op duplicate. NEVER relayed.
    Applied,   // this peer's state actually changed. The only outcome the host may pass on.
};

// Applied, Handled or Park; see Ingest.
Ingest ApplyContents(uint32_t eid, const std::vector<SR::SaveRecord>& recs, uint64_t blobHash) {
    void* actor = LivePropActor(eid);
    if (!actor) return Ingest::Park;
    // The wire eid must name a container before a cached component offset is read off it.
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

    // The allocator pre-flight: without it an empty array would silently replace real contents.
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
    // The previous buffer is orphaned: freeing the nested sub-arrays, minted FStrings and signal
    // rows recursively is far more crash-prone than leaking them, and the engine never double-frees
    // a buffer it has lost. inventory::ApplyToSaveObject orphans the same way once per join; this
    // lane is steady-state, and the content-hash gate above bounds it, since an apply happens only
    // when the contents changed.
    SR::WriteArrHeader(slot, 0, buf, n);

    g_appliedHash[eid] = blobHash;
    // The base a later local edit declares; never cleared by our own verb edge.
    if (!IsHost()) g_baseHash[eid] = blobHash;
    RederiveManagedState(OwnerOf(inv), inv);
    UE_LOGI("container_contents: eid=%u applied %d records", eid, n);
    return Ingest::Applied;
}

// Host: may this client-authored slice be applied. The compare-and-swap that keeps a stale author
// from erasing a change it never saw; false, and logged, when refused.
bool HostAcceptsClientWrite(uint32_t eid, uint64_t baseHash, uint8_t authorSlot) {
    // An up-to-date author edited from what the host last published; an author that never received
    // anything sends 0 and is refused rather than trusted.
    uint64_t published = 0;
    auto it = g_publishedHash.find(eid);
    if (it != g_publishedHash.end()) published = it->second;

    const bool baseMatches = (baseHash != 0 && baseHash == published);
    bool hostChangeInFlight = false;
    if (baseMatches) {
        // The author edited the published world; still refused if the host changed this container
        // inside the conflict window, a change in flight the author provably had not seen.
        auto lc = g_localChangeMs.find(eid);
        hostChangeInFlight = (lc != g_localChangeMs.end() &&
                              NowMs() - lc->second <= kConflictWindowMs);
        if (!hostChangeInFlight) return true;
    }

    ++g_conflictRejects;
    // The failed condition is named: reported together, a never-published container once read as a
    // racing host change.
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
    // Host arbitration before anything is touched; a refusal is answered by re-publishing the
    // host's truth to the author, so it converges instead of sitting on a divergent view.
    if (IsHost() && senderSlot != 0 && !HostAcceptsClientWrite(outEid, baseHash, senderSlot)) {
        auto* s = g_session.load(std::memory_order_acquire);
        void* actor = LivePropActor(outEid);
        void* inv = actor && IsContainerActor(actor) ? InventoryOf(actor) : nullptr;
        if (s && inv && IsWorldContainerInventory(inv)) {
            BroadcastContainer(s, outEid, inv, static_cast<int>(senderSlot), /*force=*/true);
        }
        // Handled, not Applied: never relayed; third peers run no CAS.
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
    // Host, client-authored and accepted: this content is now the host's published truth, recorded
    // here so the host's own drain does not re-broadcast the identical slice, which would reach the
    // author the long way round and stomp whatever it did since.
    if (outcome == Ingest::Applied && IsHost() && senderSlot != 0) g_sentHash[outEid] = contentHash;
    return outcome;
}

void SweepParked() {
    if (g_parked.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        uint32_t eid = 0;
        // A parked blob is always one a receiver could not resolve; the host never parks a client
        // write, so slot 0 is the author for every replay.
        if (ParseAndApply(it->second.blob, eid, /*senderSlot=*/0) != Ingest::Park) {
            it = g_parked.erase(it);
        } else if (!g_joinBracketOpen &&
                   now - it->second.at > std::chrono::seconds(kParkTtlSec)) {
            UE_LOGW("container_contents: parked eid=%u expired after %ds unresolved -- dropped",
                    it->first, kParkTtlSec);
            it = g_parked.erase(it);
        } else {
            ++it;
        }
    }
}

// The verb edge.

// Fires at the entry of addObject and takeObj on the game thread, a change notice rather than an
// action: it remembers which component was touched. No role gate: the verb is a local virtual
// call, so by the time it is seen the item has already moved on this machine, and an intent the
// host could deny cannot exist here. Gated on the host it dropped every client extraction: the
// client took one of two burgers, the host's slot stayed at two, and the world gained a burger.
void OnVerbEntry(const vm::Bracket& br) {
    // The first statement, ahead of every filter: a resolved verb name does not prove this callback
    // runs, and a registration once returned true with the callback inert for a whole session. If
    // this line is absent from a log, the lane is dead.
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
    // The verb filter matches on the name alone, so the context is discriminated here: the first
    // non-propInventory context carrying an addObject would otherwise poison the offset cache for
    // the session.
    if (!IsInventoryComponent(br.ctx)) return;
    // A takeObj on any inventory component arms the extraction latch; addObject must not.
    if (br.verbId == kVerbTakeObj) g_takeObjInFlight.store(true, std::memory_order_relaxed);
    void* owner = OwnerOf(br.ctx);
    if (!owner) return;
    const uint32_t eid =
        static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(owner));
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) return;
    g_dirty.insert(eid);   // resolve identity AT THE EDGE; deref nothing later
    // The local change is stamped, so the host can tell a stale client write from a clean one.
    g_localChangeMs[eid] = NowMs();
    // And the applied hash is dropped: it means "an identical blob is a no-op" only while our state
    // still equals what we applied, and after our own mutation a corrective re-publish of the
    // unchanged host truth would look like a duplicate; a client whose write was refused once kept
    // its diverged contents forever that way.
    g_appliedHash.erase(eid);
}

}  // namespace

// From event_feed's client-side SnapshotBegin and SnapshotComplete dispatch, on the game thread.
void NoteJoinSnapshotBracket(bool open) {
    if (g_joinBracketOpen == open) return;
    g_joinBracketOpen = open;
    if (!open) {
        const auto now = std::chrono::steady_clock::now();
        for (auto& kv : g_parked) kv.second.at = now;
        if (!g_parked.empty())
            UE_LOGI("container_contents: snapshot bracket closed -- %zu park(s) re-stamped, "
                    "TTL runs from now (leak-guard)", g_parked.size());
    }
}

// Read-and-clear of the takeObj-in-flight latch; prop_drop_intent consumes it at a FinishSpawn
// enqueue to admit that birth, and only that birth, as a host-authoritative drop intent. One-shot
// by exchange; also reached on a task-graph worker, hence atomic.
bool TakeObjInFlight() {
    return g_takeObjInFlight.exchange(false, std::memory_order_relaxed);
}

void Install(coop::net::Session* session) {
    // This lane owns a container's GObjStack slice behind a host-arbitrated compare-and-swap; the
    // container's own save record carries the index into that stack, with none of the arbitration.
    coop::prop_save_data::DeclareClassOwnedElsewhere(L"prop_container_C");
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    if (!g_verbsRegistered) {
        g_verbsRegistered =
            vm::RegisterVirtualVerb(L"addObject", kVerbDirty, &OnVerbEntry) &&
            vm::RegisterVirtualVerb(L"takeObj",   kVerbTakeObj, &OnVerbEntry);
        static bool s_saidFailed = false;
        if (!g_verbsRegistered && !s_saidFailed) {
            // Once. This block retries every tick, and the failure it reports is the substrate
            // refusing permanently (vm_dispatch latches an install failure), so an unlatched
            // line here is a warning per tick for the rest of the session.
            s_saidFailed = true;
            UE_LOGW("container_contents: verb registration FAILED -- the lane is inert");
        }
    }
    vm::TickResolvePending();
    // This lane owns its own enable: riding another consumer's SetEnabled, its retirement would
    // leave the registration green and the callback silent.
    vm::SetEnabled(true);

    if (!g_announced) {
        g_announced = true;
        UE_LOGI("container_contents: installed (GObjStack slice lane, 0x45 addObject/takeObj edge)");
    }

    const uint64_t now = NowMs();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepMs;

    g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));

    // Both peers drain: the host fans its changes out, a client ships the container it mutated to
    // the host, which arbitrates and relays. A client also sweeps its parked inbound blobs.
    DrainDirty(s);
    if (!IsHost()) SweepParked();
}

void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // The acceptance matrix, before the assembler so no peer drives reassembly for a direction it
    // may not send: the host accepts only a non-zero slot (slot 0 is its own fan-out coming back);
    // a client accepts only slot 0, since peers never hear each other directly.
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
        // The container's element is not bound yet: parked (latest wins per eid) and retried by the
        // sweep until the TTL.
        g_parked[eid] = Parked{std::move(blob), std::chrono::steady_clock::now()};
        UE_LOGI("container_contents: eid=%u not resolvable yet -- parked (TTL %ds)", eid, kParkTtlSec);
        return;
    }
    // Only what the host applied is relayed: a refused, malformed, non-container, boundary-refused
    // or duplicate blob would reach the other peers stamped slot 0, host truth they cannot judge.
    // Never back to the author.
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
        // IsLiveByIndex: the snapshot does not protect the actor pointer.
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!ue_wrap::prop::WalksToBase(R::ClassOf(pr.actor), base)) continue;
        void* inv = InventoryOf(pr.actor);
        if (!inv || !IsWorldContainerInventory(inv)) continue;   // BOUNDARY 1 (fail-closed)
        if (BroadcastContainer(s, static_cast<uint32_t>(pr.id), inv, peerSlot, /*force=*/true)) ++sent;
    }
    UE_LOGI("container_contents: connect seed -> slot %d: %zu world containers", peerSlot, sent);
}

// The dev-instrument seams (see the header).

size_t SnapshotWorldContainers(WorldContainer* out, size_t want) {
    if (!out || want == 0) return 0;
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    void* base = ContainerClass();
    if (!base) return 0;
    size_t n = 0;
    for (const auto& pr : pairs) {
        if (n >= want) break;
        // IsLiveByIndex: the snapshot does not protect the actor pointer.
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
    g_joinBracketOpen = false;  // session state must not survive the session
    g_sentHash.clear();
    g_publishedHash.clear();
    g_baseHash.clear();
    g_appliedHash.clear();
    g_asm.Clear();
    g_nextSweep = 0;
    g_announced = false;
}

}  // namespace coop::props::container_contents_sync
