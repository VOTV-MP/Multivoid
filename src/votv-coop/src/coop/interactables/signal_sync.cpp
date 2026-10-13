// coop/interactables/signal_sync.cpp -- see coop/interactables/signal_sync.h.

#include "coop/interactables/signal_sync.h"

#include "coop/config/config.h"  // ReadEnv (drill mutate knob)
#include "coop/net/blob_chunks.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/interactables/signal_wire.h"
#include "coop/session/join_seed.h"
#include "coop/session/net_pump.h"
#include "coop/session/world_load_episode.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/saved_signals.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <vector>

namespace coop::signal_sync {
namespace {

namespace UE = ue_wrap::saved_signals;
namespace SD = ue_wrap::signal_dynamic;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPollInterval = std::chrono::milliseconds(1000);
constexpr auto kAssemblyTTL  = std::chrono::seconds(20);
constexpr auto kTombstoneTTL = std::chrono::seconds(20);

struct ShadowRow {
    UE::RowKey key;
    uint64_t   hash = 0;  // signal_wire::ContentHash (0 = hash-unknown)
    bool       sent = true;
};
std::vector<ShadowRow> g_shadow;
bool g_primed = false;
uint32_t g_nextSeq = 1;
Clock::time_point g_nextPoll{};

coop::blob_chunks::Assembler g_assembler;

struct AppliedMark {
    UE::RowKey key;
    uint64_t hash;
    Clock::time_point at;
};
std::vector<AppliedMark> g_applied;

std::map<uint64_t, Clock::time_point> g_tombstones;

// The receive-side apply PARK, for the rows that arrive while the lane cannot ask the game at all:
// unsettled (a join load, a world travel, the array still filling) or with no gamemode. "Not yet" is
// the lane's state and never a row's, so a row is tried once the lane settles and then leaves the
// park, applied or refused; nothing is retried. FIFO-ONCE-NONEMPTY: while non-empty, every newly
// completed blob appends behind it, so a live row can never apply ahead of a still-parked one.
// Event-anchored on our own world-up, with NO wall-clock TTL -- a timer here would be a drop one level
// up. Reaching kParkCap raises the session's receive pause: pause, not drop. MTA keeps no such park:
// a packet it cannot apply yet is a protocol error
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:760-765); ours exists because
// our world loads after the link is up and travels mid-session.
struct ParkedRow {
    std::vector<uint8_t> blob;
    uint8_t senderSlot = 0;
};
std::deque<ParkedRow> g_applyPark;
constexpr size_t kParkCap = 256;

coop::net::RefusalTally g_refusals{"signal_sync"};

bool IsHostRole() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

void NoteRefusal(uint8_t slot, const char* why) {
    g_refusals.Note(slot, why, Clock::now());
}
// The client lane is mute until its own world-ready announce: a pre-ready client line reaching
// the host before the flip rides the seed back as a duplicate. The host is always ready.
bool CanSend() {
    return IsHostRole() || coop::net_pump::HasAnnouncedWorldReady();
}

uint64_t HashRow(const SD::Row& r) {
    const std::vector<uint8_t> blob = coop::signal_wire::Serialize(r);
    return coop::signal_wire::ContentHash(blob);
}

bool SendRowBlob(coop::net::Session* s, const SD::Row& r) {
    const std::vector<uint8_t> blob = coop::signal_wire::Serialize(r);
    const uint32_t seq = g_nextSeq++;
    if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::SavedSignalAppend, seq, blob))
        return false;
    UE_LOGI("signal_sync: row broadcast (seq=%u, '%ls' lvl %d)",
            seq, r.name.c_str(), r.level);
    return true;
}

// Delete the local row whose shadow hash matches (alignment-verified; defers
// on mismatch -- the email_sync ApplyDeleteByHash doctrine).
bool ApplyDeleteByHash(uint64_t hash) {
    if (!g_primed) return false;
    if (hash == 0) return false;  // 0 marks hash-unknown shadow rows
    for (size_t i = 0; i < g_shadow.size(); ++i) {
        if (g_shadow[i].hash != hash) continue;
        UE::RowKey live;
        if (!UE::ReadRowKey(static_cast<int32_t>(i), live) || !(live == g_shadow[i].key))
            return false;
        if (!UE::DeleteSignal(static_cast<int32_t>(i))) return false;
        g_shadow.erase(g_shadow.begin() + static_cast<ptrdiff_t>(i));
        UE_LOGI("signal_sync: applied remote delete (row %zu, hash %016llx)",
                i, static_cast<unsigned long long>(hash));
        return true;
    }
    return false;
}

// The apply core. Terminal: the row is correctly dead (a tombstone won), unreadable, or refused by
// the game -- each final, since the same row would meet the same answer again. NotNow: the game
// cannot be asked yet, which the caller parks.
enum class ApplyVerdict { Applied, Terminal, NotNow };

