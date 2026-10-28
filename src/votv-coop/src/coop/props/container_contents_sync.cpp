// coop/props/container_contents_sync.cpp -- world-container contents over the wire: a write to a
// container (the writer watches, coop/props/container_writer_watch) marks it dirty, a 250 ms
// sweep ships its GObjStack slice as a blob, the receiver writes the slice back and re-derives the volume and names through the
// engine's own verbs; a client's edit is a compare-and-swap the host arbitrates and relays. See
// coop/props/container_contents_sync.h.

#include "coop/props/prop_save_data.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_birth.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_park.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_writer_watch.h"
#include "coop/props/container_write_policy.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_route_census.h"  // the census counts the slices a route sends and the host judges
#include "coop/dev/dronesack_drill.h"  // IsEnabled -- the drill's judgement record is kept only while it runs
#include "coop/element/bind_wait.h"
#include "coop/element/registry.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"  // ClassByName -- the host's O(1) class resolve for a client's contents
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
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
namespace wp = coop::props::container_write_policy;
namespace pk = coop::props::container_park;
namespace cw = coop::props::container_slice_wire;
namespace ci = ue_wrap::container_inventory;
namespace cb = coop::props::container_birth;
namespace cio = coop::props::container_contents_io;
namespace cww = coop::props::container_writer_watch;

using coop::element::LivePropActor;

// The sweep drains an edge-driven set, not a poll; 250 ms coalesces a burst (a loot roll fires
// addObject four times) into one broadcast.
constexpr uint64_t kSweepMs = 250;

constexpr uint16_t kContainerContents = static_cast<uint16_t>(coop::net::ReliableKind::ContainerContents);

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_announced = false;
uint64_t g_nextSweep = 0;
uint32_t g_nextSeq = 1;

coop::blob_chunks::Assembler g_asm;

// Containers marked dirty by a writer watch, drained on the next sweep, keyed by element id rather
// than the component pointer: IsLive cannot detect an address recycled by a new object between the
// edge and the drain, and an eid re-resolves forward, so a destroyed container stops resolving.
std::set<uint32_t> g_dirty;
std::set<uint32_t> g_wrongClassSaid;  // eids a slice named that are no container here (said once a session)

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

// Containers whose broadcast the transport refused while a world-ready peer was present, and
// first publications whose contents do not read yet; retried by the sweep.
std::set<uint32_t> g_retry;

// Dev instruments: sends refused with no world-ready peer (owed to the connect replay), and the
// re-arms of those when container_retry_red is on. Reset at session end.
uint64_t g_noReadyRefusals = 0;
uint64_t g_noReadyRetries = 0;
std::set<uint32_t> g_noReadyEids;   // the containers whose fan-out met such a refusal
// Dev instrument: the host's last judgement of a client-authored slice, per container. Reset at session end.
std::map<uint32_t, ClientSliceOutcome> g_clientSlice;
// Dev instrument: per container, how often this lane touched it (a local write, an applied remote slice, a
// connect seed's send). The writer probe reads it to tell a change the lane saw from one it did not; kept only
// while that probe is armed. Reset at session end.
std::map<uint32_t, uint32_t> g_touchEpoch;
// Containers whose accepted slice re-read to another hash, said once each. Reset at session end.
std::set<uint32_t> g_rereadSaid;

bool NoReadyRetryRed() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::container_retry_red);
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// Broadcast one container.

// False if the send was refused while a world-ready peer was present, or a first publication is
// not readable yet (the caller arms the retry). Run by both peers: on the host toSlot
// < 0 fans out; on a client the same call reaches the host alone, the author-to-arbiter edge.

