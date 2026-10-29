// coop/props/container_contents_sync.cpp -- world-container contents over the wire: a write to a
// container (the writer watches, coop/props/container_writer_watch) marks it dirty, a 250 ms
// sweep ships its GObjStack slice as a blob, the receiver writes the slice back and re-derives the volume and names
// through the engine's own verbs. The host publishes; a client ships only a thrown container's birth slice, and the
// host refuses every other client slice. See
// coop/props/container_contents_sync.h. The host-to-peers half (the dirty drain, the broadcast, the relay) is
// container_contents_publish.cpp.

#include "coop/props/prop_save_data.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_contents_internal.h"
#include "coop/props/container_apply_hold.h"
#include "coop/props/container_arming.h"
#include "coop/props/container_birth.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_park.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_truth.h"
#include "coop/props/container_writer_watch.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_route_census.h"  // the census counts the slices a route sends
#include "coop/element/bind_wait.h"
#include "coop/element/registry.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"  // ClassByName -- the host's O(1) class resolve for a client's contents
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace coop::props::container_contents_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;
namespace sg = ue_wrap::script_gate;
namespace pk = coop::props::container_park;
namespace cw = coop::props::container_slice_wire;
namespace ci = ue_wrap::container_inventory;
namespace cb = coop::props::container_birth;
namespace cio = coop::props::container_contents_io;
namespace cww = coop::props::container_writer_watch;
namespace ct  = coop::props::container_truth;
namespace ca  = coop::props::container_arming;

using coop::element::LivePropActor;

// The sweep drains an edge-driven set, not a poll; 250 ms coalesces a burst (a loot roll fires
// addObject four times) into one broadcast.
constexpr uint64_t kSweepMs = 250;

constexpr uint16_t kContainerContents = static_cast<uint16_t>(coop::net::ReliableKind::ContainerContents);

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_announced = false;
uint64_t g_nextSweep = 0;

coop::blob_chunks::Assembler g_asm;

std::set<uint32_t> g_wrongClassSaid;  // eids a slice named that are no container here (said once a session)

// What was last applied, per eid: the hash of the records and the flags apart. Skipping an unchanged blob is what bounds the
// orphaned-buffer cost: a steady-state re-apply allocates nothing. A change of the container's arming with the same records
// is applied to the mirror without a write of its records.
using Seen = internal::Seen;
std::map<uint32_t, Seen> g_applied;

// Dev instrument: per container, how often this lane touched it (a local write, an applied remote slice, a
// connect seed's send). The writer probe reads it to tell a change the lane saw from one it did not; kept only
// while that probe is armed. Reset at session end.
std::map<uint32_t, uint32_t> g_touchEpoch;
// Containers whose accepted slice re-read to another hash, said once each. Reset at session end.
std::set<uint32_t> g_rereadSaid;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}


// The apply.

// The setter-managed state (currVol, Mass, the display names) is re-derived through the engine's
// own verbs, never raw-written (ci::RederiveShownState).

// What an inbound blob did to this peer; a bool cannot express it, since "handled" and "changed
// something" are different facts and the relay needs the second: a refused write must never be relayed.
enum class Ingest {
    NoActor,   // the eid has no live actor here yet (birth skew / mid-join) -- hold in bind_wait for its bind
    Park,      // the actor is here but not ready (inventory, slice slot, author's body, budget) -- the pen
    Handled,   // dealt with and deliberately NOT applied: refused, malformed, non-container,
               // BOUNDARY 1, or a no-op duplicate. NEVER relayed.
    Applied,   // this peer's state actually changed. The only outcome the host may pass on.
};

// Dev: the touch epoch is kept only while the writer probe is armed.
void Touch(uint32_t eid) {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::container_writer_probe);
    if (on) ++g_touchEpoch[eid];
}

