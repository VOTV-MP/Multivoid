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

// Per-eid content hash of the last blob we SENT / APPLIED. Skipping an unchanged blob is what
// keeps the orphaned-buffer cost bounded: a steady-state re-broadcast applies nothing and
// allocates nothing (the raw-write path deliberately orphans the old arrays, so an apply that
// changes nothing must not happen at all).
std::map<uint32_t, uint64_t> g_sentHash;
std::map<uint32_t, uint64_t> g_appliedHash;

// name -> UClass* memo. R::FindClass is a FULL GUObjectArray walk; calling it per record per
// broadcast is the exact per-frame-full-scan pattern the project's post-ship audit rule bans.
std::map<std::wstring, void*> g_classMemo;

// Containers whose broadcast was refused by the transport (I-3) -- retried by the sweep.
std::set<uint32_t> g_retry;

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
std::map<void*, void*> g_fnUpdateVol;   // owner class -> Aprop_container_C::updateVolumesAndMass
std::map<void*, void*> g_fnRecalcName;  // comp class  -> UpropInventory_C::recalculateNames

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

std::vector<uint8_t> PackContents(uint32_t eid, const std::vector<SR::SaveRecord>& recs) {
    std::vector<uint8_t> b;
    b.push_back(kOpContents);
    W::AppU32(b, eid);
    AppU16(b, static_cast<uint16_t>(recs.size()));
    for (const auto& r : recs) W::SerSave(b, r);
    return b;
}

// ---- host: broadcast one container -----------------------------------------------------------

// Returns false if the send was refused (caller arms the retry).
bool BroadcastContainer(coop::net::Session* s, uint32_t eid, void* inv, int toSlot, bool force) {
    std::vector<SR::SaveRecord> recs;
    if (!ReadContents(inv, recs)) return true;  // nothing resolvable -- not a transport failure
    const std::vector<uint8_t> blob = PackContents(eid, recs);
    const uint64_t h = coop::blob_chunks::Fnv64(blob);
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
        UE_LOGI("container_contents: eid=%u shipped %zu records (%zu B)%s",
                eid, recs.size(), blob.size(),
                toSlot < 0 ? "" : " [connect seed]");
    }
    return ok;
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
void CallOn(void* obj, const wchar_t* fnName, std::map<void*, void*>& cache) {
    if (!obj) return;
    void* cls = R::ClassOf(obj);
    if (!cls) return;
    // Keyed BY CLASS: container subclasses (prop_inventoryContainer_drone_C, prop_backpack_C,
    // prop_garbageBin_C, prop_mailbox_C ...) are distinct BP classes, and one global UFunction*
    // would run class A's bytecode against class B's property layout if any of them overrides.
    auto it = cache.find(cls);
    if (it == cache.end()) it = cache.emplace(cls, R::FindFunction(cls, fnName)).first;
    if (it->second) ue_wrap::component_calls::CallParamless(obj, it->second);
}

void RederiveManagedState(void* owner, void* inv) {
    CallOn(owner, L"updateVolumesAndMass", g_fnUpdateVol);
    CallOn(inv,   L"recalculateNames",     g_fnRecalcName);
}