ApplyVerdict ApplyRowBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    const uint64_t hash = coop::signal_wire::ContentHash(blob);
    auto t = g_tombstones.find(hash);
    if (t != g_tombstones.end()) {
        g_tombstones.erase(t);
        UE_LOGI("signal_sync: append from slot %u dropped -- tombstoned delete won the race",
                static_cast<unsigned>(senderSlot));
        return ApplyVerdict::Terminal;
    }
    SD::Row row;
    if (!coop::signal_wire::Deserialize(blob, row)) {
        NoteRefusal(senderSlot, "the row blob is malformed");
        return ApplyVerdict::Terminal;
    }
    if (!UE::EnsureResolved()) return ApplyVerdict::NotNow;
    const ue_wrap::RowApply r = UE::ApplySaveSignal(row);
    if (r.result == ue_wrap::RowApplyResult::NotNow) return ApplyVerdict::NotNow;
    if (r.result == ue_wrap::RowApplyResult::Refused) {
        NoteRefusal(senderSlot, r.why);
        return ApplyVerdict::Terminal;
    }
    const int32_t n = UE::Count();
    UE::RowKey key;
    if (n > 0 && UE::ReadRowKey(n - 1, key))
        g_applied.push_back({key, hash, Clock::now()});
    UE_LOGI("signal_sync: applied row from slot %u ('%ls' lvl %d)",
            static_cast<unsigned>(senderSlot), row.name.c_str(), row.level);
    return ApplyVerdict::Applied;
}

// The session's receive pause for this lane: raised at the flood bound, lowered once the park is half
// drained. The session holds the state (coop/net/apply_pause.h) and resets it when it starts, so no
// copy here can outlive the session that raised it.
void UpdateParkPause() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    constexpr auto kKind = coop::net::ReliableKind::SavedSignalAppend;
    const size_t depth = g_applyPark.size();
    if (depth >= kParkCap) {
        if (s->SetApplyPause(kKind, true))
            UE_LOGW("signal_sync: apply park BACKPRESSURE ON (depth %zu)", depth);
    } else if (depth < kParkCap / 2 && s->ApplyPausedBy(kKind)) {  // a load, not a write, each tick
        if (s->SetApplyPause(kKind, false))
            UE_LOGW("signal_sync: apply park drained (depth %zu)", depth);
    }
}

void ParkRow(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    g_applyPark.push_back({blob, senderSlot});
    UpdateParkPause();
}

void CompleteAssembly(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    // FIFO-once-nonempty: while rows are parked, a newly completed blob must apply BEHIND them,
    // never ahead. An UNSETTLED lane (!g_primed -- join load, world travel, array still filling)
    // parks every arrival too, so the park is the ONE ordering point for the whole unsettled
    // window.
    if (!g_applyPark.empty() || !g_primed) { ParkRow(blob, senderSlot); return; }
    if (ApplyRowBlob(blob, senderSlot) == ApplyVerdict::NotNow)
        ParkRow(blob, senderSlot);
}

// Drain the park in arrival order (called every Tick once resolve succeeds).
void DrainApplyPark() {
    // The drain anchor is the lane's OWN settle predicate -- g_primed -- and not a bare
    // engine-resolve: after a mid-session world travel the array RESOLVES while still filling, and
    // applying into that window is the loss class one level down. While unprimed the park only
    // absorbs, in FIFO order, exactly like the join episode. Settled, each row is tried once and
    // leaves; the first that meets NotNow keeps its place, since the lane went unsettled under it,
    // and is tried again next tick (NotNow is decided before any call into the game).
    if (!g_primed) return;
    while (!g_applyPark.empty()) {
        const ParkedRow& front = g_applyPark.front();
        if (ApplyRowBlob(front.blob, front.senderSlot) == ApplyVerdict::NotNow) break;
        g_applyPark.pop_front();
    }
    UpdateParkPause();
}

// --- the ready-edge join seed (shared helper + this lane's adapter) ---

bool SeedHashArray(std::map<uint64_t, int32_t>& out) {
    if (!UE::EnsureResolved()) return false;
    const int32_t n = UE::Count();
    if (n < 0) return false;
    for (int32_t i = 0; i < n; ++i) {
        SD::Row r;
        if (!UE::ReadRow(i, r)) return false;  // unreadable => fail the WHOLE capture
        ++out[HashRow(r)];
    }
    return true;
}

