// coop/world/email_sync.cpp -- the email mirror: the shadow diff, the chunked append and the
// content-hash delete, the apply park and the join seed adapter. See coop/world/email_sync.h.

#include "coop/world/email_sync.h"

#include "coop/config/config.h"  // ReadEnv (drill mutate knob)
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"

#include "coop/comms/peer_action_feed.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_seed.h"
#include "coop/session/net_pump.h"
#include "coop/session/world_load_episode.h"

#include "ue_wrap/world/email.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace coop::email_sync {
namespace {

namespace UE = ue_wrap::email;
using Clock = std::chrono::steady_clock;
using coop::blob_chunks::Fnv64;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPollInterval = std::chrono::milliseconds(1000);
constexpr auto kAssemblyTTL  = std::chrono::seconds(20);
constexpr auto kTombstoneTTL = std::chrono::seconds(20);
constexpr size_t kTopicCap = 256, kTextCap = 4096, kPfpCap = 96;
// A single poll that adds more than this many rows is a save load (the history materialising
// at once), never gameplay, which trickles a mail or two per event. Such a batch is adopted as
// baseline and never broadcast: the joiner already has the history from the save transfer. The
// timing-independent catch-all behind stabilise-before-prime.
constexpr size_t kBulkAppendThreshold = 32;

// The shadow: one entry per email row, prefix-aligned with the array between polls (the game
// only appends at the tail; every other mutation flows through the poll diff or
// ApplyDeleteByHash, which keep alignment). The steady-state cost is one raw key read per row
// per second.
struct ShadowRow {
    UE::RowKey   key;       // per-process instance identity (raw bytes)
    uint64_t     hash = 0;  // cross-peer identity (serialized-blob FNV-1a 64)
    bool         sent = true;
    std::wstring topic;     // truncated subject -- so a delete can name the email in
                            // the peer-action feed ("<nick> deleted an email: X")
};
std::vector<ShadowRow> g_shadow;

// Truncate a topic for the shadow and the announce without splitting a surrogate pair: a
// dangling high surrogate would encode as invalid UTF-8 in the feed line.
std::wstring TruncTopic(const std::wstring& topic) {
    std::wstring t = topic.substr(0, 80);
    if (!t.empty() && t.back() >= 0xD800 && t.back() <= 0xDBFF) t.pop_back();
    return t;
}
bool g_primed = false;
// Stabilise before prime: the save's emails load asynchronously, so the array climbs over the
// first seconds, and priming at first sight would classify the rest as new local appends and
// broadcast them to a joiner that already has them, an echo flood with a multi-second
// game-thread freeze. So the prime waits until the count has held for two consecutive polls;
// the bulk-append re-baseline below is the catch-all if a mid-load pause defeats it.
int32_t g_primeLastCount = -1;
int     g_primeStable    = 0;
uint32_t g_nextSeq = 1;  // per-sender email id
Clock::time_point g_nextPoll{};

// Receiver assembly: (sender slot, blob seq) to blob, the shared transport.
coop::blob_chunks::Assembler g_assembler;

// Wire-applied rows awaiting shadow registration, keyed by the content hash, the stable
// cross-peer identity: the poll's append branch finds the freshly appended tail row by hash and
// marks it sent, so it never echoes. Not by the per-process row key: addEmail rebuilds the
// laptop list, which can re-touch the row's text between the apply-time read and the next poll
// and drift the key, and a wire row mis-flagged as local is re-broadcast.
struct AppliedMark {
    uint64_t hash;
    Clock::time_point at;
};
std::vector<AppliedMark> g_applied;

// Deletes not yet appliable (row not here, the delete beat the append, a transient
// misalignment), hash to arrival; retried each poll, swept by TTL.
std::map<uint64_t, Clock::time_point> g_tombstones;

// The receive-side apply park: FIFO once non-empty, drained each tick, anchored on the lane's
// own settle rather than a wall-clock TTL; after kMaxApplyRetries with the engine resolved a
// row is malformed for this world and dropped loudly. Reaching kParkCap escalates to the inbox
// pause. signal_sync carries the same machinery.
struct ParkedRow {
    std::vector<uint8_t> blob;
    uint8_t senderSlot = 0;
    int retries = 0;
    Clock::time_point lastAttempt{};  // retries pace at 1 Hz real time
};
std::deque<ParkedRow> g_applyPark;
bool g_parkBackpressure = false;
constexpr size_t kParkCap = 256;
constexpr int kMaxApplyRetries = 30;

bool IsHostRole() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}
// The client lane is mute until its own world-ready announce; email deletes are the client's
// only send. The host is always ready.
bool CanSend() {
    return IsHostRole() || coop::net_pump::HasAnnouncedWorldReady();
}

