// coop/email_sync.cpp -- see coop/email_sync.h.

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
// A single poll that adds more than this many rows is a SAVE LOAD (the email
// history materializing all at once), never gameplay (which trickles 1-2 mails per
// in-game event). Such a batch is ADOPTED as baseline and never broadcast -- the
// joiner already gets the full history via the v56 save transfer. This is the
// timing-independent catch-all behind stabilize-before-prime (2026-06-19 flood RCA).
constexpr size_t kBulkAppendThreshold = 32;

// The shadow: one entry per saveSlot.emails row, prefix-aligned with the
// array between polls (the game only appends at the tail; every other
// mutation flows through the poll diff or ApplyDeleteByHash, both of which
// keep alignment). Single steady-state cost: n raw RowKey reads per second.
struct ShadowRow {
    UE::RowKey   key;       // per-process instance identity (raw bytes)
    uint64_t     hash = 0;  // cross-peer identity (serialized-blob FNV-1a 64)
    bool         sent = true;
    std::wstring topic;     // truncated subject -- so a delete can name the email in
                            // the peer-action feed ("<nick> deleted an email: X")
};
std::vector<ShadowRow> g_shadow;

// Truncate a topic for the shadow/announce (80 UTF-16 units) WITHOUT splitting a
// surrogate pair -- a dangling high surrogate would encode as invalid UTF-8 in the
// peer-action line (audit 2026-07-10 LOW).
std::wstring TruncTopic(const std::wstring& topic) {
    std::wstring t = topic.substr(0, 80);
    if (!t.empty() && t.back() >= 0xD800 && t.back() <= 0xDBFF) t.pop_back();
    return t;
}
bool g_primed = false;
// Stabilize-before-prime (2026-06-19 flood RCA): the save's emails load
// ASYNchronously, so the array climbs 2 -> ... -> N over the first seconds. Priming
// at the first sight (N=2 of a 2056-row save) mis-classifies the other 2054 as "new
// local appends" and broadcasts them to a joiner that already has them via the v56
// save transfer -> the echo flood + a multi-second game-thread freeze. So we prime
// only after the count HOLDS for 2 consecutive polls (a lighter take on
// save_transfer's stable-read guard -- the email array loads monotonically, and the
// bulk-append re-baseline below is the catch-all if a mid-load pause defeats this).
int32_t g_primeLastCount = -1;
int     g_primeStable    = 0;
uint32_t g_nextSeq = 1;  // per-sender email id
Clock::time_point g_nextPoll{};

// Receiver assembly: (senderSlot, blobSeq) -> blob (shared transport).
coop::blob_chunks::Assembler g_assembler;

// Wire-applied rows awaiting shadow registration, keyed by the CONTENT HASH (the
// stable cross-peer identity). The poll's append branch finds the freshly-appended
// tail row by hash and marks it sent (echo-proof). We deliberately do NOT correlate
// by the per-process RowKey: addEmail rebuilds the laptop list UI, which can re-touch
// the row's FText between the apply-time read and the next poll, drifting the RowKey
// -> a wire row mis-flagged as a local append -> re-broadcast -> the 2026-06-19 echo
// flood. The content hash is computed from the serialized bytes, so it cannot drift.
struct AppliedMark {
    uint64_t hash;
    Clock::time_point at;
};
std::vector<AppliedMark> g_applied;

// Deletes we couldn't apply yet (row not here / delete beat the append /
// transient misalignment): hash -> arrival. Retried each poll, TTL-swept.
std::map<uint64_t, Clock::time_point> g_tombstones;

// Seeds arc (2026-08-23): the receive-side apply PARK -- replaces the old
// warn-and-drop ("engine unresolved -- row lost" / "apply FAILED -- row lost").
// FIFO-once-nonempty; drained each Tick; event-anchored on own world-up (no
// wall-clock TTL); kMaxApplyRetries with the engine RESOLVED = malformed-for-
// this-world, dropped loudly. Bound kParkCap escalates to the inbox pause.
// The signal_sync twin carries the same machinery (shared shape, per the
// deliberate-second-instance doctrine; the SEED half is the extracted third).
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
// gate; email deletes are the client's only send). The host is always ready.
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

