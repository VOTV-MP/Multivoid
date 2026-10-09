// coop/props/container_contents_sync.cpp -- world-container contents over the wire: the
// addObject/takeObj verb edge marks a container dirty, a 250 ms sweep ships its GObjStack slice as
// a blob, the receiver writes the slice back and re-derives the volume and names through the
// engine's own verbs; a client's edit is a compare-and-swap the host arbitrates and relays. See
// coop/props/container_contents_sync.h.

#include "coop/props/prop_save_data.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_park.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_write_policy.h"

#include "coop/element/registry.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace coop::props::container_contents_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;
namespace sg = ue_wrap::script_gate;
namespace wp = coop::props::container_write_policy;
namespace pk = coop::props::container_park;
namespace cw = coop::props::container_slice_wire;
namespace ci = ue_wrap::container_inventory;

using coop::element::LivePropActor;

// Verb ids: addObject marks dirty; takeObj also arms the container-extraction birth latch that
// prop_drop_intent consumes (a client-extracted item's actor is admitted as a host-authoritative
// drop intent).
constexpr int kVerbDirty   = 1;   // addObject
constexpr int kVerbTakeObj = 2;   // takeObj

// The sweep drains an edge-driven set, not a poll; 250 ms coalesces a burst (a loot roll fires
// addObject four times) into one broadcast.
constexpr uint64_t kSweepMs = 250;

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_verbsRegistered = false;
bool g_verbEntered = false;   // the watch has fired at least once on this peer
bool g_announced = false;
uint64_t g_nextSweep = 0;
uint32_t g_nextSeq = 1;

coop::blob_chunks::Assembler g_asm;

// Containers marked dirty by the verb edge, drained on the next sweep, keyed by element id rather
// than the component pointer: IsLive cannot detect an address recycled by a new object between the
// edge and the drain, and an eid re-resolves forward, so a destroyed container stops resolving.
std::set<uint32_t> g_dirty;

// The hash of the last blob sent and of the last applied, per eid. Skipping an unchanged blob is
// what bounds the orphaned-buffer cost: a steady-state re-broadcast applies and allocates nothing.
std::map<uint32_t, uint64_t> g_sentHash;
std::map<uint32_t, uint64_t> g_appliedHash;

// Client: the content this peer believes the host now publishes for an eid -- host truth it
// applied, or its own slice once the transport took it, since an accepted client write becomes
// exactly what the host publishes next. Not the same map as g_appliedHash, although both are
// written at the same moment: a local mutation clears g_appliedHash (or a corrective re-publish is
// skipped as a duplicate and the peer never converges) and keeps g_baseHash (that is the edit's
// base; cleared, the peer declares base 0 and the host refuses every write). Fusing them produced
// both failures in turn.
std::map<uint32_t, uint64_t> g_baseHash;

// Containers whose broadcast the transport refused; retried by the sweep.
std::set<uint32_t> g_retry;

// The takeObj-in-flight latch: armed at the verb's entry, consumed by prop_drop_intent at the
// extracted item's FinishSpawn enqueue (the item spawns inside the takeObj call, so the latch is
// live exactly then). Atomic exchange, so the consume is one-shot. A personal-inventory take arms
// it too, harmlessly; the drain's own gates filter those.
std::atomic<bool> g_takeObjInFlight{false};