// True if applied; false if the eid could not be resolved (caller parks the blob).
bool ApplyContents(uint32_t eid, const std::vector<SR::SaveRecord>& recs, uint64_t blobHash) {
    void* actor = LivePropActor(eid);
    if (!actor) return false;
    // The wire eid must name a CONTAINER before we read a cached component offset off it.
    if (!IsContainerActor(actor)) {
        UE_LOGW("container_contents: eid=%u does not resolve to a container -- refusing", eid);
        return true;
    }
    void* inv = InventoryOf(actor);
    if (!inv || !IsInventoryComponent(inv)) return false;
    {   // Identical contents -> do nothing. The raw-write orphans the previous arrays, so a
        // no-op apply must not allocate at all (that is what bounds the leak).
        auto it = g_appliedHash.find(eid);
        if (it != g_appliedHash.end() && it->second == blobHash) return true;
    }
    if (!IsWorldContainerInventory(inv)) {           // BOUNDARY 1 (fail-closed)
        UE_LOGW("container_contents: eid=%u resolves to a PERSONAL inventory (or an unresolvable "
                "Player flag) -- refusing to apply", eid);
        return true;                                  // resolved; deliberately not applied
    }
    uint8_t* slot = GObjStackSlot(inv);
    if (!slot) return false;

    // GMalloc pre-flight: without it we would silently write an EMPTY array over real contents.
    if (void* probe = R::EngineAlloc(16)) {
        R::EngineFree(probe);
    } else {
        UE_LOGW("container_contents: eid=%u -- EngineAlloc unavailable; refusing to write", eid);
        return true;
    }

    const int32_t n = static_cast<int32_t>(recs.size());
    void* buf = SR::AllocZeroed(static_cast<size_t>(n), static_cast<size_t>(SR::kSaveStride));
    if (!buf && n > 0) {
        UE_LOGW("container_contents: eid=%u -- alloc of %d records failed; leaving contents", eid, n);
        return true;
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
    RederiveManagedState(OwnerOf(inv), inv);
    UE_LOGI("container_contents: eid=%u applied %d records", eid, n);
    return true;
}

bool ParseAndApply(const std::vector<uint8_t>& blob, uint32_t& outEid) {
    const uint64_t blobHash = coop::blob_chunks::Fnv64(blob);
    size_t o = 0;
    uint8_t op = 0;
    if (!W::RdU8(blob, o, op) || op != kOpContents) return true;  // unknown op -- ignore, not park
    if (!W::RdU32(blob, o, outEid)) return true;
    if (o + 2 > blob.size()) return true;
    const uint16_t n = static_cast<uint16_t>(blob[o] | (blob[o + 1] << 8));
    o += 2;
    if (n > kMaxRecordsPerContainer || !W::Feasible(n, blob, o)) {
        UE_LOGW("container_contents: eid=%u declares %u records -- rejected", outEid, n);
        return true;
    }
    std::vector<SR::SaveRecord> recs(n);
    for (auto& r : recs) {
        if (!W::DeSave(blob, o, r)) {
            UE_LOGW("container_contents: eid=%u malformed record stream -- dropped", outEid);
            return true;
        }
    }
    return ApplyContents(outEid, recs, blobHash);
}

void SweepParked() {
    if (g_parked.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        uint32_t eid = 0;
        if (ParseAndApply(it->second.blob, eid)) {
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
void OnVerbEntry(const vm::Bracket& br) {
    if (!br.ctx) return;
    if (!IsHost()) return;             // clients never author world-container contents
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

    if (IsHost()) {
        DrainDirty(s);
    } else {
        SweepParked();
    }
}

void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // HOST-AUTHORED ONLY, checked BEFORE the assembler so a client cannot drive host-side
    // reassembly state for a kind it may not send. World containers are host-owned; accepting a
    // client blob would let any peer rewrite every container in the world.
    if (senderSlot != 0) {
        UE_LOGW("container_contents: blob from non-host slot %u -- REJECTED",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (IsHost()) return;  // the host never applies its own broadcast
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;  // incomplete

    uint32_t eid = 0;
    if (!ParseAndApply(blob, eid)) {
        // Birth skew / mid-activity join: the container's element is not bound yet. Park it
        // (latest wins per eid) and let the sweep retry until the TTL.
        g_parked[eid] = Parked{std::move(blob), std::chrono::steady_clock::now()};
        UE_LOGI("container_contents: eid=%u not resolvable yet -- parked (TTL %ds)", eid, kParkTtlSec);
    }
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

void OnDisconnect() {
    g_dirty.clear();
    g_retry.clear();
    g_parked.clear();
    g_sentHash.clear();
    g_appliedHash.clear();
    g_asm.Clear();
    g_nextSweep = 0;
    g_announced = false;
}

}  // namespace coop::props::container_contents_sync
