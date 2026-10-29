// coop/props/container_contents_publish.cpp -- the host-to-peers half of the contents lane: the dirty drain, the broadcast of
// one container's slice (with the guard that skips an unchanged one), and the relay of an accepted client slice. The
// ingest half (the apply, the pens, the connect seed's walk) stays in container_contents_sync.cpp, which reaches this half
// through container_contents_internal.h. See coop/props/container_contents_sync.h.

#include "coop/props/container_contents_sync.h"
#include "coop/props/container_contents_internal.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_arming.h"
#include "coop/props/container_birth.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_route_census.h"  // the census counts the slices a route sends
#include "coop/element/registry.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace coop::props::container_contents_sync {
namespace {

namespace SR = ue_wrap::save_record;
namespace cw = coop::props::container_slice_wire;
namespace ci = ue_wrap::container_inventory;
namespace cb = coop::props::container_birth;
namespace cio = coop::props::container_contents_io;
namespace ca  = coop::props::container_arming;

using coop::element::LivePropActor;

uint32_t g_nextSeq = 1;

// Containers marked dirty by a writer watch, drained on the next sweep, keyed by element id rather
// than the component pointer: IsLive cannot detect an address recycled by a new object between the
// edge and the drain, and an eid re-resolves forward, so a destroyed container stops resolving.
std::set<uint32_t> g_dirty;

// What was last sent, per eid: the hash of the records and the flags apart. Skipping an unchanged blob is what bounds the
// orphaned-buffer cost: a steady-state re-broadcast allocates nothing. A change of the container's arming with the same
// records is sent.
std::map<uint32_t, internal::Seen> g_sent;

// Containers whose broadcast the transport refused while a world-ready peer was present, and
// first publications whose contents do not read yet; retried by the sweep.
std::set<uint32_t> g_retry;

// Dev instruments: sends refused with no world-ready peer (owed to the connect replay), and the
// re-arms of those when container_retry_red is on. Reset at session end.
uint64_t g_noReadyRefusals = 0;
uint64_t g_noReadyRetries = 0;
std::set<uint32_t> g_noReadyEids;   // the containers whose fan-out met such a refusal

bool NoReadyRetryRed() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::container_retry_red);
    return s;
}

}  // namespace

namespace internal {

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
    // A client ships a slice only for a container it threw: its birth.
    const bool birth = !IsHostRole() && toSlot < 0 && cb::IsBirthSlice(eid);
    // The host's reading of the container's arming rides every slice it sends; a client's slice carries none (container_arming).
    void* const owner = ci::OwnerOf(inv);
    if (IsHostRole()) ca::Track(eid);
    const uint8_t flags = IsHostRole() ? ca::ReadFlags(owner) : uint8_t{0};
    const uint64_t h = cw::RecordsHash(eid, recs);
    if (!force && !birth) {
        const auto it = g_sent.find(eid);
        if (it != g_sent.end() && it->second.records == h && it->second.flags == flags)
            return true;  // unchanged -- say nothing
    }
    const std::vector<uint8_t> blob = cw::Pack(eid, recs, flags);
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
        g_sent.erase(eid);
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
        if (toSlot < 0) g_sent[eid] = Seen{h, flags};  // only a FAN-OUT establishes what every peer has
        if (toSlot < 0 && !IsHostRole()) coop::dev::container_route_census::NoteSliceSent(eid);   // dev: the route census
        // The author of a mutation re-derives its own volume, mass and names here: the host
        // excludes the author from the relay, and the native take path does not call
        // updateVolumesAndMass, so the mutator's own displayed volume went stale while every other
        // peer converged. A targeted send -- a joiner's seed, the truth re-sent to a refused
        // author -- changed nothing on this peer, so it re-derives nothing; the seed sends every
        // container in the world in one frame.
        if (toSlot < 0) ci::RederiveShownState(owner, inv);
        UE_LOGI("container_contents: eid=%u shipped %zu records (%zu B)%s%s%s",
                eid, recs.size(), blob.size(),
                toSlot < 0 ? "" : " [targeted]",
                IsHostRole() ? "" : " [client-authored]",
                birth ? " [birth]" : "");
    }
    return ok;
}

// Host: an accepted client-authored slice on to every other peer, never back to the author:
// echoing a peer's own state reverts a newer local value and primes the baseline over it,
// silently eating that player's next action.
void RelayToOthers(coop::net::Session* s, uint8_t authorSlot, const std::vector<uint8_t>& blob) {
    if (!s || !IsHostRole()) return;
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
            ca::Untrack(eid);
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
        if (IsHostRole() && cb::Awaited(actor, 0)) { g_retry.insert(eid); continue; }
        if (!BroadcastContainer(s, eid, inv, -1, /*force=*/false)) g_retry.insert(eid);
    }
}

// Any change of a container's records or arming reaches here: the host's overlap-put judge forgets what it refused for it.
void MarkDirty(uint32_t eid) {
    g_dirty.insert(eid);
    container_act_host::overlap::OnContainerChanged(eid);
}

std::set<uint32_t>& Dirty() { return g_dirty; }

void NoteSent(uint32_t eid, const Seen& seen) { g_sent[eid] = seen; }

void ForgetSent(uint32_t eid) { g_sent.erase(eid); }

void ResetPublish() {
    g_dirty.clear();
    g_retry.clear();
    g_noReadyRefusals = 0;
    g_noReadyRetries = 0;
    g_noReadyEids.clear();
    g_sent.clear();
}

}  // namespace internal

uint64_t NoReadyRefusals() { return g_noReadyRefusals; }
uint64_t NoReadyRetries() { return g_noReadyRetries; }
bool NoReadyRefusedEid(uint32_t eid) { return g_noReadyEids.count(eid) != 0; }
bool SentBaselineKnown(uint32_t eid) { return g_sent.count(eid) != 0; }

}  // namespace coop::props::container_contents_sync
