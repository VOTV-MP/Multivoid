// coop/interactables/dish_hashcode_sync.cpp -- see coop/interactables/dish_hashcode_sync.h.
//
// The blob: [u8 rows] then per row [u8 dish index][u32 chars + UTF-16LE code], through the save
// record's string codec, whose reads are bounded; a code is nine lines, and the UTF-8 codec drops the
// control characters between them.

#include "coop/interactables/dish_hashcode_sync.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::dish_hashcode_sync {
namespace {

namespace D  = ue_wrap::dish;
namespace GT = ue_wrap::game_thread;
namespace W  = coop::save_record_wire;
namespace sg = ue_wrap::script_gate;

using Clock = std::chrono::steady_clock;

UE_FACT_FN(kRowGenerteHashcode, "dish_hashcode_sync", L"dish_C", L"generteHashcode", Declared);
constexpr int kTagRoll = 0x44484301;  // 'DHC' 1
constexpr int kMaxDishIndex = 64;     // gamemode.dishs holds 24 on this map; the digest reads up to 64
constexpr auto kRetry = std::chrono::seconds(1);

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
bool g_refusedWatch = false;
bool g_settled = false;

// HOST: the dishes whose code the rollover rewrote since the last send, and the joiners owed a set.
uint64_t g_marked = 0;
bool g_owed[coop::net::kMaxPeers] = {};
Clock::time_point g_nextSend{};
uint32_t g_seq = 1;
bool g_saidUnindexed = false;
bool g_saidUnreadable = false;

// CLIENT: rows whose write failed (the dish not resolved yet), by index, the newest kept; tried every tick,
// since a joined client's digest is read the moment its join completes.
std::vector<std::pair<int32_t, std::wstring>> g_waiting;
coop::blob_chunks::Assembler g_asm;
Clock::time_point g_nextSweep{};
uint64_t g_refused = 0;

coop::net::Session* SessionIf(coop::net::Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == role) ? s : nullptr;
}

// A client's own call: the codes are the host's, and a call the census did not find is named.
sg::Verdict OnRollPre(const sg::Call& c) {
    if (!SessionIf(coop::net::Role::Client)) return sg::Verdict::Run;
    if (g_refused++ == 0)
        UE_LOGW("dish_hashcode_sync: this client's own generteHashcode on dish %d refused -- the codes are the "
                "host's (caller %p)", D::IndexOf(c.object), c.callerObject);
    return sg::Verdict::Cancel;
}

void OnRollPost(const sg::Call& c) {
    if (!SessionIf(coop::net::Role::Host)) return;
    const int32_t i = D::IndexOf(c.object);
    if (i < 0 || i >= kMaxDishIndex) {
        if (!g_saidUnindexed) {
            g_saidUnindexed = true;
            UE_LOGW("dish_hashcode_sync: generteHashcode ran on %p, which gamemode.dishs does not hold -- not sent",
                    c.object);
        }
        return;
    }
    g_marked |= 1ull << i;
}

bool Encode(uint64_t which, std::vector<uint8_t>& blob, int& rows) {
    blob.clear();
    rows = 0;
    std::vector<std::pair<int32_t, std::wstring>> codes;
    for (int32_t i = 0; i < kMaxDishIndex; ++i) {
        if (!(which & (1ull << i))) continue;
        std::wstring code;
        if (D::ReadHashcode(i, code)) {
            codes.emplace_back(i, std::move(code));
        } else if (!g_saidUnreadable) {
            g_saidUnreadable = true;
            UE_LOGW("dish_hashcode_sync: dish %d's code could not be read (unresolved, or past %d characters) -- "
                    "not sent", i, D::kMaxHashcodeChars);
        }
    }
    if (codes.empty()) return false;
    W::AppU8(blob, static_cast<uint8_t>(codes.size()));
    for (const auto& [i, code] : codes) {
        W::AppU8(blob, static_cast<uint8_t>(i));
        W::AppWStr(blob, code);
    }
    rows = static_cast<int>(codes.size());
    return blob.size() <= coop::blob_chunks::MaxBlobBytes();
}

uint64_t AllDishes() {
    const int32_t n = D::Count();
    if (n <= 0) return 0;
    return n >= kMaxDishIndex ? ~0ull : (1ull << n) - 1;
}

bool AnyWorldReady(coop::net::Session* s) {
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (s->IsSlotWorldReady(i)) return true;
    return false;
}

// A joiner's whole set. False leaves it owed, retried a second later.
bool SendSetTo(coop::net::Session* s, int slot) {
    std::vector<uint8_t> blob;
    int rows = 0;
    if (!Encode(AllDishes(), blob, rows) ||
        !coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::DishHashcodes, g_seq++, blob))
        return false;
    UE_LOGI("dish_hashcode_sync: HOST sent slot %d every dish code (%d, %zu B)", slot, rows, blob.size());
    return true;
}

