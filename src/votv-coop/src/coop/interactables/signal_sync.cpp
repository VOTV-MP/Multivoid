// coop/signal_sync.cpp -- see coop/signal_sync.h.

#include "coop/interactables/signal_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/interactables/signal_wire.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/saved_signals.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <atomic>
#include <chrono>
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

void CompleteAssembly(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    const uint64_t hash = coop::signal_wire::ContentHash(blob);
    auto t = g_tombstones.find(hash);
    if (t != g_tombstones.end()) {
        g_tombstones.erase(t);
        UE_LOGI("signal_sync: append from slot %u dropped -- tombstoned delete won the race",
                static_cast<unsigned>(senderSlot));
        return;
    }
    SD::Row row;
    bool adopt = false;
    if (!coop::signal_wire::Deserialize(blob, row, adopt)) {
        UE_LOGW("signal_sync: malformed row blob from slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (adopt && senderSlot != 0) {
        // Audit I-3: adopt is host-only (no signal adopt path exists today --
        // the latent trust gate matches comp_sync::ApplyData's).
        UE_LOGW("signal_sync: adopt from non-host slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (!UE::EnsureResolved()) {
        UE_LOGW("signal_sync: row from slot %u dropped -- engine unresolved "
                "(world transition?)", static_cast<unsigned>(senderSlot));
        return;
    }
    if (UE::ApplySaveSignal(row)) {
        const int32_t n = UE::Count();
        UE::RowKey key;
        if (n > 0 && UE::ReadRowKey(n - 1, key))
            g_applied.push_back({key, hash, Clock::now()});
        UE_LOGI("signal_sync: applied row from slot %u ('%ls' lvl %d)",
                static_cast<unsigned>(senderSlot), row.name.c_str(), row.level);
    } else {
        UE_LOGW("signal_sync: saveSignal apply FAILED for slot %u ('%ls') -- row lost",
                static_cast<unsigned>(senderSlot), row.name.c_str());
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!UE::EnsureResolved()) return;
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollInterval;

    g_assembler.Sweep(now, kAssemblyTTL);
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

        if (!removed.empty() && s->connected()) {
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

    if (s->connected()) {
        for (size_t i = 0; i < g_shadow.size(); ++i) {
            ShadowRow& srow = g_shadow[i];
            if (srow.sent) continue;
            SD::Row r;
            if (!UE::ReadRow(static_cast<int32_t>(i), r)) {
                srow.sent = true;
                continue;
            }
            if (!SendRowBlob(s, r)) break;  // channel refused: retry next poll
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
}

}  // namespace coop::signal_sync