// Births: a container thrown into the world, whose contents no verb marked. A client holds the actor
// it threw until the host's echo binds it, then ships its slice as a birth. The host's copy, built
// empty from the intent, is a transfer in progress until the author's slice lands: it is not
// published, and every slice of that author for it is the newest version of the transfer. Both
// sides expire, so a lost intent leaves nothing behind.
constexpr uint64_t kBirthTtlMs = 30000;
constexpr size_t   kMaxBirths  = 64;
// The actor is held across ticks, so it is a CachedObjRef: a successor in the same slot at the same
// address reads as dead rather than as the thrown container.
struct Birth {
    ue_wrap::CachedObjRef actor;
    uint8_t  authorSlot = 0;   // host: the author it awaits
    uint64_t deadlineMs = 0;
};
std::vector<Birth> g_authoredBirths;   // client
std::set<uint32_t> g_birthEids;        // client: bound births whose slice is owed
std::vector<Birth> g_awaitedBirths;    // host
// A first publication owed for an eid -- a client's birth, the host's own thrown container -- whose
// slot may not be readable yet: an unreadable read is retried until the deadline instead of being
// taken as nothing to send.
std::map<uint32_t, uint64_t> g_owedFirst;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// BOUNDARY 1, fail closed: only a world container's own inventory is this lane's to author
// (ci::IsWorldInventory); authoring a personal inventory from the host would wipe that peer's own.
// BOUNDARY 2: a nested container's ints[0][0] is its own GObjStack index, a slot number in the
// sender's array that must not survive the wire, so it is shipped and written as -1
// (ci::ClearInventoryIndex says why never by clearing ints).
bool ReadContents(void* inv, std::vector<SR::SaveRecord>& out, bool neuterNested = true) {
    int32_t count = -1;
    if (!ci::ReadContents(inv, out, cw::kMaxRecords, &count)) {
        if (count > static_cast<int32_t>(cw::kMaxRecords))
            UE_LOGW("container_contents: %d records exceeds the %zu cap -- refusing to ship a "
                    "truncated slice", count, cw::kMaxRecords);
        return false;
    }
    if (neuterNested)
        for (auto& r : out)
            if (ci::IsContainerRecord(r)) ci::ClearInventoryIndex(r);
    return true;
}

// Broadcast one container.

// False if the send was refused (the caller arms the retry). Run by both peers: on the host toSlot
// < 0 fans out; on a client the same call reaches the host alone, the author-to-arbiter edge.