void TickHost(coop::net::Session* s) {
    const auto now = Clock::now();
    if (now < g_nextSend) return;
    if (g_marked) {
        // A joiner whose world is not up is skipped by the broadcast and gets every code at its world-ready.
        if (!AnyWorldReady(s)) {
            g_marked = 0;
        } else {
            std::vector<uint8_t> blob;
            int rows = 0;
            if (!Encode(g_marked, blob, rows)) {
                g_marked = 0;
            } else if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::DishHashcodes, g_seq++, blob)) {
                D::HashDigest hd{};
                D::ReadHashDigest(hd);
                UE_LOGI("dish_hashcode_sync: HOST sent %d dish code(s), %zu B; this host's digest %016llx", rows,
                        blob.size(), static_cast<unsigned long long>(hd.digest));
                g_marked = 0;
            } else {
                g_nextSend = now + kRetry;
                UE_LOGW("dish_hashcode_sync: %d dish code(s) not taken whole -- sent again in a second", rows);
                return;
            }
        }
    }
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (!g_owed[slot]) continue;
        if (!SendSetTo(s, slot)) {
            g_nextSend = now + kRetry;
            return;
        }
        g_owed[slot] = false;
    }
}

// False while the dish is not there to take the row.
bool Apply(int32_t index, const std::wstring& code) { return D::WriteHashcode(index, code); }

void Wait(int32_t index, std::wstring code) {
    for (auto& w : g_waiting) {
        if (w.first == index) {
            w.second = std::move(code);
            return;
        }
    }
    g_waiting.emplace_back(index, std::move(code));
}

void TickClient() {
    const auto now = Clock::now();
    if (!g_asm.Idle() && now >= g_nextSweep) {
        g_nextSweep = now + kRetry;
        g_asm.Sweep(now, std::chrono::seconds(10));
    }
    if (g_waiting.empty()) return;
    size_t applied = 0;
    for (auto it = g_waiting.begin(); it != g_waiting.end();) {
        if (Apply(it->first, it->second)) {
            it = g_waiting.erase(it);
            ++applied;
        } else {
            ++it;
        }
    }
    if (applied)
        UE_LOGI("dish_hashcode_sync: CLIENT wrote %zu waiting dish code(s), %zu still waiting", applied,
                g_waiting.size());
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched || g_refusedWatch || !sg::IsInstalled()) return;
    g_watched = sg::WatchName(kRowGenerteHashcode, kTagRoll, &OnRollPre, &OnRollPost);
    if (!g_watched) {
        g_refusedWatch = true;
        UE_LOGE("dish_hashcode_sync: the gate refused the generteHashcode watch -- the codes do not cross");
    }
}

void Tick() {
    // Driven until the watch settles, live or dead for good, and said once either way.
    if (!g_settled && g_watched) {
        sg::ResolvePendingNames();
        if (sg::NameWatchSettled(kRowGenerteHashcode, kTagRoll)) {
            g_settled = true;
            if (sg::NameWatchLive(kRowGenerteHashcode, kTagRoll))
                UE_LOGI("dish_hashcode_sync: generteHashcode is watched -- a host sends the codes, a client "
                        "refuses its own");
            else
                UE_LOGE("dish_hashcode_sync: the generteHashcode watch died at its name's resolve -- the codes do "
                        "not cross");
        }
    }
    if (auto* s = SessionIf(coop::net::Role::Host)) TickHost(s);
    else if (SessionIf(coop::net::Role::Client)) TickClient();
}

void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    auto* s = g_session.load(std::memory_order_acquire);
    if (!GT::IsGameThread() || !s || !s->running()) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    size_t o = 0;
    uint8_t rows = 0;
    if (!W::RdU8(blob, o, rows)) return;
    int written = 0, waiting = 0;
    for (uint8_t r = 0; r < rows; ++r) {
        uint8_t index = 0;
        std::wstring code;
        if (!W::RdU8(blob, o, index) || !W::RdWStr(blob, o, code) || index >= kMaxDishIndex ||
            code.size() > static_cast<size_t>(D::kMaxHashcodeChars)) {
            UE_LOGW("dish_hashcode_sync: a malformed row %u of %u -- the rest of the blob dropped",
                    static_cast<unsigned>(r), static_cast<unsigned>(rows));
            break;
        }
        if (Apply(index, code)) {
            ++written;
        } else {
            Wait(index, std::move(code));
            ++waiting;
        }
    }
    D::HashDigest hd{};
    D::ReadHashDigest(hd);
    UE_LOGI("dish_hashcode_sync: CLIENT wrote %d of %u dish code(s) from the host (%d waiting for their dish); "
            "digest %016llx, %d of %d filled", written, static_cast<unsigned>(rows), waiting,
            static_cast<unsigned long long>(hd.digest), hd.filled, hd.dishes);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = SessionIf(coop::net::Role::Host);
    if (!s || peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return;
    g_owed[peerSlot] = !SendSetTo(s, peerSlot);
    if (g_owed[peerSlot])
        UE_LOGW("dish_hashcode_sync: slot %d's set did not go whole at its world-ready -- sent again from the tick",
                peerSlot);
}

void OnPeerGone(uint8_t slot) {
    g_asm.ClearSlot(slot);
    if (slot < coop::net::kMaxPeers) g_owed[slot] = false;
}

void OnDisconnect() {
    g_marked = 0;
    for (bool& o : g_owed) o = false;
    g_waiting.clear();
    g_asm.Clear();
    g_nextSend = g_nextSweep = {};
    g_saidUnindexed = g_saidUnreadable = false;
    if (g_refused)
        UE_LOGI("dish_hashcode_sync: session end -- %llu generteHashcode call(s) of this client's own refused",
                static_cast<unsigned long long>(g_refused));
    g_refused = 0;
}

}  // namespace coop::dish_hashcode_sync
