// coop/signal_sync.cpp -- see coop/signal_sync.h.

#include "coop/interactables/signal_sync.h"

#include "coop/config/config.h"  // ReadEnv (drill mutate knob)
#include "coop/net/blob_chunks.h"
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

// Seeds arc (2026-08-23): the receive-side apply PARK -- replaces the old
// warn-and-drop when the engine is unresolved (world transition) or the native
// apply fails transiently. FIFO-ONCE-NONEMPTY: while non-empty, every newly
// completed blob appends behind it (a live row must never apply ahead of a
// still-parked one); drained each Tick once the engine resolves. Event-anchored
// on own world-up -- NO wall-clock TTL (a timer here is a drop one level up);
// a row still rejected kMaxApplyRetries times WITH the engine resolved is
// malformed-for-this-world and drops loudly (the deserialize-failure class).
// Bound kParkCap escalates to the session inbox pause (pause-not-drop).
struct ParkedRow {
    std::vector<uint8_t> blob;
    uint8_t senderSlot = 0;
    int retries = 0;
    Clock::time_point lastAttempt{};  // audit F-3: retries pace at 1 Hz real time
};
std::deque<ParkedRow> g_applyPark;
bool g_parkBackpressure = false;
constexpr size_t kParkCap = 256;
constexpr int kMaxApplyRetries = 30;

bool IsHostRole() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}
// The client lane is mute until its own world-ready announce (the meadow R6
// gate: a pre-ready client line reaching the host before the flip rides the
// seed back as a dup). The host is always ready.
bool CanSend() {
    return IsHostRole() || coop::net_pump::HasAnnouncedWorldReady();
}

uint64_t HashRow(const SD::Row& r) {
    const std::vector<uint8_t> blob = coop::signal_wire::Serialize(r, /*adopt=*/false);
    return coop::signal_wire::ContentHash(blob);
}

bool SendRowBlob(coop::net::Session* s, const SD::Row& r) {
    const std::vector<uint8_t> blob = coop::signal_wire::Serialize(r, /*adopt=*/false);
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

// The apply core. Tombstone/malformed/authority verdicts are TERMINAL (the row
// is correctly dead or garbage); an unresolved engine or a failed native apply
// is NOT-APPLIABLE (the caller parks + retries -- the old warn-and-drop here
// was a measured silent loss, seeds-arc design doc par.1.3).
enum class ApplyVerdict { Applied, Terminal, NotAppliable };

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
    bool adopt = false;
    if (!coop::signal_wire::Deserialize(blob, row, adopt)) {
        UE_LOGW("signal_sync: malformed row blob from slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return ApplyVerdict::Terminal;
    }
    if (adopt && senderSlot != 0) {
        // Audit I-3: adopt is host-only (no signal adopt path exists today --
        // the latent trust gate matches comp_sync::ApplyData's).
        UE_LOGW("signal_sync: adopt from non-host slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return ApplyVerdict::Terminal;
    }
    if (!UE::EnsureResolved()) return ApplyVerdict::NotAppliable;
    if (UE::ApplySaveSignal(row)) {
        const int32_t n = UE::Count();
        UE::RowKey key;
        if (n > 0 && UE::ReadRowKey(n - 1, key))
            g_applied.push_back({key, hash, Clock::now()});
        UE_LOGI("signal_sync: applied row from slot %u ('%ls' lvl %d)",
                static_cast<unsigned>(senderSlot), row.name.c_str(), row.level);
        return ApplyVerdict::Applied;
    }
    return ApplyVerdict::NotAppliable;
}

void SetParkBackpressure(bool on) {
    if (on == g_parkBackpressure) return;
    g_parkBackpressure = on;
    if (auto* s = g_session.load(std::memory_order_acquire)) s->SetApplyBackpressure(on);
    UE_LOGW("signal_sync: apply park %s (depth %zu)", on ? "BACKPRESSURE ON" : "drained",
            g_applyPark.size());
}

void ParkRow(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    g_applyPark.push_back({blob, senderSlot, 0});
    if (g_applyPark.size() >= kParkCap) SetParkBackpressure(true);
}

void CompleteAssembly(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    // FIFO-once-nonempty: while rows are parked, a newly completed blob must
    // apply BEHIND them, never ahead. Audit F-2: an UNSETTLED lane (!g_primed --
    // join load, world travel, array still filling) parks every arrival too, so
    // the park is the ONE ordering point for the whole unsettled window.
    if (!g_applyPark.empty() || !g_primed) { ParkRow(blob, senderSlot); return; }
    if (ApplyRowBlob(blob, senderSlot) == ApplyVerdict::NotAppliable)
        ParkRow(blob, senderSlot);
}

// Drain the park in arrival order (called every Tick once resolve succeeds).
void DrainApplyPark() {
    // Audit F-2: the drain anchor is the lane's OWN settle predicate (g_primed
    // encodes count-stability), not bare engine-resolve -- after a mid-session
    // world travel the array RESOLVES while still asynchronously filling, and
    // applying into that window is the loss class one level down. While
    // unprimed the park only absorbs (FIFO), exactly like the join episode.
    if (!g_primed) return;
    const auto now = Clock::now();
    while (!g_applyPark.empty()) {
        ParkedRow& front = g_applyPark.front();
        // Audit F-3: pace the stuck-front retry at 1 Hz REAL time (per-frame
        // counting burned all 30 retries in ~0.4 s -- a transient AddEmail
        // failure right after settle earned the malformed verdict).
        if (front.retries > 0 && now - front.lastAttempt < std::chrono::seconds(1)) break;
        const ApplyVerdict v = ApplyRowBlob(front.blob, front.senderSlot);
        if (v == ApplyVerdict::NotAppliable) {
            front.lastAttempt = now;
            if (++front.retries < kMaxApplyRetries) break;  // retry next second
            UE_LOGE("signal_sync: parked row from slot %u rejected %d times over ~%ds with "
                    "the lane settled -- dropped as malformed-for-this-world",
                    static_cast<unsigned>(front.senderSlot), front.retries, kMaxApplyRetries);
        }
        g_applyPark.pop_front();
    }
    if (g_parkBackpressure && g_applyPark.size() < kParkCap / 2) SetParkBackpressure(false);
}

// --- Seeds arc: the ready-edge join seed (shared helper + this lane's adapter) ---

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
        const std::vector<uint8_t> blob = coop::signal_wire::Serialize(r, /*adopt=*/false);
        int sent = 0;
        for (int32_t k = 0; k < count; ++k) {
            if (coop::blob_chunks::SendBlobToSlot(
                    s, peerSlot, coop::net::ReliableKind::SavedSignalAppend, g_nextSeq++, blob))
                ++sent;
        }
        return sent;
    }
    return 0;  // raced away since capture -- fine (meadow :817 precedent)
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
    // Drill mutate control (design doc par.3): with the capture disabled the
    // in-window drill email must NOT arrive (RED) -- proving the seed, not a
    // leftover retry, is the delivery mechanism. Env-gated, drill-only.
    if (coop::config::ReadEnv("VOTVCOOP_SEED_DISABLE") == "1") {
        UE_LOGW("%s: capture DISABLED by drill knob VOTVCOOP_SEED_DISABLE", "signal_sync");
        return;
    }
    g_seeder.Capture(peerSlot);
}