// Applied, Handled, NoActor or Park; see Ingest. `recordsHash` is cw::RecordsHash of `recs`. `flags`, when given, is the
// slice's flags byte as sent: a client takes the container's box from it (container_arming), the host never does, since
// its own reading is the truth; a caller with no flags to give leaves the box as it is.
Ingest ApplyContents(uint32_t eid, const std::vector<SR::SaveRecord>& recs, uint64_t recordsHash,
                     std::optional<uint8_t> flags) {
    void* actor = LivePropActor(eid);
    if (!actor) return Ingest::NoActor;
    // The wire eid must name a container before a cached component offset is read off it.
    if (!ci::IsContainer(actor)) {
        if (g_wrongClassSaid.insert(eid).second)
            UE_LOGW("container_contents: eid=%u does not resolve to a container -- refusing", eid);
        return Ingest::Handled;
    }
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsInventory(inv)) return Ingest::Park;
    const bool boxDue = flags.has_value() && !IsHost();
    const uint8_t want = flags ? static_cast<uint8_t>(*flags & cw::kKnownFlags) : uint8_t{0};
    {   // Identical contents -> do nothing. The raw-write orphans the previous arrays, so a
        // no-op apply must not allocate at all (that is what bounds the leak). The same records with other flags
        // apply the flags alone: an arming edge with no record change always reaches the mirror.
        const auto it = g_applied.find(eid);
        if (it != g_applied.end() && it->second.records == recordsHash) {
            if (!boxDue || it->second.flags == want) return Ingest::Handled;
            ca::ApplyFlags(eid, actor, want);
            it->second.flags = want;
            Touch(eid);
            return Ingest::Applied;
        }
    }
    // The HOST applies a CLIENT's contents with its world up, so it resolves each record's class
    // names through object_index (an attacker's unknown name is an O(1) miss, not a whole-array walk);
    // a CLIENT applying the host's broadcast may be mid-load, so it keeps FindClass (its index can trail).
    const SR::ClassResolver resolve = IsHost() ? &ue_wrap::object_index::ClassByName : nullptr;
    switch (cio::WriteRecords(eid, inv, recs, resolve)) {
        case cio::WriteResult::Refused:  return Ingest::Handled;
        case cio::WriteResult::NotReady: return Ingest::Park;
        case cio::WriteResult::Written:  break;
    }

    g_applied[eid] = Seen{recordsHash, want};
    if (boxDue) ca::ApplyFlags(eid, actor, want);
    Touch(eid);
    // An open screen onto this container does not rebuild on a raw write; redraw it from the new records.
    ue_wrap::container_view::Redraw(actor);
    UE_LOGI("container_contents: eid=%u applied %d records", eid, static_cast<int32_t>(recs.size()));
    return Ingest::Applied;
}

// Dev red (container_slice_accept_red): the refusal is skipped, so the host applies a forged slice as it would a birth.
bool SliceAcceptRed() {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::container_slice_accept_red);
    return on;
}

// The hash of the records the container holds now (the guards' hash); false when they do not read.
bool RecordsHashOfLive(uint32_t eid, uint64_t& out) {
    void* const inv = internal::LiveWorldInventory(eid);
    std::vector<SR::SaveRecord> recs;
    if (!inv || !cio::ReadRecords(inv, recs)) return false;
    out = cw::RecordsHash(eid, recs);
    return true;
}