bool BroadcastContainer(coop::net::Session* s, uint32_t eid, void* inv, int toSlot, bool force) {
    std::vector<SR::SaveRecord> recs;
    if (!cio::ReadRecords(inv, recs)) {
        // Nothing resolvable is not a transport failure -- unless a first publication is owed for this
        // eid, which a slot not readable yet would otherwise lose for good.
        switch (cb::FirstOwed(eid, NowMs())) {
            case cb::Owed::None:    return true;
            case cb::Owed::Pending: return false;
            case cb::Owed::GivenUp: break;
        }
        UE_LOGW("container_contents: eid=%u -- its contents never became readable; the first "
                "publication is given up", eid);
        return true;
    }
    // The base being edited from: for a client the last host truth it applied; the host authors
    // from its own state and sends 0.
    uint64_t baseHash = 0;
    const bool birth = !IsHost() && toSlot < 0 && cb::IsBirthSlice(eid);
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
    if (toSlot < 0 && s && !s->AnyWorldReadyPeer()) {
        // The fan-out skips a slot that is not world-ready, so with no world-ready peer nobody
        // could take it, and it is not attempted: a send would only be refused and warn that it
        // retries. That slot's ready edge runs the connect replay, which reads the contents then
        // (QueueConnectBroadcastForSlot); a re-arm owes nothing. A refusal with a world-ready peer
        // present still retries. Same rule as interactable_channel's SendEdge. Safe before the
        // send: slot readiness is marked on the game thread (world_ready_answer's
        // MarkSlotWorldReady), the thread of this sweep, so the check cannot race a ready edge.
        // Nobody received this hash, so no baseline of what every peer has may stand: a later edit
        // back to the old contents must be sent, not judged unchanged.
        g_sentHash.erase(eid);
        ++g_noReadyRefusals;
        g_noReadyEids.insert(eid);
        if (!NoReadyRetryRed()) return true;
        ++g_noReadyRetries;
        return false;
    }
    const bool ok = (toSlot < 0)
        ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::ContainerContents,
                                      g_nextSeq++, blob)
        : coop::blob_chunks::SendBlobToSlot(s, toSlot, coop::net::ReliableKind::ContainerContents,
                                            g_nextSeq++, blob);
    if (ok) {
        if (toSlot < 0) cb::FirstSent(eid);
        if (toSlot < 0) g_sentHash[eid] = h;  // only a FAN-OUT establishes what every peer has
        if (toSlot < 0 && !IsHost()) coop::dev::container_route_census::NoteSliceSent(eid);   // dev: the route census
        // A client's own accepted slice IS the host's next published truth, and the author is
        // deliberately excluded from the relay that carries it -- so it advances its base here.
        // Optimistic and self-correcting: a refusal is answered by the host re-publishing its
        // truth to this peer, which lands in this same map.
        if (!IsHost()) g_baseHash[eid] = h;
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
            cb::Forget(eid);
            continue;
        }
        if (!ci::IsContainer(actor)) continue;
        void* inv = ci::InventoryOf(actor);
        if (!inv || !ci::IsWorldInventory(inv)) {   // BOUNDARY 1 (fail-closed)
            // A first publication owed for it waits for the component instead of ending here, and
            // is given up, said, at its deadline.
            const cb::Owed owed = cb::FirstOwed(eid, NowMs());
            if (owed == cb::Owed::None) continue;
            if (owed == cb::Owed::Pending) { g_retry.insert(eid); continue; }
            UE_LOGW("container_contents: eid=%u -- its inventory never resolved; the first publication "
                    "is given up", eid);
            continue;
        }
        // A transfer in progress is not published: what the host added to it so far merges into the
        // author's slice when it lands, and is published with it.
        if (IsHost() && cb::Awaited(actor, 0)) { g_retry.insert(eid); continue; }
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
    NoActor,   // the eid has no live actor here yet (birth skew / mid-join) -- hold in bind_wait for its bind
    Park,      // the actor is here but not ready (inventory, slice slot, author's body, budget) -- the pen
    Handled,   // dealt with and deliberately NOT applied: refused, malformed, non-container,
               // BOUNDARY 1, or a no-op duplicate. NEVER relayed.
    Applied,   // this peer's state actually changed. The only outcome the host may pass on.
};

// The lane's reader the policy calls at the base compare (wp::LiveHashReader).
bool ReadLiveHash(uint32_t eid, uint64_t& outHash) {
    uint32_t records = 0;
    return ContentHashOf(eid, outHash, records);
}

// Dev: the touch epoch is kept only while the writer probe is armed.
void Touch(uint32_t eid) {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::container_writer_probe);
    if (on) ++g_touchEpoch[eid];
}