// Blob: {u8 version=1; u8 username; u16 topicChars; u16 textChars; u16 pfpChars;
//        topic UTF-16LE; text; pfpLeaf}.
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

// Ship one row as chunks via the shared transport. True only if every chunk
// was accepted (blob_chunks I-3): the caller retries the whole row next poll
// under a FRESH seq; the receiver's dangling half-assembly is TTL-swept.
bool SendRow(coop::net::Session* s, const UE::Row& r) {
    const std::vector<uint8_t> blob = SerializeRow(r);
    const uint32_t seq = g_nextSeq++;
    if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::EmailAppend, seq, blob))
        return false;
    UE_LOGI("email_sync: row broadcast (seq=%u, %zu B, topic '%ls')",
            seq, blob.size(), r.topic.c_str());
    return true;
}

// Delete the local row whose shadow hash matches. Verifies the shadow row
// still aligns with the array (a player delete in the same poll window
// shifts indexes; on mismatch we defer -- the next poll's diff resyncs and
// the tombstone retry lands it). True = applied (shadow updated inline).
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

// Deserialize + apply a complete blob (shared by the normal completion path
// and the audit-C-1 restart path).
// The apply core. Tombstone/malformed verdicts are TERMINAL; an unresolved
// engine or a failed native AddEmail is NOT-APPLIABLE (parked + retried -- the
// old warn-and-drop here was a measured silent loss, seeds-arc doc par.1.3).
enum class ApplyVerdict { Applied, Terminal, NotAppliable };

ApplyVerdict ApplyRowBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    const uint64_t hash = Fnv64(blob.data(), blob.size());
    auto t = g_tombstones.find(hash);
    if (t != g_tombstones.end()) {
        // The delete outran its own row's chunked append: honor it.
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
        // ECHO-PROOF by CONTENT HASH: record the wire hash; the next poll's append
        // branch finds this freshly-appended tail row by hash and marks it sent, so
        // we never echo it back to the sender. Hash (not RowKey) because addEmail's
        // laptop-UI rebuild can drift the row's per-process key before the poll runs.
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
    // FIFO-once-nonempty: while rows are parked, a newly completed blob applies
    // BEHIND them, never ahead. Audit F-2: an UNSETTLED lane (!g_primed -- join
    // load, world travel, array still filling) parks every arrival too, so the
    // park is the ONE ordering point for the whole unsettled window.
    if (!g_applyPark.empty() || !g_primed) { ParkRow(blob, senderSlot); return; }
    if (ApplyRowBlob(blob, senderSlot) == ApplyVerdict::NotAppliable)
        ParkRow(blob, senderSlot);
}