Ingest ParseAndApply(const std::vector<uint8_t>& blob, uint32_t& outEid, uint8_t senderSlot,
                     cb::Source src) {
    size_t o = 0;
    uint16_t count = 0;
    uint8_t flags = 0;
    if (!cw::ParseHeader(blob, o, outEid, count, flags)) return Ingest::Handled;
    if (IsHost() && senderSlot != 0 && src == cb::Source::Arrival) ct::NoteSliceReceived(senderSlot);
    // A container a client threw: the host built its copy empty from the intent and has published
    // nothing for it, so the slice of that author is the newest version of the transfer. There is no
    // reach to measure: the throw put it where it is.
    void* const bornActor = (IsHost() && senderSlot != 0) ? LivePropActor(outEid) : nullptr;
    const bool birth = bornActor && cb::Awaited(bornActor, senderSlot);
    // A birth is not unbounded: it spends the author's arrival budget, so an author cannot make the
    // host parse slices for this container at any rate while it waits. Past the budget it is HELD in
    // the pen rather than refused: it is the thrower's only copy of the contents and is sent once,
    // and a replay spends nothing, so the pen's own per-author cap and TTL are what bound it.
    if (birth) {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!s) return Ingest::Handled;
        if (cb::SpendArrival(outEid, senderSlot, NowMs(), *s, src, /*held=*/true) == cb::Arrival::TooFast)
            return Ingest::Park;
    }
    // A client authors a thrown container's birth only: any other slice is refused after its header (never applied, never relayed).
    if (IsHost() && senderSlot != 0 && !birth && !SliceAcceptRed()) {
        ct::RefuseNonBirth(outEid, senderSlot);
        return Ingest::Handled;
    }
    std::vector<SR::SaveRecord> recs;
    const char* why = "";
    if (!cw::ParseRecords(blob, o, count, recs, &why)) {
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
        if (inv && cio::ReadRecords(inv, hostRecs, /*neuterNested=*/false) && !hostRecs.empty()) {
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
    const uint64_t contentHash = cw::RecordsHash(outEid, recs);
    const Ingest outcome = ApplyContents(outEid, recs, contentHash, flags);
    // Host, a client's birth applied. the sent baseline keeps the host's own drain from re-broadcasting
    // the identical slice back the long way round.
    if (outcome == Ingest::Applied && IsHost() && senderSlot != 0) {
        if (birth) cb::Complete(bornActor);
        if (merged) {
            // The author lacks the host's part: the merged contents go to every peer, the author
            // included, by the host's own fan-out on the next sweep.
            internal::ForgetSent(outEid);
            internal::MarkDirty(outEid);
            UE_LOGI("container_contents: eid=%u slot %u transfer MERGED with the host's own records -- "
                    "published to all on the next sweep", outEid, static_cast<unsigned>(senderSlot));
            return outcome;
        }
        // The host's own reading of the container's arming is what its other peers are told, whatever the author sent.
        const uint8_t hostFlags = ca::ReadFlags(bornActor);
        internal::NoteSent(outEid, internal::Seen{contentHash, hostFlags});
        // The write and a fresh read of it must hash alike: the publication and the parity report hash by the read.
        uint64_t reread = 0;
        if (RecordsHashOfLive(outEid, reread) && reread != contentHash && g_rereadSaid.insert(outEid).second)
            UE_LOGW("container_contents: eid=%u the applied slice re-reads to a different hash", outEid);
        UE_LOGI("container_contents: eid=%u slot %u ACCEPTED -- the container now holds content %llu, "
                "which is what the relay carries", outEid, static_cast<unsigned>(senderSlot),
                static_cast<unsigned long long>(contentHash));
        // The relay belongs HERE and not at the arrival site: a slice that waited in the pen and
        // applied on a replay is the host's new truth just as much as one that applied on arrival,
        // and relaying only the second kind left every other peer without it, silently.
        if (auto* s = g_session.load(std::memory_order_acquire)) {
            std::vector<uint8_t> relayed = blob;
            cw::SetFlags(relayed, hostFlags);
            internal::RelayToOthers(s, senderSlot, relayed);
        }
    }
    return outcome;
}

// The readiness pen's replay: true when the blob was dealt with, false while its container is still
// not ready. The author rides with the blob because a parked slice must meet the SAME rule it would
// have met on arrival -- replaying a client's slice as slot 0 would hand it the host's own authority,
// and the host does park a client's birth slice (past the author's arrival budget).
bool ReplayParked(const std::vector<uint8_t>& blob, uint8_t authorSlot) {
    uint32_t eid = 0;
    const Ingest outcome = ParseAndApply(blob, eid, authorSlot, cb::Source::Replay);
    if (outcome == Ingest::NoActor) {
        // Parked with no actor (its author had no body yet), or the container went while it waited: the
        // eid was bound once or was never a bind wait, so this is a drop and not a never-bound one.
        UE_LOGW("container_contents: a parked slice for eid=%u met no container on its replay -- dropped", eid);
        return true;
    }
    return outcome != Ingest::Park;
}

// A blob held in bind_wait runs again once its container is bound: the whole of ParseAndApply, the author's
// birth check included, and its outcome is routed as on arrival except that a second miss is a drop.
void ReplayHeld(const uint8_t* bytes, size_t n, int senderSlot) {
    std::vector<uint8_t> blob(bytes, bytes + n);
    uint32_t eid = 0;
    const Ingest outcome = ParseAndApply(blob, eid, static_cast<uint8_t>(senderSlot), cb::Source::Replay);
    if (outcome == Ingest::NoActor)
        coop::element::bind_wait::NoteNeverBound(kContainerContents, eid);
    else if (outcome == Ingest::Park)
        pk::Admit(eid, static_cast<uint8_t>(senderSlot), std::move(blob), NowMs());
    else if (outcome == Ingest::Applied)
        pk::Erase(eid);   // an older pen entry of this eid must not apply over it
}

// Every live world container, boundary 1 applied: fn(eid, actor, inv). The connect seed and the
// dev walker share it.
template <typename Fn>
void WalkWorldContainers(Fn&& fn) {
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    for (const auto& pr : pairs) {
        // IsLiveByIndex: the snapshot does not protect the actor pointer.
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!ci::IsContainer(pr.actor)) continue;
        void* inv = ci::InventoryOf(pr.actor);
        if (!inv || !ci::IsWorldInventory(inv)) continue;   // BOUNDARY 1 (fail-closed)
        fn(static_cast<uint32_t>(pr.id), pr.actor, inv);
    }
}

}  // namespace