// Applied, Handled, NoActor or Park; see Ingest.
Ingest ApplyContents(uint32_t eid, const std::vector<SR::SaveRecord>& recs, uint64_t blobHash) {
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
    {   // Identical contents -> do nothing. The raw-write orphans the previous arrays, so a
        // no-op apply must not allocate at all (that is what bounds the leak).
        auto it = g_appliedHash.find(eid);
        if (it != g_appliedHash.end() && it->second == blobHash) return Ingest::Handled;
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

    g_appliedHash[eid] = blobHash;
    Touch(eid);
    // The base a later local edit declares; never cleared by our own write edge.
    if (!IsHost()) g_baseHash[eid] = blobHash;
    UE_LOGI("container_contents: eid=%u applied %d records", eid, static_cast<int32_t>(recs.size()));
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
    void* const bornActor = (IsHost() && senderSlot != 0) ? LivePropActor(outEid) : nullptr;
    const bool birth = bornActor && cb::Awaited(bornActor, senderSlot);
    // Unjudged is not unbounded: a birth slice spends the same arrival budget an edit does, so an
    // author cannot make the host parse slices for this container at any rate while it waits. Past
    // the budget it is HELD in the pen rather than refused: it is the thrower's only copy of the
    // contents and is sent once, and a replay spends nothing, so the pen's own per-author cap and TTL
    // are what bound it.
    if (birth) {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!s) return Ingest::Handled;
        if (wp::SpendArrival(outEid, senderSlot, NowMs(), *s, src, /*held=*/true) == wp::Decision::TooFast)
            return Ingest::Park;
    }
    // Host arbitration before anything is touched; a refusal is answered by re-publishing the
    // host's truth to the author, so it converges instead of sitting on a divergent view.
    if (IsHost() && senderSlot != 0 && !birth) {
        auto* s = g_session.load(std::memory_order_acquire);
        // No session, no arbitration, and a client slice is never applied unjudged: the refusal
        // that cannot be explained is still a refusal. The base the author edited from is judged
        // against what the container holds right now, whoever wrote it: a write nothing published yet
        // (the drone's sale, a host-side take) leaves the author's base behind the live contents. The
        // policy reads them through ReadLiveHash once the rate and the reach have passed.
        const wp::Decision d = s ? wp::Accept(outEid, baseHash, &ReadLiveHash, senderSlot, NowMs(), *s, src)
                                 : wp::Decision::StaleBase;
        // No body to measure the author against yet: HOLD, do not refuse. The pen replays it and
        // the reach is judged the moment that peer has posed; the hold is bounded per author and
        // by the TTL, so a peer that never poses costs the host eight slices for thirty seconds.
        if (d == wp::Decision::NoBodyYet) return Ingest::Park;
        coop::dev::container_route_census::NoteSliceJudged(senderSlot, d == wp::Decision::Accept);   // dev: the route census
        if (coop::dev::dronesack_drill::IsEnabled())
            g_clientSlice[outEid] = d == wp::Decision::Accept ? ClientSliceOutcome::Accepted : ClientSliceOutcome::Refused;
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
    const uint64_t contentHash = cw::ContentHash(outEid, recs);
    const Ingest outcome = ApplyContents(outEid, recs, contentHash);
    // Host, client-authored and accepted. g_sentHash keeps the host's own drain from re-broadcasting
    // the identical slice back the long way round. The compare-and-swap needs no record of its own:
    // the next author is judged against the contents this apply just wrote.
    if (outcome == Ingest::Applied && IsHost() && senderSlot != 0) {
        if (birth) cb::Complete(bornActor);
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
        // The next author is judged against a fresh read of what was just written, so the write and the
        // read must hash alike; if they do not, that author is refused as stale.
        uint64_t reread = 0;
        if (ReadLiveHash(outEid, reread) && reread != contentHash && g_rereadSaid.insert(outEid).second)
            UE_LOGW("container_contents: eid=%u the applied slice re-reads to a different hash -- the next client "
                    "will be judged stale", outEid);
        UE_LOGI("container_contents: eid=%u slot %u ACCEPTED -- the container now holds content %llu, "
                "which is what the relay carries", outEid, static_cast<unsigned>(senderSlot),
                static_cast<unsigned long long>(contentHash));
        // The relay belongs HERE and not at the arrival site: a slice that waited in the pen and
        // applied on a replay is the host's new truth just as much as one that applied on arrival,
        // and relaying only the second kind left every other peer without it, silently.
        if (auto* s = g_session.load(std::memory_order_acquire))
            RelayToOthers(s, senderSlot, blob);
    }
    return outcome;
}

// The readiness pen's replay: true when the blob was dealt with, false while its container is still
// not ready. The author rides with the blob because a parked slice must pass the SAME arbitration it
// would have passed on arrival -- replaying a client's write as slot 0 would hand it the host's
// own authority, and the host does park client writes (a slice whose author has no body yet).
bool ReplayParked(const std::vector<uint8_t>& blob, uint8_t authorSlot) {
    uint32_t eid = 0;
    const Ingest outcome = ParseAndApply(blob, eid, authorSlot, wp::Source::Replay);
    if (outcome == Ingest::NoActor) {
        // Parked with no actor (its author had no body yet), or the container went while it waited: the
        // eid was bound once or was never a bind wait, so this is a drop and not a never-bound one.
        UE_LOGW("container_contents: a parked slice for eid=%u met no container on its replay -- dropped", eid);
        return true;
    }
    return outcome != Ingest::Park;
}

// A blob held in bind_wait runs again once its container is bound: the whole of ParseAndApply, the author's
// arbitration included, and its outcome is routed as on arrival except that a second miss is a drop.
void ReplayHeld(const uint8_t* bytes, size_t n, int senderSlot) {
    std::vector<uint8_t> blob(bytes, bytes + n);
    uint32_t eid = 0;
    const Ingest outcome = ParseAndApply(blob, eid, static_cast<uint8_t>(senderSlot), wp::Source::Replay);
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
    g_dirty.insert(eid);
    g_appliedHash.erase(eid);
    Touch(eid);
}

void Install(coop::net::Session* session) {
    // This lane owns a container's GObjStack slice behind a host-arbitrated compare-and-swap; the
    // container's own save record carries the index into that stack, with none of the arbitration.
    coop::prop_save_data::DeclareClassOwnedElsewhere(L"prop_container_C");
    g_session.store(session, std::memory_order_release);
    cww::Install(session);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    cww::Tick();
    sg::ResolvePendingNames();

    if (!g_announced) {
        g_announced = true;
        UE_LOGI("container_contents: installed (GObjStack slice lane, the container_writer_watch watches)");
    }

    const uint64_t now = NowMs();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepMs;

    g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));

    // Both peers drain: the host fans its changes out, a client ships the container it mutated to
    // the host, which arbitrates and relays. Both sweep the readiness pen: a slice for a container that
    // is here but not ready (a client write whose author has no body yet, say) is retried and evicted.
    cb::Sweep(now, g_dirty);
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

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost()) return;

    size_t sent = 0;
    WalkWorldContainers([&](uint32_t eid, void* actor, void* inv) {
        // A transfer in progress reaches the joiner with the fan-out that completes it.
        if (cb::Awaited(actor, 0)) return;
        if (BroadcastContainer(s, eid, inv, peerSlot, /*force=*/true)) {
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
    outHash = cw::ContentHash(eid, recs);
    outRecords = static_cast<uint32_t>(recs.size());
    return true;
}

bool DevRecordHashes(uint32_t eid, std::vector<uint64_t>& out, uint64_t& whole) {
    out.clear();
    void* actor = LivePropActor(eid);
    void* inv = actor && ci::IsContainer(actor) ? ci::InventoryOf(actor) : nullptr;
    std::vector<SR::SaveRecord> recs;
    if (!inv || !ci::IsWorldInventory(inv) || !cio::ReadRecords(inv, recs)) return false;
    for (const SR::SaveRecord& r : recs) out.push_back(cw::ContentHash(eid, std::vector<SR::SaveRecord>{r}));
    whole = cw::ContentHash(eid, recs);
    return true;
}

// Applies `recs` (the container's own records plus one appended) as a client's accepted slice is applied, and when
// `publish` marks the container for the sweep like any local write.
namespace {
bool ApplyAppended(uint32_t eid, const std::vector<SR::SaveRecord>& recs, bool publish) {
    if (ApplyContents(eid, recs, cw::ContentHash(eid, recs)) != Ingest::Applied) return false;
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

void ForEachWorldContainer(void (*fn)(uint32_t eid, void* ctx), void* ctx) {
    WalkWorldContainers([&](uint32_t eid, void*, void*) { fn(eid, ctx); });
}

size_t AppliedEids() { return g_appliedHash.size(); }

uint64_t NoReadyRefusals() { return g_noReadyRefusals; }
uint64_t NoReadyRetries() { return g_noReadyRetries; }
bool NoReadyRefusedEid(uint32_t eid) { return g_noReadyEids.count(eid) != 0; }
bool SentBaselineKnown(uint32_t eid) { return g_sentHash.count(eid) != 0; }
uint32_t TouchEpochOf(uint32_t eid) {
    const auto it = g_touchEpoch.find(eid);
    return it == g_touchEpoch.end() ? 0 : it->second;
}
ClientSliceOutcome LastClientSliceOutcome(uint32_t eid) {
    const auto it = g_clientSlice.find(eid);
    return it == g_clientSlice.end() ? ClientSliceOutcome::None : it->second;
}

void OnDisconnect() {
    g_wrongClassSaid.clear();
    g_dirty.clear();
    g_retry.clear();
    g_noReadyRefusals = 0;
    g_noReadyRetries = 0;
    g_noReadyEids.clear();
    g_clientSlice.clear();
    g_touchEpoch.clear();
    g_rereadSaid.clear();
    wp::Reset();
    pk::Reset();
    g_sentHash.clear();
    g_baseHash.clear();
    g_appliedHash.clear();
    cb::Reset();
    g_asm.Clear();
    g_nextSweep = 0;
    g_announced = false;
    cww::OnDisconnect();
}

}  // namespace coop::props::container_contents_sync