void AppendU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void AppendWchars(std::vector<uint8_t>& b, const std::wstring& s, size_t cap,
                  const char* what) {
    size_t n = s.size();
    if (n > cap) {
        UE_LOGW("email_sync: %s %zu chars > cap %zu -- truncating", what, n, cap);
        n = cap;
    }
    for (size_t i = 0; i < n; ++i) {
        const uint16_t c = static_cast<uint16_t>(s[i]);
        b.push_back(static_cast<uint8_t>(c & 0xFF));
        b.push_back(static_cast<uint8_t>(c >> 8));
    }
}

// The blob: version 1, the username byte, three u16 lengths, then the topic, the text and the
// pfp leaf as UTF-16LE.
std::vector<uint8_t> SerializeRow(const UE::Row& r) {
    std::vector<uint8_t> b;
    const size_t tc = r.topic.size() > kTopicCap ? kTopicCap : r.topic.size();
    const size_t xc = r.text.size() > kTextCap ? kTextCap : r.text.size();
    const size_t pc = r.pfpLeaf.size() > kPfpCap ? kPfpCap : r.pfpLeaf.size();
    b.reserve(8 + 2 * (tc + xc + pc));
    b.push_back(1);
    b.push_back(r.username);
    AppendU16(b, static_cast<uint16_t>(tc));
    AppendU16(b, static_cast<uint16_t>(xc));
    AppendU16(b, static_cast<uint16_t>(pc));
    AppendWchars(b, r.topic, kTopicCap, "topic");
    AppendWchars(b, r.text, kTextCap, "text");
    AppendWchars(b, r.pfpLeaf, kPfpCap, "pfp leaf");
    return b;
}

bool ReadU16(const std::vector<uint8_t>& b, size_t& off, uint16_t& v) {
    if (off + 2 > b.size()) return false;
    v = static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
    off += 2;
    return true;
}
bool ReadWchars(const std::vector<uint8_t>& b, size_t& off, size_t chars, std::wstring& s) {
    if (off + chars * 2 > b.size()) return false;
    s.clear();
    s.reserve(chars);
    for (size_t i = 0; i < chars; ++i) {
        s.push_back(static_cast<wchar_t>(b[off] | (b[off + 1] << 8)));
        off += 2;
    }
    return true;
}

bool DeserializeRow(const std::vector<uint8_t>& b, UE::Row& out) {
    if (b.size() < 8 || b[0] != 1) return false;
    out.username = b[1];
    size_t off = 2;
    uint16_t tc = 0, xc = 0, pc = 0;
    if (!ReadU16(b, off, tc) || !ReadU16(b, off, xc) || !ReadU16(b, off, pc)) return false;
    if (tc > kTopicCap || xc > kTextCap || pc > kPfpCap) return false;
    return ReadWchars(b, off, tc, out.topic) &&
           ReadWchars(b, off, xc, out.text) &&
           ReadWchars(b, off, pc, out.pfpLeaf);
}