void CancelJoinSnapshot(int peerSlot) { g_seeder.Cancel(peerSlot); }

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (!IsHostRole()) return;
    g_seeder.SeedForSlot(g_session.load(std::memory_order_acquire), peerSlot);
}

void OnDisconnectSlot(int peerSlot) {
    // Slot teardown is a row transition (roster doctrine): the leaver's half
    // assemblies and seed bracket must not survive into a recycled occupant.
    if (peerSlot >= 0 && peerSlot < 256)
        g_assembler.ClearSlot(static_cast<uint8_t>(peerSlot));
    g_seeder.Cancel(peerSlot);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!UE::EnsureResolved()) return;
    // Seeds arc: drain the apply park FIRST (every tick, not 1 Hz -- parked rows
    // are already-delivered state waiting only on appliability).
    DrainApplyPark();
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollInterval;

    // JOIN-EPISODE BOUNDARY (the email_sync 2026-07-11 precedent, retrofitted by
    // the seeds arc): while THIS client is inside its own join world-load,
    // savedSignals is being torn down/re-materialized by the LOAD, not by any
    // player verb -- the shadow must never prime against nor diff across that
    // window, or the mid-load positional diff broadcasts FALSE SavedSignalDelete
    // for save rows the load then replaces (the email lane's measured 2026-06-19
    // class: the host deletes its content-identical real rows). Placed BEFORE the
    // TTL sweeps so a host delete arriving mid-load parks in g_tombstones un-swept
    // and lands after the post-load re-prime. Host never arms the episode.
    if (coop::world_load_episode::InEpisode()) {
        if (g_primed) {
            g_primed = false;
            g_shadow.clear();
            g_applied.clear();
        }
        return;
    }

    g_assembler.Sweep(now, kAssemblyTTL);
    // Audit F-4: a NON-EMPTY apply park is a live cross-dependency -- a parked
    // append's tombstone must not wall-clock-expire before the park drains (the
    // TTL is a leak-guard only, per the cross-lane-TTL lesson). Expiry resumes
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
                // hash 0 = was unreadable at shadow time (audit I-2: never
                // broadcast the hash-unknown sentinel as a delete key).
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
            SD::Row r;
            if (!UE::ReadRow(static_cast<int32_t>(i), r)) {
                srow.sent = true;
                continue;
            }
            if (!SendRowBlob(s, r)) {
                // Seeds arc (design doc par.2.6): a fan-out refused with ZERO
                // world-ready receivers is a VACUOUS success -- every absent peer
                // gets this row via save+seed. Retrying it across a future ready
                // edge was the measured pre-existing DUPLICATE (the row broadcast
                // to a joiner whose save already contains it). A refusal WITH a
                // ready peer present stays a real retry.
                if (!s->AnyWorldReadyPeer()) { srow.sent = true; continue; }
                break;  // channel refused: retry next poll
            }
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
    // Seeds arc: parks + seed brackets are session-scoped (an aborted joiner's
    // parked rows must never drain into a new session's world).
    g_applyPark.clear();
    SetParkBackpressure(false);
    g_seeder.Reset();
}

}  // namespace coop::signal_sync