// From event_feed's client-side SnapshotBegin and SnapshotComplete dispatch, on the game thread.
void NoteJoinSnapshotBracket(bool open) { pk::NoteJoinBracket(open, NowMs()); }

// A write to a container reached this peer's edge (container_writer_watch): the container is marked
// dirty for the sweep, keyed by eid so nothing is dereferenced later. The applied hash is dropped: it
// means "an identical blob is a no-op" only while our state still equals what we applied, and after
// our own mutation a corrective re-publish of the unchanged host truth would look like a duplicate;
// a client whose write was refused once kept its diverged contents forever that way.
void MarkHostWrite(uint32_t eid) {
    internal::MarkDirty(eid);
    g_applied.erase(eid);
    Touch(eid);
}

void ForgetApplied(uint32_t eid) { g_applied.erase(eid); }

namespace {
void ApplyHeld(const std::vector<uint8_t>& blob, uint8_t senderSlot);   // defined with DeliverBlob below
}  // namespace

void Install(coop::net::Session* session) {
    // This lane owns a container's GObjStack slice, which the host publishes; the container's own
    // save record carries the index into that stack, with none of the contents.
    coop::prop_save_data::DeclareClassOwnedElsewhere(L"prop_container_C");
    g_session.store(session, std::memory_order_release);
    coop::props::container_apply_hold::SetApply(&ApplyHeld);
    cww::Install(session);
    ca::Install(session);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    cww::Tick();
    ca::Tick();
    sg::ResolvePendingNames();
    ct::Flush();   // closes a burst's refusal window; cheap with none open

    if (!g_announced) {
        g_announced = true;
        UE_LOGI("container_contents: installed (GObjStack slice lane, the container_writer_watch watches)");
    }

    const uint64_t now = NowMs();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepMs;

    g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));

    // Both peers drain: the host fans its changes out, a client ships a container it threw to the host,
    // which applies it and relays. Both sweep the readiness pen: a slice for a container that is here
    // but not ready (a birth past its author's arrival budget, say) is retried and evicted.
    cb::Sweep(now, internal::Dirty());
    internal::DrainDirty(s);
    pk::Sweep(&ReplayParked, NowMs());
}