// Ship one row as chunks through the shared transport; true only if every chunk was accepted.
// The caller retries the whole row next poll under a fresh seq, and the receiver's dangling
// half-assembly is swept by TTL.
bool SendRow(coop::net::Session* s, const UE::Row& r) {
    const std::vector<uint8_t> blob = SerializeRow(r);
    const uint32_t seq = g_nextSeq++;
    if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::EmailAppend, seq, blob))
        return false;
    UE_LOGI("email_sync: row broadcast (seq=%u, %zu B, topic '%ls')",
            seq, blob.size(), r.topic.c_str());
    return true;
}

// Delete the local row whose shadow hash matches, after verifying the shadow row still aligns
// with the array (a player delete in the same poll window shifts indexes; on a mismatch the
// next poll's diff resyncs and the tombstone retry lands it). True means applied, with the
// shadow updated inline.
bool ApplyDeleteByHash(uint64_t hash, std::wstring* outTopic = nullptr) {
    if (!g_primed) return false;
    if (hash == 0) return false;  // 0 marks hash-unknown shadow rows (unreadable at scan)
    for (size_t i = 0; i < g_shadow.size(); ++i) {
        if (g_shadow[i].hash != hash) continue;
        UE::RowKey live;
        if (!UE::ReadRowKey(static_cast<int32_t>(i), live) || !(live == g_shadow[i].key))
            return false;  // misaligned this instant; retry after the next diff
        if (outTopic) *outTopic = g_shadow[i].topic;  // subject for the feed (before delete)
        if (!UE::DelEmail(static_cast<int32_t>(i))) return false;
        g_shadow.erase(g_shadow.begin() + static_cast<ptrdiff_t>(i));
        UE_LOGI("email_sync: applied remote delete (row %zu, hash %016llx)",
                i, static_cast<unsigned long long>(hash));
        return true;
    }
    return false;
}

// The apply core, shared by the completion path and the park. A tombstone or a malformed blob
// is terminal; an unresolved engine or a failed native add is not appliable, parked and
// retried, since dropping it here was a measured silent loss.
enum class ApplyVerdict { Applied, Terminal, NotAppliable };

ApplyVerdict ApplyRowBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    const uint64_t hash = Fnv64(blob.data(), blob.size());
    auto t = g_tombstones.find(hash);
    if (t != g_tombstones.end()) {
        // The delete outran its own row's chunked append: honour it.
        g_tombstones.erase(t);
        UE_LOGI("email_sync: append from slot %u dropped -- tombstoned delete won the race",
                static_cast<unsigned>(senderSlot));
        return ApplyVerdict::Terminal;
    }
    UE::Row row;
    if (!DeserializeRow(blob, row)) {
        UE_LOGW("email_sync: malformed email blob from slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return ApplyVerdict::Terminal;
    }
    if (!UE::EnsureResolved()) return ApplyVerdict::NotAppliable;
    if (UE::AddEmail(row)) {
        // Echo-proof by content hash: the next poll's append branch finds this freshly appended
        // tail row by hash and marks it sent. The hash rather than the row key, since addEmail's
        // laptop rebuild can drift the key before the poll.
        g_applied.push_back({hash, Clock::now()});
        UE_LOGI("email_sync: applied email from slot %u (topic '%ls')",
                static_cast<unsigned>(senderSlot), row.topic.c_str());
        return ApplyVerdict::Applied;
    }
    return ApplyVerdict::NotAppliable;
}

void SetParkBackpressure(bool on) {
    if (on == g_parkBackpressure) return;
    g_parkBackpressure = on;
    if (auto* s = g_session.load(std::memory_order_acquire)) s->SetApplyBackpressure(on);
    UE_LOGW("email_sync: apply park %s (depth %zu)", on ? "BACKPRESSURE ON" : "drained",
            g_applyPark.size());
}

void ParkRow(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    g_applyPark.push_back({blob, senderSlot, 0});
    if (g_applyPark.size() >= kParkCap) SetParkBackpressure(true);
}