bool BroadcastContainer(coop::net::Session* s, uint32_t eid, void* inv, int toSlot, bool force) {
    std::vector<SR::SaveRecord> recs;
    if (!ReadContents(inv, recs)) {
        // Nothing resolvable is not a transport failure -- unless a first publication is owed for this
        // eid, which a slot not readable yet would otherwise lose for good.
        auto it = g_owedFirst.find(eid);
        if (it == g_owedFirst.end()) return true;
        if (NowMs() < it->second) return false;
        g_owedFirst.erase(it);
        g_birthEids.erase(eid);
        UE_LOGW("container_contents: eid=%u -- its contents never became readable; the first "
                "publication is given up", eid);
        return true;
    }
    // The base being edited from: for a client the last host truth it applied; the host authors
    // from its own state and sends 0.
    uint64_t baseHash = 0;
    const bool birth = !IsHost() && toSlot < 0 && g_birthEids.count(eid) != 0;
    if (birth) {
        baseHash = cw::kBirthBase;
    } else if (!IsHost()) {
        auto it = g_baseHash.find(eid);
        if (it != g_baseHash.end()) baseHash = it->second;
    }
    const std::vector<uint8_t> blob = cw::Pack(eid, baseHash, recs);
    const uint64_t h = cw::ContentHash(eid, recs);
    if (!force && !birth) {
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
        if (birth) g_birthEids.erase(eid);
        if (toSlot < 0) g_owedFirst.erase(eid);
        if (toSlot < 0) g_sentHash[eid] = h;  // only a FAN-OUT establishes what every peer has
        // A client's own accepted slice IS the host's next published truth, and the author is
        // deliberately excluded from the relay that carries it -- so it advances its base here.
        // Optimistic and self-correcting: a refusal is answered by the host re-publishing its
        // truth to this peer, which lands in this same map.
        if (!IsHost()) g_baseHash[eid] = h;
        // Any publication, fan-out or a targeted seed, establishes what the receiver was told, the
        // baseline a later client write is judged against. With the two maps fused the host refused
        // every client write after a join: the seed is targeted, g_sentHash stayed empty, and the
        // CAS compared against 0 for every container in the world.
        if (IsHost()) wp::NotePublished(eid, h);
        // The author of a mutation re-derives its own volume, mass and names here: the host
        // excludes the author from the relay, and the native take path does not call
        // updateVolumesAndMass, so the mutator's own displayed volume went stale while every other
        // peer converged. A targeted send -- a joiner's seed, the truth re-sent to a refused
        // author -- changed nothing on this peer, so it re-derives nothing; the seed sends every
        // container in the world in one frame.
        if (toSlot < 0) ci::RederiveShownState(ci::OwnerOf(inv), inv);
        UE_LOGI("container_contents: eid=%u shipped %zu records (%zu B)%s%s%s",
                eid, recs.size(), blob.size(),
                toSlot < 0 ? "" : " [targeted]",
                IsHost() ? "" : " [client-authored]",
                birth ? " [birth]" : "");
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

// The awaited birth for this eid, from this author or (authorSlot 0) from any, or null. The host never
// publishes an awaited container, so nothing a slice could have been based on exists yet.
Birth* AwaitedBirth(uint32_t eid, uint8_t authorSlot) {
    void* actor = LivePropActor(eid);
    if (!actor) return nullptr;
    const uint64_t now = NowMs();
    for (Birth& b : g_awaitedBirths)
        if (b.actor.Is(actor) && (authorSlot == 0 || b.authorSlot == authorSlot) && now < b.deadlineMs)
            return &b;
    return nullptr;
}

void DropAwaitedBirth(uint32_t eid) {
    void* actor = LivePropActor(eid);
    if (!actor) return;
    for (size_t i = 0; i < g_awaitedBirths.size(); ++i) {
        if (!g_awaitedBirths[i].actor.Is(actor)) continue;
        g_awaitedBirths.erase(g_awaitedBirths.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

// One more birth, the oldest given up (and said) past the cap: throws are human-rate, so the cap
// only bounds a peer that floods.
void PushBirth(std::vector<Birth>& births, void* actor, uint8_t authorSlot) {
    if (births.size() >= kMaxBirths) {
        UE_LOGW("container_contents: %zu container transfers in flight -- the oldest is given up",
                births.size());
        births.erase(births.begin());
    }
    Birth b;
    b.actor.Set(actor);
    b.authorSlot = authorSlot;
    b.deadlineMs = NowMs() + kBirthTtlMs;
    births.push_back(std::move(b));
}

// Client: a thrown container the host's echo has bound is marked, its slice owed as a birth. Host:
// an awaited birth that never came is dropped, and said.
void SweepBirths(uint64_t now) {
    for (size_t i = 0; i < g_authoredBirths.size();) {
        const Birth& b = g_authoredBirths[i];
        if (!b.actor.Alive() || now >= b.deadlineMs) {
            g_authoredBirths.erase(g_authoredBirths.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const auto eid = coop::element::Registry::Get().EidForActor(b.actor.Get());
        if (eid == coop::element::kInvalidId) { ++i; continue; }
        g_birthEids.insert(static_cast<uint32_t>(eid));
        g_owedFirst[static_cast<uint32_t>(eid)] = b.deadlineMs;
        g_dirty.insert(static_cast<uint32_t>(eid));
        g_authoredBirths.erase(g_authoredBirths.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (size_t i = 0; i < g_awaitedBirths.size();) {
        const Birth& b = g_awaitedBirths[i];
        const bool live = b.actor.Alive();
        if (live && now < b.deadlineMs) { ++i; continue; }
        if (live)
            UE_LOGW("container_contents: the contents of a container slot %u threw never came -- it "
                    "stays as the host built it", static_cast<unsigned>(b.authorSlot));
        g_awaitedBirths.erase(g_awaitedBirths.begin() + static_cast<std::ptrdiff_t>(i));
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
        if (!actor) {   // gone: nothing is owed for it any more
            g_owedFirst.erase(eid);
            g_birthEids.erase(eid);
            continue;
        }
        if (!ci::IsContainer(actor)) continue;
        void* inv = ci::InventoryOf(actor);
        if (!inv || !ci::IsWorldInventory(inv)) {   // BOUNDARY 1 (fail-closed)
            // A first publication owed for it waits for the component instead of ending here.
            auto owed = g_owedFirst.find(eid);
            if (owed != g_owedFirst.end() && NowMs() < owed->second) g_retry.insert(eid);
            continue;
        }
        // A transfer in progress is not published: what the host added to it so far merges into the
        // author's slice when it lands, and is published with it.
        if (IsHost() && AwaitedBirth(eid, 0)) { g_retry.insert(eid); continue; }
        if (!BroadcastContainer(s, eid, inv, -1, /*force=*/false)) g_retry.insert(eid);
    }
}

// The apply.

// The setter-managed state (currVol, Mass, the display names) is re-derived through the engine's
// own verbs, never raw-written (ci::RederiveShownState).

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
    if (!ci::IsContainer(actor)) {
        UE_LOGW("container_contents: eid=%u does not resolve to a container -- refusing", eid);
        return Ingest::Handled;
    }
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsInventory(inv)) return Ingest::Park;
    {   // Identical contents -> do nothing. The raw-write orphans the previous arrays, so a
        // no-op apply must not allocate at all (that is what bounds the leak).
        auto it = g_appliedHash.find(eid);
        if (it != g_appliedHash.end() && it->second == blobHash) return Ingest::Handled;
    }
    if (!ci::IsWorldInventory(inv)) {           // BOUNDARY 1 (fail-closed)
        UE_LOGW("container_contents: eid=%u resolves to a PERSONAL inventory (or an unresolvable "
                "Player flag) -- refusing to apply", eid);
        return Ingest::Handled;                       // resolved; deliberately not applied
    }
    uint8_t* slot = ci::ContentsSlot(inv);
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
    ci::RederiveShownState(ci::OwnerOf(inv), inv);
    UE_LOGI("container_contents: eid=%u applied %d records", eid, n);
    return Ingest::Applied;
}

Ingest ParseAndApply(const std::vector<uint8_t>& blob, uint32_t& outEid, uint8_t senderSlot,
                     wp::Source src) {
    size_t o = 0;
    uint64_t baseHash = 0;
    if (!cw::ParseHeader(blob, o, outEid, baseHash)) return Ingest::Handled;
    // A container a client threw: the host built its copy empty from the intent and has published
    // nothing for it, so any slice of that author is the newest version of the transfer, whatever
    // base it names -- an edit made while the first slice waited in the pen included. There is no
    // reach to measure either: the throw put it where it is.
    const bool birth = IsHost() && senderSlot != 0 && AwaitedBirth(outEid, senderSlot) != nullptr;
    // Unjudged is not unbounded: a birth slice spends the same arrival budget an edit does, so an
    // author cannot make the host parse slices for this container at any rate while it waits.
    if (birth) {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!s || wp::SpendArrival(outEid, senderSlot, NowMs(), *s, src) == wp::Decision::TooFast)
            return Ingest::Handled;
    }
    // Host arbitration before anything is touched; a refusal is answered by re-publishing the
    // host's truth to the author, so it converges instead of sitting on a divergent view.
    if (IsHost() && senderSlot != 0 && !birth) {
        auto* s = g_session.load(std::memory_order_acquire);
        // No session, no arbitration, and a client slice is never applied unjudged: the refusal
        // that cannot be explained is still a refusal.
        const wp::Decision d = s ? wp::Accept(outEid, baseHash, senderSlot, NowMs(), *s, src)
                                 : wp::Decision::StaleBase;
        // No body to measure the author against yet: HOLD, do not refuse. The pen replays it and
        // the reach is judged the moment that peer has posed; the hold is bounded per author and
        // by the TTL, so a peer that never poses costs the host eight slices for thirty seconds.
        if (d == wp::Decision::NoBodyYet) return Ingest::Park;
        if (d != wp::Decision::Accept) {
            // Every refusal answers with the host's truth so the author converges -- except the
            // rate one, because a bound on how fast an author may spend the host must not make
            // the host spend more the faster it is pushed. No peer of this build can reach that
            // bound (one slice per container per 250 ms sweep, and only when it changed).
            void* actor = LivePropActor(outEid);
            void* inv = actor && ci::IsContainer(actor) ? ci::InventoryOf(actor) : nullptr;
            if (s && inv && d != wp::Decision::TooFast && ci::IsWorldInventory(inv)) {
                BroadcastContainer(s, outEid, inv, static_cast<int>(senderSlot), /*force=*/true);
            }
            // Handled, not Applied: never relayed; third peers run no arbitration.
            return Ingest::Handled;
        }
    }
    std::vector<SR::SaveRecord> recs;
    const char* why = "";
    if (!cw::ParseRecords(blob, o, recs, &why)) {
        UE_LOGW("container_contents: eid=%u -- %s; dropped", outEid, why);
        return Ingest::Handled;
    }
    // BOUNDARY 2, the INBOUND half. The send side neuters a nested container's own GObjStack index
    // because it names a slot in the SENDER's array; enforcing that outbound alone trusts every
    // sender to be this build. Written through unchanged, prop_container::loadData reads ints[0][0]
    // unguarded and the nested container would bind whatever sits in that slot on THIS machine --
    // another container's contents, or a player's inventory.
    size_t foreignIndices = 0;
    for (auto& r : recs) {
        if (!ci::IsContainerRecord(r)) continue;
        if (ci::CarriesInventoryIndex(r)) ++foreignIndices;
        ci::ClearInventoryIndex(r);
    }
    if (foreignIndices) {
        // A hit is a peer that is not this build, or a second producer that skipped the boundary.
        UE_LOGW("container_contents: eid=%u -- %zu nested-container record(s) arrived carrying a "
                "foreign GObjStack index; neutered before the write", outEid, foreignIndices);
    }
    // What the host put into its copy while the transfer was in flight. That copy was born empty, so
    // all of it is the host's own and is kept beside the author's: replacing it would lose the host's
    // item, refusing the slice would lose the author's. Read raw: these records stay in this array.
    bool merged = false;
    if (birth) {
        void* actor = LivePropActor(outEid);
        void* inv = actor && ci::IsContainer(actor) ? ci::InventoryOf(actor) : nullptr;
        std::vector<SR::SaveRecord> hostRecs;
        if (inv && ReadContents(inv, hostRecs, /*neuterNested=*/false) && !hostRecs.empty()) {
            if (recs.size() + hostRecs.size() <= cw::kMaxRecords) {
                recs.insert(recs.end(), hostRecs.begin(), hostRecs.end());
                merged = true;
            } else {
                // Past the slice cap the two cannot travel as one. The thrower's half is the only copy
                // of its contents; the host's was added within the transfer's own round trip, and no
                // other peer has seen it: the thrower's slice is applied alone, and every peer
                // converges on it.
                UE_LOGW("container_contents: eid=%u -- the host's %zu record(s) do not fit beside the "
                        "thrower's %zu under the %zu cap; the thrower's slice is applied alone and the "
                        "host's records are dropped", outEid, hostRecs.size(), recs.size(), cw::kMaxRecords);
            }
        }
    }
    const uint64_t contentHash = cw::ContentHash(outEid, recs);
    const Ingest outcome = ApplyContents(outEid, recs, contentHash);
    // Host, client-authored and accepted. Two records, and the second one was missing: g_sentHash
    // keeps the host's own drain from re-broadcasting the identical slice back the long way round,
    // while NotePublished moves the compare-and-swap baseline onto what the relay is about to hand
    // every other peer. Without it the baseline stayed at the last HOST fan-out while the receivers
    // moved on, and the second peer to edit a container was refused for being as up to date as the
    // first.
    if (outcome == Ingest::Applied && IsHost() && senderSlot != 0) {
        if (birth) DropAwaitedBirth(outEid);
        if (merged) {
            // The author lacks the host's part: the merged contents go to every peer, the author
            // included, by the host's own fan-out on the next sweep.
            g_sentHash.erase(outEid);
            g_dirty.insert(outEid);
            UE_LOGI("container_contents: eid=%u slot %u transfer MERGED with the host's own records -- "
                    "published to all on the next sweep", outEid, static_cast<unsigned>(senderSlot));
            return outcome;
        }
        g_sentHash[outEid] = contentHash;
        wp::NotePublished(outEid, contentHash);
        UE_LOGI("container_contents: eid=%u slot %u ACCEPTED -- the published baseline is now "
                "%llu, which is what the relay carries", outEid, static_cast<unsigned>(senderSlot),
                static_cast<unsigned long long>(contentHash));
        // The relay belongs HERE and not at the arrival site: a slice that waited in the pen and
        // applied on a replay is the host's new truth just as much as one that applied on arrival,
        // and relaying only the second kind left every other peer without it, silently, while the
        // baseline above told the host they had it.
        if (auto* s = g_session.load(std::memory_order_acquire))
            RelayToOthers(s, senderSlot, blob);
    }
    return outcome;
}

// The park's replay: true when the blob was dealt with, false while its container still does not
// resolve. The author rides with the blob because a parked slice must pass the SAME arbitration it
// would have passed on arrival -- replaying a client's write as slot 0 would hand it the host's
// own authority, and the host does park client writes (every inbound blob it cannot resolve yet).
bool ReplayParked(const std::vector<uint8_t>& blob, uint8_t authorSlot) {
    uint32_t eid = 0;
    return ParseAndApply(blob, eid, authorSlot, wp::Source::Replay) != Ingest::Park;
}

// The verb edge.

// Fires at the entry of addObject and takeObj on the game thread, a change notice rather than an
// action: it remembers which component was touched. No role gate: the verb is a local virtual
// call, so by the time it is seen the item has already moved on this machine, and an intent the
// host could deny cannot exist here. Gated on the host it dropped every client extraction: the
// client took one of two burgers, the host's slot stayed at two, and the world gained a burger.
sg::Verdict OnVerbEntry(const sg::Call& br) {
    // The first statement, ahead of every filter: a resolved verb name does not prove this callback
    // runs, and a registration once returned true with the callback inert for a whole session. If
    // this line is absent from a log, the lane is dead.
    if (!g_verbEntered) {
        g_verbEntered = true;
        UE_LOGI("container_contents: the verb watch ENTERED for the first time on this peer "
                "(role=%s) -- the addObject/takeObj edge is LIVE",
                IsHost() ? "HOST" : "CLIENT");
    }
    if (!br.object) return sg::Verdict::Run;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    // The verb filter matches on the name alone, so the context is discriminated here: the first
    // non-propInventory context carrying an addObject would otherwise poison the offset cache for
    // the session.
    if (!ci::IsInventory(br.object)) return sg::Verdict::Run;
    // A takeObj on any inventory component arms the extraction latch; addObject must not.
    if (br.tag == kVerbTakeObj) g_takeObjInFlight.store(true, std::memory_order_relaxed);
    void* owner = ci::OwnerOf(br.object);
    if (!owner) return sg::Verdict::Run;
    const uint32_t eid =
        static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(owner));
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) return sg::Verdict::Run;
    g_dirty.insert(eid);   // resolve identity AT THE EDGE; deref nothing later
    // The local change is stamped, so the host can tell a stale client write from a clean one.
    // Host-only: nothing on a client ever reads it, and an ungated stamp grows a row per container
    // that peer has ever touched.
    if (IsHost()) wp::NoteLocalChange(eid, NowMs());
    // And the applied hash is dropped: it means "an identical blob is a no-op" only while our state
    // still equals what we applied, and after our own mutation a corrective re-publish of the
    // unchanged host truth would look like a duplicate; a client whose write was refused once kept
    // its diverged contents forever that way.
    g_appliedHash.erase(eid);
    return sg::Verdict::Run;
}

}  // namespace

// From event_feed's client-side SnapshotBegin and SnapshotComplete dispatch, on the game thread.
void NoteJoinSnapshotBracket(bool open) { pk::NoteJoinBracket(open, NowMs()); }

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
            sg::WatchName(L"addObject", kVerbDirty, &OnVerbEntry, nullptr) &&
            sg::WatchName(L"takeObj",   kVerbTakeObj, &OnVerbEntry, nullptr);
        static bool s_saidFailed = false;
        if (!g_verbsRegistered && !s_saidFailed) {
            // Once. This block retries every tick, and the failure it reports is the gate
            // refusing permanently (a full table, or no detour), so an unlatched line here is a
            // warning per tick for the rest of the session.
            s_saidFailed = true;
            UE_LOGW("container_contents: verb registration FAILED -- the lane is inert");
        }
    }
    sg::ResolvePendingNames();

    if (!g_announced) {
        g_announced = true;
        UE_LOGI("container_contents: installed (GObjStack slice lane, the addObject/takeObj watch)");
    }

    const uint64_t now = NowMs();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepMs;

    g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));

    // Both peers drain: the host fans its changes out, a client ships the container it mutated to
    // the host, which arbitrates and relays. Both sweep their parked inbound blobs -- the host's
    // were never swept at all, so a client write it could not resolve on arrival was neither
    // retried nor evicted for the life of the session.
    SweepBirths(now);
    DrainDirty(s);
    pk::Sweep(&ReplayParked, NowMs());
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
    const Ingest outcome = ParseAndApply(blob, eid, senderSlot, wp::Source::Arrival);
    if (outcome == Ingest::Park) {
        // The container's element is not bound yet: parked (latest wins per eid) and retried by the
        // sweep until the TTL.
        pk::Admit(eid, senderSlot, std::move(blob), NowMs());
        return;
    }
}