void DrainApplyPark() {
    // Audit F-2: the drain anchor is the lane's OWN settle predicate (g_primed
    // encodes count-stability via the 2-stable-poll prime), not bare engine-
    // resolve -- after a mid-session world travel the array RESOLVES while still
    // asynchronously filling, and applying into that window is the loss class
    // one level down. While unprimed the park only absorbs (FIFO).
    if (!g_primed) return;
    const auto now = Clock::now();
    while (!g_applyPark.empty()) {
        ParkedRow& front = g_applyPark.front();
        // Audit F-3: pace the stuck-front retry at 1 Hz REAL time (per-frame
        // counting burned all 30 retries in ~0.4 s).
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

// --- Seeds arc: the ready-edge join seed (shared helper + this lane's adapter) ---

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
    return 0;  // raced away since capture -- fine (meadow :817 precedent)
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
    // Drill mutate control (design doc par.3): with the capture disabled the
    // in-window drill email must NOT arrive (RED) -- proving the seed, not a
    // leftover retry, is the delivery mechanism. Env-gated, drill-only.
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
    // Seeds arc: drain the apply park FIRST (every tick, not 1 Hz).
    DrainApplyPark();
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollInterval;

    // JOIN-EPISODE BOUNDARY (2026-07-11, user-observed live). While THIS client is
    // inside its own join world-load (world_load_episode: Arm at BootStorySaveBlocking
    // -> quiescence), saveSlot.emails is being torn down / re-materialized by the LOAD,
    // not by any player verb -- so the shadow must never prime against, nor diff
    // across, that window. Without this gate the mid-load prime latches onto rows the
    // load then replaces (fresh instance keys): a small save's default rows ("hi",
    // "welcome") read as removed.size()==2, slip UNDER the >32 bulk-desync gate, and
    // broadcast EmailDelete -- the HOST then deletes its content-identical rows and
    // announces "<client> deleted an email: hi". Same root class as the v106 destroy-
    // seam host-wipe; the same source-anchored invariant closes it. Placed BEFORE the
    // TTL sweeps so a host delete arriving mid-load parks in g_tombstones un-swept and
    // lands after the post-load re-prime. Host never arms the episode (client-only).
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

    // Stale-state sweeps (audit N-1 precedent: sweeping only on arrival left
    // a quiet session holding dead entries indefinitely). 1 Hz over handfuls.
    g_assembler.Sweep(now, kAssemblyTTL);
    // Audit F-4: a NON-EMPTY apply park is a live cross-dependency -- a parked
    // append's tombstone must not wall-clock-expire before the park drains (the
    // TTL is a leak-guard only, per the cross-lane-TTL lesson). Expiry resumes
    // once the park is empty; entries merely age while it waits.
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
        // World down (level transition) or array unresolved: the next world's
        // rows are fresh allocations with fresh instance keys -- never diff
        // across that boundary. Drop and re-prime silently at world-up.
        // Tombstones go too (audit IMP-2: a prior-world tombstone must never
        // eat a new-world append).
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
        // First sight of the array this world: the rows are the save's history (a
        // joiner got them via the v56 save transfer; the host's are its own) -- shadow
        // them WITHOUT broadcasting. CRITICAL: the array loads ASYNCHRONOUSLY, so wait
        // until the count has stopped climbing (held for 2 consecutive polls) before
        // priming -- otherwise the rows that load AFTER the prime are mis-seen as new
        // local appends and mass-broadcast to the joiner (the 2026-06-19 echo flood).
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
        // Positional diff under the append-at-tail invariant: surviving rows
        // keep their relative order; anything in the shadow that no longer
        // matches the array prefix was deleted; the array tail past the
        // matched prefix is new.
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
        // Local peer's slot for the "<OwnNick> deleted an email" peer-action line
        // (the subject is always a nickname). Computed once; the removed loop below
        // is the local-delete branch (a remote delete erased the shadow synchronously
        // in ApplyDeleteByHash, so it never appears here). Raw LocalPeerId: Announce
        // resolves the local slot to LocalNickname() itself (the old Unknown->0
        // forcing would misattribute a pre-assignment delete to the host under nick
        // rendering; host's id IS 0 already).
        const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
        size_t j = 0;
        std::vector<std::wstring> removedTopics;  // announces deferred past the bulk gate below
        for (ShadowRow& srow : g_shadow) {
            if (j < cur.size() && srow.key == cur[j]) {
                // move, not copy: the topic wstring copied per surviving row per poll
                // was ~2k allocs/s on a big save (audit 2026-07-10 LOW); g_shadow is
                // replaced by `next` wholesale below, so moving out of it is safe.
                next.push_back(std::move(srow));
                ++j;
            } else if (srow.hash != 0) {
                // hash 0 = was unreadable at shadow time (v65 audit I-2 parity:
                // the hash-unknown sentinel is not a wire key).
                removed.push_back(srow.hash);
                removedTopics.push_back(std::move(srow.topic));
            }
        }
        // Bulk-removed gate (2026-07-10, the append side's kBulkAppendThreshold made
        // symmetric): the delete-verb census proved the ONLY game code removing from
        // saveSlot.emails is ui_laptop.delEmail -- a PLAYER action -- so a bulk batch
        // of removals in ONE poll cannot be gameplay; it is a shadow desync (world
        // swap / array reset). Re-baseline silently: no announce, no EmailDelete
        // broadcast (each would misattribute + mass-delete the peers' inboxes).
        if (removed.size() > kBulkAppendThreshold) {
            UE_LOGW("email_sync: %zu rows removed in one poll -- shadow desync "
                    "re-baseline (adopted, NOT announced/broadcast)", removed.size());
            removed.clear();
            removedTopics.clear();
        }
        // Announce each user delete to the shared feed so the other peers learn who
        // deleted what. Only in an actual session (no point telling yourself solo).
        if (s->connected())
            for (const std::wstring& topic : removedTopics)
                coop::peer_action_feed::Announce(localSlot,
                                                 L"deleted an email: " + topic);
        // A bulk batch of new rows in ONE poll is a save load (history materializing),
        // not gameplay -- adopt it as baseline and never broadcast it (the joiner gets
        // history via the save transfer). Catches the case where the prime ran before
        // the real save loaded (count sat stable at a menu/partial value), which the
        // stabilize guard alone can miss. See kBulkAppendThreshold.
        const bool bulkLoad = (cur.size() - j) > kBulkAppendThreshold;
        if (bulkLoad)
            UE_LOGW("email_sync: %zu new rows in one poll -- save-load re-baseline "
                    "(adopted as sent, NOT broadcast)", cur.size() - j);
        for (; j < cur.size(); ++j) {
            ShadowRow srow;
            srow.key = cur[j];
            // Compute the row's STABLE content hash first -- the identity we correlate
            // a wire-applied row on (the per-process RowKey can drift; see AddEmail).
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
        // Disconnected deletes stay local-only: a later joiner converges via
        // the save transfer, and there is no peer to inform now.
    }

    // Retry parked deletes against the freshly-synced shadow.
    for (auto it = g_tombstones.begin(); it != g_tombstones.end();) {
        if (ApplyDeleteByHash(it->first)) it = g_tombstones.erase(it);
        else ++it;
    }

    // Broadcast pending appends in array order. HOST-ONLY (2026-07-09 authority
    // audit): email is host-owned world state -- every addEmail site is
    // world/story/system-authored (census), so the client is NOT an email-
    // distribution authority. Gating the send on role()==Host closes the shared-
    // inbox pollution vector (a client's diverged world-sim self-authoring a false
    // email -> broadcast -> permanent host row) AND the startup transient before a
    // client-side source-kill (e.g. serverbox's ticker_serverBreaker) latches.
    // Authority-direction parity with weather/time/serverbox. The client keeps ALL
    // apply + echo-proof bookkeeping (g_applied/prime/shadow/diff are load-bearing
    // for the mirror); only this send is gated -- its locally-authored sent=false
    // rows simply stay pending (never broadcast). Delete stays symmetric (a client
    // USER-delete must propagate; echo-proofed by the synchronous g_shadow erase).
    // Host rows stay pending while disconnected (audit I-1: dropping them would
    // silently skip rows produced before the first connect).
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
                // Seeds arc (design doc par.2.6): refused with ZERO world-ready
                // receivers = VACUOUS success -- every absent peer gets this row
                // via save+seed; retrying across a future ready edge was the
                // measured pre-existing DUPLICATE (audit I-1's hold-and-retry
                // delivered a row the joiner already loaded from its save).
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
        // A remote peer deleted this shared email -- surface who, to the local feed.
        coop::peer_action_feed::Announce(senderSlot, L"deleted an email: " + topic);
    } else {
        // Deferred (row not present yet / transient misalignment): the tombstone retry
        // in Tick applies it later WITHOUT an announce (senderSlot isn't carried on the
        // retry path -- a rare edge; the delete still converges).
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
    // Seeds arc: parks + seed brackets are session-scoped.
    g_applyPark.clear();
    SetParkBackpressure(false);
    g_seeder.Reset();
}

}  // namespace coop::email_sync