void CompleteAssembly(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    // FIFO once non-empty: while rows are parked a newly completed blob applies behind them, never
    // ahead. An unsettled lane (not primed: a join load, a world travel, the array still filling)
    // parks every arrival too, so the park is the one ordering point for the whole unsettled
    // window.
    if (!g_applyPark.empty() || !g_primed) { ParkRow(blob, senderSlot); return; }
    if (ApplyRowBlob(blob, senderSlot) == ApplyVerdict::NotAppliable)
        ParkRow(blob, senderSlot);
}

void DrainApplyPark() {
    // The drain anchor is the lane's own settle predicate (primed encodes count stability), not
    // bare engine resolve: after a mid-session world travel the array resolves while still filling
    // asynchronously, and applying into that window loses rows. While unprimed the park only
    // absorbs.
    if (!g_primed) return;
    const auto now = Clock::now();
    while (!g_applyPark.empty()) {
        ParkedRow& front = g_applyPark.front();
        // The stuck-front retry paces at 1 Hz real time; per-frame counting burned every retry in
        // well under a second.
        if (front.retries > 0 && now - front.lastAttempt < std::chrono::seconds(1)) break;
        const ApplyVerdict v = ApplyRowBlob(front.blob, front.senderSlot);
        if (v == ApplyVerdict::NotAppliable) {
            front.lastAttempt = now;
            if (++front.retries < kMaxApplyRetries) break;  // retry next second
            UE_LOGE("email_sync: parked row from slot %u rejected %d times over ~%ds with "
                    "the lane settled -- dropped as malformed-for-this-world",
                    static_cast<unsigned>(front.senderSlot), front.retries, kMaxApplyRetries);
        }
        g_applyPark.pop_front();
    }
    if (g_parkBackpressure && g_applyPark.size() < kParkCap / 2) SetParkBackpressure(false);
}

// The ready-edge join seed: the shared helper and this lane's adapter.

bool SeedHashArray(std::map<uint64_t, int32_t>& out) {
    if (!UE::EnsureResolved()) return false;
    const int32_t n = UE::EmailCount();
    if (n < 0) return false;
    for (int32_t i = 0; i < n; ++i) {
        UE::Row r;
        if (!UE::ReadRow(i, r)) return false;  // unreadable => fail the WHOLE capture
        const std::vector<uint8_t> blob = SerializeRow(r);
        ++out[Fnv64(blob.data(), blob.size())];
    }
    return true;
}

int SeedSendAppendToSlot(coop::net::Session* s, int peerSlot, uint64_t hash, int32_t count) {
    if (!UE::EnsureResolved()) return 0;
    const int32_t n = UE::EmailCount();
    for (int32_t i = 0; i < n; ++i) {
        UE::Row r;
        if (!UE::ReadRow(i, r)) continue;
        const std::vector<uint8_t> blob = SerializeRow(r);
        if (Fnv64(blob.data(), blob.size()) != hash) continue;
        int sent = 0;
        for (int32_t k = 0; k < count; ++k) {
            if (coop::blob_chunks::SendBlobToSlot(
                    s, peerSlot, coop::net::ReliableKind::EmailAppend, g_nextSeq++, blob))
                ++sent;
        }
        return sent;
    }
    return 0;  // raced away since the capture
}

int SeedSendDeleteToSlot(coop::net::Session* s, int peerSlot, uint64_t hash, int32_t count) {
    int sent = 0;
    for (int32_t k = 0; k < count; ++k) {
        coop::net::ContentHashPayload p{hash};
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::EmailDelete,
                                  &p, sizeof(p)))
            ++sent;
    }
    return sent;
}

constexpr coop::join_seed::LaneAdapter kSeedAdapter{
    "email_sync", &SeedHashArray, &SeedSendAppendToSlot, &SeedSendDeleteToSlot};
coop::join_seed::Seeder g_seeder{kSeedAdapter};

}  // namespace