void NoteAuthoredBirth(void* actor) {
    if (IsHost() || !ci::IsContainer(actor)) return;
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsWorldInventory(inv)) {
        UE_LOGW("container_contents: CLIENT threw container %p, but its inventory %s -- its contents "
                "stay on this peer only", actor, inv ? "is not a world container's" : "does not resolve");
        return;
    }
    PushBirth(g_authoredBirths, actor, 0);
    UE_LOGI("container_contents: CLIENT threw container %p -- its contents go to the host once bound", actor);
}

void ExpectBirthSlice(void* actor, uint8_t authorSlot) {
    if (!IsHost() || !ci::IsContainer(actor)) return;
    PushBirth(g_awaitedBirths, actor, authorSlot);
}

void NoteHostBirth(void* actor) {
    if (!IsHost() || !ci::IsContainer(actor)) return;
    for (const Birth& b : g_awaitedBirths)
        if (b.actor.Is(actor)) return;   // built from a client's intent: its author holds the contents
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsWorldInventory(inv)) return;
    const auto eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId) return;
    std::vector<SR::SaveRecord> recs;
    const bool readable = ReadContents(inv, recs);
    if (readable && recs.empty()) return;   // the mirrors are born empty too
    // Not readable yet is not empty: the publication is owed and retried until it is.
    if (!readable) g_owedFirst[static_cast<uint32_t>(eid)] = NowMs() + kBirthTtlMs;
    g_dirty.insert(static_cast<uint32_t>(eid));
}