namespace {

// What an assembled blob does here: applied, or kept in the bind wait or the readiness pen.
void DeliverBlob(std::vector<uint8_t> blob, uint8_t senderSlot) {
    uint32_t eid = 0;
    const Ingest outcome = ParseAndApply(blob, eid, senderSlot, cb::Source::Arrival);
    if (outcome == Ingest::NoActor) {
        // The container's element is not bound yet: held in bind_wait for the bind, the latest blob
        // winning. Not deferred: no bind is coming for this eid, so there is nothing to wait for.
        if (!coop::element::bind_wait::Hold(eid, kContainerContents, senderSlot, &ReplayHeld, blob.data(),
                                            blob.size(), /*coalesce=*/true))
            coop::element::bind_wait::NoteNeverBound(kContainerContents, eid);
        return;
    }
    if (outcome == Ingest::Park) {
        // The container is here but not ready (its inventory or slice slot, the author's body, the
        // arrival budget): kept in the readiness pen, latest wins per eid, retried by the sweep until the TTL.
        pk::Admit(eid, senderSlot, std::move(blob), NowMs());
        return;
    }
    // Applied here and now: an older slice of this eid still waiting in the pen must not apply over it at the
    // next sweep.
    if (outcome == Ingest::Applied) pk::Erase(eid);
}

// What container_apply_hold replays a kept blob through.
void ApplyHeld(const std::vector<uint8_t>& blob, uint8_t senderSlot) { DeliverBlob(blob, senderSlot); }

}  // namespace

void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // The acceptance matrix, before the assembler so no peer drives reassembly for a direction it
    // may not send: the host accepts only a non-zero slot (slot 0 is its own fan-out coming back);
    // a client accepts only slot 0, since peers never hear each other directly. A slot past the roster is no
    // client's: the per-slot tables clamp it into slot 0, so it is refused before any of them is touched.
    if (IsHost()) {
        if (senderSlot == 0) return;  // our own fan-out echoed back -- not a thing to apply
        if (senderSlot >= coop::net::kMaxPeers) {
            UE_LOGW("container_contents: blob from slot %u, past the roster -- REJECTED",
                    static_cast<unsigned>(senderSlot));
            return;
        }
    } else if (senderSlot != 0) {
        UE_LOGW("container_contents: blob from non-host slot %u on a CLIENT -- REJECTED "
                "(the host is the only authority a client accepts)",
                static_cast<unsigned>(senderSlot));
        return;
    }
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;  // incomplete

    if (coop::props::container_apply_hold::Armed()) {   // the drill's hold: the header is read for it alone
        size_t headerEnd = 0;
        uint16_t headerCount = 0;
        uint8_t headerFlags = 0;
        uint32_t headerEid = 0;
        if (cw::ParseHeader(blob, headerEnd, headerEid, headerCount, headerFlags) &&
            coop::props::container_apply_hold::Keep(headerEid, std::move(blob), senderSlot))
            return;
    }
    DeliverBlob(std::move(blob), senderSlot);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost()) return;

    size_t sent = 0;
    WalkWorldContainers([&](uint32_t eid, void* actor, void* inv) {
        // A transfer in progress reaches the joiner with the fan-out that completes it.
        if (cb::Awaited(actor, 0)) return;
        if (internal::BroadcastContainer(s, eid, inv, peerSlot, /*force=*/true)) {
            ++sent;
            Touch(eid);
        }
    });
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

bool ContentHashOf(uint32_t eid, uint64_t& outHash, uint32_t& outRecords) {
    void* actor = LivePropActor(eid);
    if (!actor || !ci::IsContainer(actor)) return false;
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsWorldInventory(inv)) return false;
    if (cb::Awaited(actor, 0)) return false;   // a transfer in progress is not a settled reading
    std::vector<SR::SaveRecord> recs;
    if (!cio::ReadRecords(inv, recs)) return false;
    outHash = cw::ContentHash(eid, recs, ca::ReadFlags(actor));
    outRecords = static_cast<uint32_t>(recs.size());
    return true;
}