int SeedSendAppendToSlot(coop::net::Session* s, int peerSlot, uint64_t hash, int32_t count) {
    if (!UE::EnsureResolved()) return 0;
    const int32_t n = UE::Count();
    for (int32_t i = 0; i < n; ++i) {
        SD::Row r;
        if (!UE::ReadRow(i, r)) continue;
        if (HashRow(r) != hash) continue;
        if (!UE::ReadRow(i, r, /*withImage=*/true)) continue;  // the photo for the row that is sent, not each scanned
        const std::vector<uint8_t> blob = coop::signal_wire::Serialize(r);
        int sent = 0;
        for (int32_t k = 0; k < count; ++k) {
            if (coop::blob_chunks::SendBlobToSlot(
                    s, peerSlot, coop::net::ReliableKind::SavedSignalAppend, g_nextSeq++, blob))
                ++sent;
        }
        return sent;
    }
    return 0;  // raced away since capture -- fine
}

int SeedSendDeleteToSlot(coop::net::Session* s, int peerSlot, uint64_t hash, int32_t count) {
    int sent = 0;
    for (int32_t k = 0; k < count; ++k) {
        coop::net::ContentHashPayload p{hash};
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SavedSignalDelete,
                                  &p, sizeof(p)))
            ++sent;
    }
    return sent;
}

constexpr coop::join_seed::LaneAdapter kSeedAdapter{
    "signal_sync", &SeedHashArray, &SeedSendAppendToSlot, &SeedSendDeleteToSlot};
coop::join_seed::Seeder g_seeder{kSeedAdapter};

}  // namespace

void CaptureJoinSnapshot(int peerSlot) {
    if (!IsHostRole()) return;
    // Drill mutate control: with the capture disabled the in-window drill email must NOT arrive
    // (RED), which proves the seed, and not a leftover retry, is the delivery mechanism.
    // Env-gated, drill-only.
    if (coop::config::ReadEnv("VOTVCOOP_SEED_DISABLE") == "1") {
        UE_LOGW("%s: capture DISABLED by drill knob VOTVCOOP_SEED_DISABLE", "signal_sync");
        return;
    }
    g_seeder.Capture(peerSlot);
}

void CancelJoinSnapshot(int peerSlot) { g_seeder.Cancel(peerSlot); }

bool JoinSnapshotCaptured(int peerSlot) { return g_seeder.Captured(peerSlot); }

bool Settled() { return g_primed; }

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (!IsHostRole()) return;
    g_seeder.SeedForSlot(g_session.load(std::memory_order_acquire), peerSlot);
}