void CaptureJoinSnapshot(int peerSlot) {
    if (!IsHostRole()) return;
    // The drill's mutate control: with the capture disabled the in-window drill email must not
    // arrive, proving the seed, not a leftover retry, is the delivery mechanism. Env-gated, drill
    // only.
    if (coop::config::ReadEnv("VOTVCOOP_SEED_DISABLE") == "1") {
        UE_LOGW("%s: capture DISABLED by drill knob VOTVCOOP_SEED_DISABLE", "email_sync");
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
    // Slot teardown is a row transition: the leaver's half assemblies and seed bracket must not
    // survive into a recycled occupant.
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
    // The apply park drains first, every tick, not at the poll rate.
    DrainApplyPark();
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollInterval;

    // The join-episode boundary: while this client is inside its own join world load, the email
    // array is torn down and re-materialised by the load, not by any player verb, so the shadow
    // must never prime against or diff across that window. Otherwise a mid-load prime latches onto
    // rows the load then replaces, a small save's default rows read as removed under the bulk
    // gate, and the client broadcasts deletes the host then applies to its own content-identical
    // rows. Placed before the TTL sweeps, so a host delete arriving mid-load parks unswept and
    // lands after the post-load re-prime. Only a client arms the episode.
    if (coop::world_load_episode::InEpisode()) {
        if (g_primed) {
            g_primed = false;
            g_shadow.clear();
            g_applied.clear();
        }
        g_primeLastCount = -1;  // re-stabilize on the settled post-load array
        g_primeStable = 0;
        return;
    }

    // The stale-state sweeps, 1 Hz over handfuls; sweeping only on arrival would leave a quiet
    // session holding dead entries.
    g_assembler.Sweep(now, kAssemblyTTL);
    // A non-empty apply park is a live cross-dependency: a parked append's tombstone must not
    // expire before the park drains, since the TTL is a leak guard only. Expiry resumes once the
    // park is empty.
    if (g_applyPark.empty())
    for (auto it = g_tombstones.begin(); it != g_tombstones.end();) {
        if (now - it->second > kTombstoneTTL) {
            UE_LOGW("email_sync: delete for hash %016llx expired unmatched",
                    static_cast<unsigned long long>(it->first));
            it = g_tombstones.erase(it);
        } else ++it;
    }
    for (auto it = g_applied.begin(); it != g_applied.end();) {
        if (now - it->at > kAssemblyTTL) it = g_applied.erase(it);
        else ++it;
    }

    const int32_t n = UE::EmailCount();
    if (n < 0) {
        // World down (a level transition) or the array unresolved: the next world's rows are fresh
        // allocations with fresh keys, never diffed across that boundary. Drop and re-prime
        // silently at world-up; the tombstones go too, since a prior world's tombstone must never
        // eat a new world's append.
        if (g_primed) {
            g_primed = false;
            g_shadow.clear();
            g_applied.clear();
            g_tombstones.clear();
        }
        g_primeLastCount = -1;  // re-stabilize before the next world's prime
        g_primeStable = 0;
        return;
    }

    if (!g_primed) {
        // First sight of the array this world: the rows are the save's history (a joiner got them
        // from the save transfer, the host's are its own), shadowed without broadcasting. The array
        // loads asynchronously, so the prime waits until the count has held for two consecutive
        // polls; rows loading after the prime would read as new local appends and be
        // mass-broadcast.
        if (n != g_primeLastCount) {
            g_primeLastCount = n;
            g_primeStable = 0;
            return;  // count still moving -- the save is still loading
        }
        if (++g_primeStable < 2) return;  // need one more identical poll (~1s) to confirm settled

        g_shadow.clear();
        g_shadow.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            ShadowRow srow;
            UE::ReadRowKey(i, srow.key);
            UE::Row r;
            if (UE::ReadRow(i, r)) {
                const std::vector<uint8_t> blob = SerializeRow(r);
                srow.hash = Fnv64(blob.data(), blob.size());
                srow.topic = TruncTopic(r.topic);
            }
            g_shadow.push_back(srow);
        }
        g_primed = true;
        UE_LOGI("email_sync: shadow primed at %d existing email(s) (settled)", n);
    } else {
        // The positional diff under the append-at-tail invariant: surviving rows keep their
        // relative order, anything in the shadow that no longer matches the array prefix was
        // deleted, and the tail past the matched prefix is new.
        std::vector<UE::RowKey> cur(static_cast<size_t>(n));
        bool readable = true;
        for (int32_t i = 0; i < n; ++i) {
            if (!UE::ReadRowKey(i, cur[static_cast<size_t>(i)])) {
                readable = false;
                break;
            }
        }
        if (!readable) return;  // transient; retry next poll

        std::vector<ShadowRow> next;
        next.reserve(static_cast<size_t>(n));
        std::vector<uint64_t> removed;
        // The local slot for the deleted-an-email feed line; the removed loop is the local-delete
        // branch (a remote delete erased the shadow synchronously in ApplyDeleteByHash). The raw
        // peer id: the announce resolves the local slot to the local nickname itself.
        const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
        size_t j = 0;
        std::vector<std::wstring> removedTopics;  // announces deferred past the bulk gate below
        for (ShadowRow& srow : g_shadow) {
            if (j < cur.size() && srow.key == cur[j]) {
                // Moved, not copied: the topic copied per surviving row per poll was thousands of
                // allocations a second on a big save, and the shadow is replaced wholesale below.
                next.push_back(std::move(srow));
                ++j;
            } else if (srow.hash != 0) {
                // Hash 0 means unreadable at shadow time; the sentinel is not a wire key.
                removed.push_back(srow.hash);
                removedTopics.push_back(std::move(srow.topic));
            }
        }
        // The bulk-removed gate, symmetric with the append threshold: the game's only remover from
        // the email array is the laptop's delete, a player action, so a bulk batch of removals in
        // one poll cannot be gameplay; it is a shadow desync (a world swap, an array reset).
        // Re-baselined silently, with no announce and no delete broadcast, each of which would
        // misattribute and mass-delete the peers' inboxes.
        if (removed.size() > kBulkAppendThreshold) {
            UE_LOGW("email_sync: %zu rows removed in one poll -- shadow desync "
                    "re-baseline (adopted, NOT announced/broadcast)", removed.size());
            removed.clear();
            removedTopics.clear();
        }
        // Each user delete goes to the shared feed, so the other peers learn who deleted what; only
        // in a session.
        if (s->connected())
            for (const std::wstring& topic : removedTopics)
                coop::peer_action_feed::Announce(localSlot,
                                                 L"deleted an email: " + topic);
        // A bulk batch of new rows in one poll is a save load, adopted as baseline and never
        // broadcast. It catches a prime that ran before the real save loaded (the count sat stable
        // at a menu or partial value), which the stabilise guard alone can miss.
        const bool bulkLoad = (cur.size() - j) > kBulkAppendThreshold;
        if (bulkLoad)
            UE_LOGW("email_sync: %zu new rows in one poll -- save-load re-baseline "
                    "(adopted as sent, NOT broadcast)", cur.size() - j);
        for (; j < cur.size(); ++j) {
            ShadowRow srow;
            srow.key = cur[j];
            // The row's stable content hash first, the identity a wire-applied row is correlated
            // on.
            UE::Row r;
            uint64_t rowHash = 0;
            const bool rowReadable = UE::ReadRow(static_cast<int32_t>(j), r);
            if (rowReadable) {
                const std::vector<uint8_t> blob = SerializeRow(r);
                rowHash = Fnv64(blob.data(), blob.size());
                srow.topic = TruncTopic(r.topic);
            }
            srow.hash = rowHash;
            bool wireApplied = false;
            if (rowReadable && !bulkLoad) {
                for (auto it = g_applied.begin(); it != g_applied.end(); ++it) {
                    if (it->hash == rowHash) {  // applied from the wire -> never echo back
                        srow.sent = true;
                        g_applied.erase(it);
                        wireApplied = true;
                        break;
                    }
                }
            }
            if (!wireApplied) {
                if (bulkLoad) {
                    srow.sent = true;   // save-load batch: adopt as baseline, never broadcast
                } else if (rowReadable) {
                    srow.sent = false;  // genuine local append: broadcast below
                } else {
                    srow.sent = true;   // unreadable: skip past it (never broadcast garbage)
                    UE_LOGW("email_sync: unreadable new row %zu -- skipped", j);
                }
            }
            next.push_back(srow);
        }
        g_shadow = std::move(next);

        if (!removed.empty() && s->connected() && CanSend()) {
            for (uint64_t h : removed) {
                coop::net::ContentHashPayload p{h};
                s->SendReliable(coop::net::ReliableKind::EmailDelete, &p, sizeof(p));
                UE_LOGI("email_sync: delete broadcast (hash %016llx)",
                        static_cast<unsigned long long>(h));
            }
        }
        // Disconnected deletes stay local: a later joiner converges through the save transfer, and
        // there is no peer to inform now.
    }

    // Retry the parked deletes against the freshly synced shadow.
    for (auto it = g_tombstones.begin(); it != g_tombstones.end();) {
        if (ApplyDeleteByHash(it->first)) it = g_tombstones.erase(it);
        else ++it;
    }

    // Broadcast pending appends in array order, host only: email is host-owned world state, since
    // every addEmail site is world, story or system authored, so a client is not an email
    // authority. Gating the send on the role closes the shared-inbox pollution vector (a client's
    // diverged simulation authoring a false email into a permanent host row) and the startup
    // transient before a client-side source kill latches. The client keeps all apply and
    // echo-proof bookkeeping, since the shadow and the diff are load-bearing for the mirror; only
    // the send is gated, and its locally authored rows stay pending. Delete stays symmetric: a
    // client's user delete must propagate. Host rows stay pending while disconnected; dropping
    // them would skip rows produced before the first connect.
    if (s->connected() && s->role() == coop::net::Role::Host) {
        for (size_t i = 0; i < g_shadow.size(); ++i) {
            ShadowRow& srow = g_shadow[i];
            if (srow.sent) continue;
            UE::Row r;
            if (!UE::ReadRow(static_cast<int32_t>(i), r)) {
                srow.sent = true;  // unreadable: skip past it (prime parity)
                continue;
            }
            if (!SendRow(s, r)) {
                // Refused with zero world-ready receivers is vacuous success: every absent peer
                // gets this row through the save and the seed, and retrying across a future ready
                // edge delivered a row the joiner had already loaded from its save.
                if (!s->AnyWorldReadyPeer()) { srow.sent = true; continue; }
                break;  // channel refused with a live audience: retry next poll
            }
            srow.sent = true;
        }
    }
}