bool DevRecordHashes(uint32_t eid, std::vector<uint64_t>& out, uint64_t& whole) {
    out.clear();
    void* actor = LivePropActor(eid);
    void* inv = actor && ci::IsContainer(actor) ? ci::InventoryOf(actor) : nullptr;
    std::vector<SR::SaveRecord> recs;
    if (!inv || !ci::IsWorldInventory(inv) || !cio::ReadRecords(inv, recs)) return false;
    for (const SR::SaveRecord& r : recs) out.push_back(cw::RecordsHash(eid, std::vector<SR::SaveRecord>{r}));
    whole = cw::RecordsHash(eid, recs);
    return true;
}

// Applies `recs` (the container's own records plus one appended) as a client's accepted slice is applied, and when
// `publish` marks the container for the sweep like any local write.
namespace {
bool ApplyAppended(uint32_t eid, const std::vector<SR::SaveRecord>& recs, bool publish) {
    if (ApplyContents(eid, recs, cw::RecordsHash(eid, recs), std::nullopt) != Ingest::Applied) return false;
    if (publish) MarkHostWrite(eid);
    return true;
}
}  // namespace

bool DevRawAppend(uint32_t eid, bool publish) {
    void* actor = LivePropActor(eid);
    void* inv = actor && ci::IsContainer(actor) ? ci::InventoryOf(actor) : nullptr;
    std::vector<SR::SaveRecord> recs;
    if (!inv || !ci::IsWorldInventory(inv) || !cio::ReadRecords(inv, recs, /*neuterNested=*/false) || recs.empty())
        return false;
    recs.push_back(recs.front());
    return ApplyAppended(eid, recs, publish);
}

// The item's own getData (save_record's CaptureRecord), which is what addObject files. A raw write is not a put
// replay: the record lacks the owner's fridge flag that addObject appends to bools[0].
bool DevRawAppendActor(uint32_t eid, void* item, bool publish) {
    void* actor = LivePropActor(eid);
    void* inv = actor && ci::IsContainer(actor) ? ci::InventoryOf(actor) : nullptr;
    std::vector<SR::SaveRecord> recs;
    SR::SaveRecord rec;
    if (!item || !inv || !ci::IsWorldInventory(inv) || !cio::ReadRecords(inv, recs, /*neuterNested=*/false) ||
        !SR::CaptureRecord(item, rec))
        return false;
    recs.push_back(std::move(rec));
    return ApplyAppended(eid, recs, publish);
}

bool SendTruthToSlot(uint32_t eid, int slot) {
    if (!IsHost()) return false;
    void* inv = internal::LiveWorldInventory(eid);
    return inv && internal::SendContainerTo(eid, inv, slot);
}

namespace internal {

bool IsHostRole() { return IsHost(); }

uint64_t NowMs() { return container_contents_sync::NowMs(); }

void* LiveWorldInventory(uint32_t eid) {
    void* actor = LivePropActor(eid);
    if (!actor || !ci::IsContainer(actor)) return nullptr;
    void* inv = ci::InventoryOf(actor);
    return inv && ci::IsWorldInventory(inv) ? inv : nullptr;
}

bool SendContainerTo(uint32_t eid, void* inv, int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && internal::BroadcastContainer(s, eid, inv, slot, /*force=*/true);
}

}  // namespace internal

void ForEachWorldContainer(void (*fn)(uint32_t eid, void* ctx), void* ctx) {
    WalkWorldContainers([&](uint32_t eid, void*, void*) { fn(eid, ctx); });
}

size_t AppliedEids() { return g_applied.size(); }

uint32_t TouchEpochOf(uint32_t eid) {
    const auto it = g_touchEpoch.find(eid);
    return it == g_touchEpoch.end() ? 0 : it->second;
}

void OnDisconnect() {
    coop::props::container_apply_hold::Reset();
    g_wrongClassSaid.clear();
    internal::ResetPublish();
    g_touchEpoch.clear();
    g_rereadSaid.clear();
    ct::Reset();
    pk::Reset();
    g_applied.clear();
    cb::Reset();
    g_asm.Clear();
    g_nextSweep = 0;
    g_announced = false;
    cww::OnDisconnect();
    ca::OnDisconnect();
}

}  // namespace coop::props::container_contents_sync