void OnPeerGone(uint8_t slot) {
    // A transfer is bound to the peer that threw: a later occupant of the slot does not complete it.
    for (size_t i = 0; i < g_awaitedBirths.size();) {
        if (g_awaitedBirths[i].authorSlot != slot) { ++i; continue; }
        UE_LOGW("container_contents: slot %u left before the contents of a container it threw came",
                static_cast<unsigned>(slot));
        g_awaitedBirths.erase(g_awaitedBirths.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost()) return;

    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    size_t sent = 0;
    for (const auto& pr : pairs) {
        // IsLiveByIndex: the snapshot does not protect the actor pointer.
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!ci::IsContainer(pr.actor)) continue;
        void* inv = ci::InventoryOf(pr.actor);
        if (!inv || !ci::IsWorldInventory(inv)) continue;   // BOUNDARY 1 (fail-closed)
        // A transfer in progress reaches the joiner with the fan-out that completes it.
        if (AwaitedBirth(static_cast<uint32_t>(pr.id), 0)) continue;
        if (BroadcastContainer(s, static_cast<uint32_t>(pr.id), inv, peerSlot, /*force=*/true)) ++sent;
    }
    UE_LOGI("container_contents: connect seed -> slot %d: %zu world containers", peerSlot, sent);
}

// The dev-instrument seams (see the header).