void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (senderSlot >= coop::net::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (g_assembler.OnChunk(p, senderSlot, blob))
        CompleteAssembly(blob, senderSlot);
}

void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot) {
    if (senderSlot >= coop::net::kMaxPeers) return;
    std::wstring topic;
    if (ApplyDeleteByHash(p.contentHash, &topic)) {
        // A remote peer deleted this shared email: surface who, to the local feed.
        coop::peer_action_feed::Announce(senderSlot, L"deleted an email: " + topic);
    } else {
        // Deferred (row not present yet, a transient misalignment): the tombstone retry in Tick
        // applies it later without an announce, since the sender slot is not carried on the retry
        // path.
        g_tombstones[p.contentHash] = Clock::now();
    }
}

void OnDisconnect() {
    g_assembler.Clear();
    g_tombstones.clear();
    g_applied.clear();
    g_shadow.clear();
    g_primed = false;  // re-prime on the next session (save state may differ)
    g_primeLastCount = -1;  // re-stabilize before the next session's prime
    g_primeStable = 0;
    g_nextSeq = 1;
    g_nextPoll = {};   // no residual throttle into the next session
    // The park and the seed brackets are session-scoped.
    g_applyPark.clear();
    SetParkBackpressure(false);
    g_seeder.Reset();
}

}  // namespace coop::email_sync