void OnDisconnectSlot(int peerSlot) {
    // Slot teardown is a row transition (roster doctrine): the leaver's half
    // assemblies and seed bracket must not survive into a recycled occupant.
    if (peerSlot >= 0 && peerSlot < 256) {
        g_assembler.ClearSlot(static_cast<uint8_t>(peerSlot));
    }
    g_seeder.Cancel(peerSlot);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!UE::EnsureResolved()) return;
    // Drain the apply park FIRST, every tick and not at 1 Hz: parked rows are already-delivered
    // state waiting only on appliability.
    DrainApplyPark();
    const auto now = Clock::now();
    g_refusals.Flush(now);  // a burst's summed line once its window has passed
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollInterval;

    // JOIN-EPISODE BOUNDARY. While THIS client is inside its own join world-load, savedSignals is
    // being torn down and re-materialized by the LOAD, not by any player verb -- so the shadow must
    // never prime against that window nor diff across it, or the mid-load positional diff
    // broadcasts a FALSE SavedSignalDelete for save rows the load then replaces, and the host
    // deletes its content-identical real rows. Placed BEFORE the TTL sweeps so a host delete
    // arriving mid-load parks in g_tombstones un-swept and lands after the post-load re-prime. The
    // host never arms the episode.
    if (coop::world_load_episode::InEpisode()) {
        if (g_primed) {
            g_primed = false;
            g_shadow.clear();
            g_applied.clear();
        }
        return;
    }

    g_assembler.Sweep(now, kAssemblyTTL);
    // A NON-EMPTY apply park is a live cross-dependency: a parked append's tombstone must not
    // wall-clock-expire before the park drains, the TTL being a leak-guard only. Expiry resumes
    // once the park is empty; entries merely age while it waits.
    if (g_applyPark.empty())
    for (auto it = g_tombstones.begin(); it != g_tombstones.end();) {
        if (now - it->second > kTombstoneTTL) {
            UE_LOGW("signal_sync: delete for hash %016llx expired unmatched",
                    static_cast<unsigned long long>(it->first));
            it = g_tombstones.erase(it);
        } else ++it;
    }
    for (auto it = g_applied.begin(); it != g_applied.end();) {
        if (now - it->at > kAssemblyTTL) it = g_applied.erase(it);
        else ++it;
    }

    const int32_t n = UE::Count();
    if (n < 0) {
        // World down: fresh allocations at world-up -- never diff across it.
        if (g_primed) {
            g_primed = false;
            g_shadow.clear();
            g_applied.clear();
            g_tombstones.clear();
        }
        return;
    }

    if (!g_primed) {
        g_shadow.clear();
        g_shadow.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            ShadowRow srow;
            UE::ReadRowKey(i, srow.key);
            SD::Row r;
            if (UE::ReadRow(i, r)) srow.hash = HashRow(r);
            g_shadow.push_back(srow);
        }
        g_primed = true;
        UE_LOGI("signal_sync: shadow primed at %d saved signal(s)", n);
    } else {
        std::vector<UE::RowKey> cur(static_cast<size_t>(n));
        bool readable = true;
        for (int32_t i = 0; i < n; ++i) {
            if (!UE::ReadRowKey(i, cur[static_cast<size_t>(i)])) {
                readable = false;
                break;
            }
        }
        if (!readable) return;

        std::vector<ShadowRow> next;
        next.reserve(static_cast<size_t>(n));
        std::vector<uint64_t> removed;
        size_t j = 0;
        for (const ShadowRow& srow : g_shadow) {
            if (j < cur.size() && srow.key == cur[j]) {
                next.push_back(srow);
                ++j;
            } else if (srow.hash != 0) {
                // hash 0 = was unreadable at shadow time; never broadcast the hash-unknown sentinel
                // as a delete key.
                removed.push_back(srow.hash);
            }
        }
        for (; j < cur.size(); ++j) {
            ShadowRow srow;
            srow.key = cur[j];
            bool wireApplied = false;
            for (auto it = g_applied.begin(); it != g_applied.end(); ++it) {
                if (it->key == cur[j]) {
                    srow.hash = it->hash;
                    srow.sent = true;
                    g_applied.erase(it);
                    wireApplied = true;
                    break;
                }
            }
            if (!wireApplied) {
                SD::Row r;
                if (UE::ReadRow(static_cast<int32_t>(j), r)) {
                    srow.hash = HashRow(r);
                    srow.sent = false;
                } else {
                    UE_LOGW("signal_sync: unreadable new row %zu -- skipped", j);
                }
            }
            next.push_back(srow);
        }
        g_shadow = std::move(next);

        if (!removed.empty() && s->connected() && CanSend()) {
            for (uint64_t h : removed) {
                coop::net::ContentHashPayload p{h};
                s->SendReliable(coop::net::ReliableKind::SavedSignalDelete, &p, sizeof(p));
                UE_LOGI("signal_sync: delete broadcast (hash %016llx)",
                        static_cast<unsigned long long>(h));
            }
        }
    }

    for (auto it = g_tombstones.begin(); it != g_tombstones.end();) {
        if (ApplyDeleteByHash(it->first)) it = g_tombstones.erase(it);
        else ++it;
    }

    if (s->connected() && CanSend()) {
        for (size_t i = 0; i < g_shadow.size(); ++i) {
            ShadowRow& srow = g_shadow[i];
            if (srow.sent) continue;
            // No world-ready receiver: on the host every absent peer gets this row through the
            // save and the seed (SeedSendAppendToSlot), so the row counts as sent; retrying it
            // across a future ready edge was the measured duplicate, a row broadcast to a joiner
            // whose save already holds it. On a client no ready receiver means the host is gone,
            // and the row has nowhere to go. Asked before the send, so a broadcast nobody could
            // receive is not sent; a peer lost between this check and the send is still refused
            // once (a net-thread disconnect), and the next poll counts the row as sent.
            if (!s->AnyWorldReadyPeer()) { srow.sent = true; continue; }
            SD::Row r;
            if (!UE::ReadRow(static_cast<int32_t>(i), r, /*withImage=*/true)) {
                srow.sent = true;
                continue;
            }
            if (!SendRowBlob(s, r)) break;  // refused with a live audience: retry next poll
            srow.sent = true;
        }
    }
}

void OnAppendChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (senderSlot >= coop::net::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (g_assembler.OnChunk(p, senderSlot, blob))
        CompleteAssembly(blob, senderSlot);
}

void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot) {
    if (senderSlot >= coop::net::kMaxPeers) return;
    if (!ApplyDeleteByHash(p.contentHash))
        g_tombstones[p.contentHash] = Clock::now();
}

void OnDisconnect() {
    g_assembler.Clear();
    g_tombstones.clear();
    g_applied.clear();
    g_shadow.clear();
    g_primed = false;
    g_nextSeq = 1;
    g_nextPoll = {};
    // Parks and seed brackets are session-scoped: an aborted joiner's parked rows must never drain
    // into a new session's world.
    g_applyPark.clear();
    UpdateParkPause();
    g_refusals.Clear();
    g_seeder.Reset();
}

}  // namespace coop::signal_sync