size_t SnapshotWorldContainers(WorldContainer* out, size_t want) {
    if (!out || want == 0) return 0;
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    size_t n = 0;
    for (const auto& pr : pairs) {
        if (n >= want) break;
        // IsLiveByIndex: the snapshot does not protect the actor pointer.
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!ci::IsContainer(pr.actor)) continue;
        void* inv = ci::InventoryOf(pr.actor);
        if (!inv || !ci::IsWorldInventory(inv)) continue;   // BOUNDARY 1, the shipped one
        out[n++] = WorldContainer{static_cast<uint32_t>(pr.id), pr.actor, inv};
    }
    return n;
}

bool VerbWatchEntered() { return g_verbEntered; }

bool ContentsDigest(uint32_t eid, int32_t& outCount, float& outVol) {
    outCount = -1;
    outVol = 0.f;
    void* actor = LivePropActor(eid);
    if (!actor || !ci::IsContainer(actor)) return false;
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsWorldInventory(inv)) return false;
    if (uint8_t* slot = ci::ContentsSlot(inv)) outCount = SR::ReadArr(slot, 0).num;
    ci::CurrentVolume(inv, outVol);
    return true;
}

void OnDisconnect() {
    g_dirty.clear();
    g_retry.clear();
    wp::Reset();
    pk::Reset();
    g_sentHash.clear();
    g_baseHash.clear();
    g_appliedHash.clear();
    g_authoredBirths.clear();
    g_birthEids.clear();
    g_awaitedBirths.clear();
    g_owedFirst.clear();
    g_asm.Clear();
    g_nextSweep = 0;
    g_announced = false;
    g_verbEntered = false;
}

}  // namespace coop::props::container_contents_sync
