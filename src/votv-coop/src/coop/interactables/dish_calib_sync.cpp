// coop/interactables/dish_calib_sync.cpp -- see coop/interactables/dish_calib_sync.h.

#include "coop/interactables/dish_calib_sync.h"

#include "coop/net/intent_bucket.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/desk/dish_writers.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace coop::dish_calib_sync {
namespace {

namespace D = ue_wrap::dish;
namespace DW = ue_wrap::dish_writers;
namespace P = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;
using coop::net::kMaxDishes;

constexpr int kTagTool = 0x44435754;          // 'DCWT'
constexpr int kTagUncalibrator = 0x44435755;  // 'DCWU'
constexpr int kTagSetPrec = 0x44435350;       // 'DCSP'

// A client's intents, per peer: a player's verbs come a few a second at most.
constexpr coop::net::IntentBudget kIntentBudget{8.f, 4.f};
constexpr uint64_t kRefusalSayMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};

// HOST: what it last sent, the baseline its poll diffs against.
float g_sent[kMaxDishes] = {};
bool  g_haveSent = false;
// CLIENT: the host's values it holds, each dish from the seed or a batch, or from this player's own verb, for the
// session.
float g_host[kMaxDishes] = {};
bool  g_held[kMaxDishes] = {};
Counts g_counts;

struct Bucket {
    coop::net::IntentBucket rate;
    uint64_t                nextSayMs = 0;
    int                     overBudget = 0, notFinite = 0, noDish = 0;  // refused since the last line said so
};
Bucket g_budget[coop::net::kMaxPeers];

// A stored value changed: bit for bit, so a NaN the host's tool wrote is one value, not a change every poll.
bool Same(float a, float b) {
    uint32_t x = 0, y = 0;
    std::memcpy(&x, &a, sizeof(x));
    std::memcpy(&y, &b, sizeof(y));
    return x == y;
}

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() != coop::net::Role::Host;
}

// HOST: the refusals a slot's intents drew since the line last said them, by reason; once per kRefusalSayMs, from the
// intent that draws one and from the host's poll, so the last of a burst is said too, and at once when `force` is set.
void SayRefusals(uint8_t slot, uint64_t now, bool force) {
    Bucket& b = g_budget[slot];
    const int n = b.overBudget + b.notFinite + b.noDish;
    if (n == 0 || (!force && now < b.nextSayMs)) return;
    b.nextSayMs = now + kRefusalSayMs;
    UE_LOGW("dish_calib_sync: HOST refused %d of slot %u's dish precision(s) since its last line: %d past its intent "
            "budget, %d not a finite value, %d no live dish -- a live dish named is answered at the host's value, to "
            "that client alone when nothing was performed", n, static_cast<unsigned>(slot), b.overBudget, b.notFinite,
            b.noDish);
    b.overBudget = b.notFinite = b.noDish = 0;
}

bool TakeToken(uint8_t slot) { return g_budget[slot].rate.Take(kIntentBudget, ::GetTickCount64()); }

void SendAll(coop::net::Session* s, const D::DishCalibration* rows, int32_t n, int toSlot) {
    coop::net::DishCalibPayload p{};
    for (int32_t i = 0; i < n; ++i) {
        if (rows[i].index < 0 || rows[i].index >= kMaxDishes) continue;
        auto& e = p.entries[p.count++];
        e.index = static_cast<uint8_t>(rows[i].index);
        e.value = rows[i].value;
    }
    if (p.count == 0) return;
    if (toSlot < 0) s->SendReliable(coop::net::ReliableKind::DishCalib, &p, sizeof(p));
    else s->SendReliableToSlot(toSlot, coop::net::ReliableKind::DishCalib, &p, sizeof(p));
}

// CLIENT: every held dish that moved off the host's value is put back to it.
void PutBack(const D::DishCalibration* rows, int32_t n) {
    int reverted = 0;
    for (int32_t i = 0; i < n; ++i) {
        const int32_t idx = rows[i].index;
        if (idx < 0 || idx >= kMaxDishes || !g_held[idx] || Same(rows[i].value, g_host[idx])) continue;
        if (D::WriteCalibration(idx, g_host[idx])) ++reverted;
    }
    if (reverted == 0) return;
    g_counts.putBack += static_cast<uint64_t>(reverted);
    UE_LOGI("dish_calib_sync: CLIENT %d dish precision(s) put back to the host's (n=%llu)", reverted,
            static_cast<unsigned long long>(g_counts.putBack));
}

// CLIENT: setPrec averages the dishes into the rate the desk's download reads every tick (its speed text, its
// completion test), so the copy is put back first; a deviation the poll has not reached yet would otherwise stand
// in that rate for the timer's whole ten seconds.
sg::Verdict OnSetPrecPre(const sg::Call&) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return sg::Verdict::Run;
    D::DishCalibration rows[kMaxDishes];
    PutBack(rows, D::ReadCalibrations(rows, kMaxDishes));
    return sg::Verdict::Run;
}

// The two verbs, each body's entry reading every dish and its exit sending the ones it changed. A body can nest in
// another watched body, so after a send every enclosing body's reading takes the values as sent, and a change is
// sent once. The gate's own chain is the scope: an entry whose body ended without its exit (another watcher's Cancel,
// a fault the firewall absorbed) is dropped at the next entry at its depth or shallower, at an enclosing body's exit,
// and at any verb's entry once no body of its function is on the chain.
struct InFlight {
    int   depth;
    void* stack;
    void* function;
    float before[kMaxDishes];
    bool  read[kMaxDishes];
};
std::vector<InFlight> g_inFlight;  // game thread only

void DropFrom(int depth) {
    while (!g_inFlight.empty() && g_inFlight.back().depth >= depth) g_inFlight.pop_back();
}

// At a body's entry the gate's chain holds only the bodies around it (its own scope is pushed after the entry
// callbacks), so an entry whose function no body on the chain runs ended without its exit.
void DropEnded() {
    g_inFlight.erase(std::remove_if(g_inFlight.begin(), g_inFlight.end(),
                                    [](const InFlight& f) { return !sg::IsBodyActive(f.function); }),
                     g_inFlight.end());
}

sg::Verdict OnVerbPre(const sg::Call& call) {
    DropFrom(call.depth);
    DropEnded();
    // A verb on an object of a world this client has not announced ready is not a player's verb on the shared one.
    if (!IsClient(g_session.load(std::memory_order_acquire)) || !coop::net_pump::IsInAnnouncedWorld(call.object))
        return sg::Verdict::Run;
    D::DishCalibration rows[kMaxDishes];
    int32_t n = D::ReadCalibrations(rows, kMaxDishes);
    if (n <= 0) return sg::Verdict::Run;
    // The verb is judged against the host's values, as setPrec's PRE reads them: a deviation the poll has not put back
    // yet would otherwise hide a verb that writes exactly it. Not inside another verb, whose change is not sent yet.
    if (g_inFlight.empty()) {
        PutBack(rows, n);
        n = D::ReadCalibrations(rows, kMaxDishes);
    }
    InFlight f{call.depth, call.stack, call.function, {}, {}};
    for (int32_t i = 0; i < n; ++i) {
        if (rows[i].index < 0 || rows[i].index >= kMaxDishes) continue;
        f.before[rows[i].index] = rows[i].value;
        f.read[rows[i].index] = true;
    }
    g_inFlight.push_back(f);
    return sg::Verdict::Run;
}

void OnVerbPost(const sg::Call& call) {
    DropFrom(call.depth + 1);
    if (g_inFlight.empty() || g_inFlight.back().depth != call.depth || g_inFlight.back().stack != call.stack)
        return;  // its entry read nothing
    const InFlight f = g_inFlight.back();
    g_inFlight.pop_back();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s)) return;
    D::DishCalibration rows[kMaxDishes];
    const int32_t n = D::ReadCalibrations(rows, kMaxDishes);
    coop::net::DishCalibPayload p{};
    for (int32_t i = 0; i < n; ++i) {
        const int32_t idx = rows[i].index;
        if (idx < 0 || idx >= kMaxDishes || !f.read[idx] || Same(rows[i].value, f.before[idx])) continue;
        auto& e = p.entries[p.count++];
        e.index = static_cast<uint8_t>(idx);
        e.value = rows[i].value;
        for (InFlight& outer : g_inFlight) outer.before[idx] = rows[i].value;
    }
    if (p.count == 0) return;
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::DishCalibIntent, &p, sizeof(p))) {
        UE_LOGW("dish_calib_sync: CLIENT %u dish precision(s) this player set were not sent (the session refused "
                "them); the host's values stand", static_cast<unsigned>(p.count));
        return;
    }
    // Held once sent: the host performs it and answers with it, or answers with its own value in its place.
    for (uint8_t i = 0; i < p.count; ++i) {
        g_host[p.entries[i].index] = p.entries[i].value;
        g_held[p.entries[i].index] = true;
    }
    g_counts.intentsSent += p.count;
    UE_LOGI("dish_calib_sync: CLIENT this player's verb set %u dish precision(s), dish %u = %.4f first -- intent to "
            "the host", static_cast<unsigned>(p.count), static_cast<unsigned>(p.entries[0].index),
            p.entries[0].value);
}

// A watch's registration, once a process, and its one settle line.
enum class Reg : uint8_t { Pending, Registered, Refused };
UE_FACT_FN(kRowToolInit, "dish_calib_sync", L"tool_setDishCalibration_C", L"init", Declared);
UE_FACT_FN(kRowPlayerHandUseLmb, "dish_calib_sync", L"prop_uncalibrator_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowSetPrec, "dish_calib_sync", L"mainGamemode_C", L"setPrec", Declared);

struct Watch {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
    Reg reg = Reg::Pending;
    bool settled = false;
};
Watch g_watches[] = {
    {&kRowToolInit, kTagTool, &OnVerbPre, &OnVerbPost},
    {&kRowPlayerHandUseLmb, kTagUncalibrator, &OnVerbPre, &OnVerbPost},
    {&kRowSetPrec, kTagSetPrec, &OnSetPrecPre, nullptr},
};

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    for (Watch& w : g_watches) {
        if (w.reg != Reg::Pending) continue;
        if (sg::WatchName(*w.row, w.tag, w.pre, w.post)) {
            w.reg = Reg::Registered;
            continue;
        }
        w.reg = Reg::Refused;  // the gate refuses a watch it cannot keep for the process, an uninstalled gate included
    }
}

void Tick() {
    bool pending = false;
    for (Watch& w : g_watches) pending |= !w.settled && w.reg != Reg::Pending;
    if (!pending) return;
    sg::ResolvePendingNames();
    for (Watch& w : g_watches) {
        if (w.settled || w.reg == Reg::Pending) continue;
        if (w.reg == Reg::Registered && sg::NameWatchLive(*w.row, w.tag)) {
            w.settled = true;
            UE_LOGI("dish_calib_sync: the watch on %ls::%ls is live", w.row->Class(), w.row->Name());
        } else if (w.reg == Reg::Refused || sg::NameWatchSettled(*w.row, w.tag)) {
            w.settled = true;
            UE_LOGE("dish_calib_sync: the watch on %ls::%ls is dead -- %ls", w.row->Class(), w.row->Name(),
                    w.pre == &OnSetPrecPre ? L"a client's download rate can read a deviation for up to ten seconds"
                                           : L"a client's use of it is put back and never reaches the host");
        }
    }
}

// HOST: the changed dishes to every client, and all of them when the baseline primes (the session's first poll, or a
// desk reload on the host). CLIENT: the put-back.
void Poll(coop::net::Session* s) {
    D::DishCalibration rows[kMaxDishes];
    const int32_t n = D::ReadCalibrations(rows, kMaxDishes);
    if (n <= 0) return;
    if (s->role() != coop::net::Role::Host) {
        PutBack(rows, n);
        return;
    }
    const uint64_t now = ::GetTickCount64();
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) SayRefusals(slot, now, false);
    if (!g_haveSent) {
        for (int32_t i = 0; i < n; ++i)
            if (rows[i].index >= 0 && rows[i].index < kMaxDishes) g_sent[rows[i].index] = rows[i].value;
        g_haveSent = true;
        SendAll(s, rows, n, -1);
        return;
    }
    coop::net::DishCalibPayload p{};
    for (int32_t i = 0; i < n; ++i) {
        const int32_t idx = rows[i].index;
        if (idx < 0 || idx >= kMaxDishes || Same(rows[i].value, g_sent[idx])) continue;
        auto& e = p.entries[p.count++];
        e.index = static_cast<uint8_t>(idx);
        e.value = rows[i].value;
        g_sent[idx] = rows[i].value;
    }
    if (p.count > 0) s->SendReliable(coop::net::ReliableKind::DishCalib, &p, sizeof(p));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || !D::EnsureResolved()) return;
    D::DishCalibration rows[kMaxDishes];
    SendAll(s, rows, D::ReadCalibrations(rows, kMaxDishes), peerSlot);
}

void OnDishCalib(const coop::net::DishCalibPayload& p) {
    if (!g_session.load(std::memory_order_acquire)) return;
    // The host authors every dish's precision, and its receive edge refuses a client's batch (client_authorship.h).
    // Held as the host's values even before this copy's dishes resolve; the poll puts them in once they do.
    const bool resolved = D::EnsureResolved();
    const int32_t n = p.count <= kMaxDishes ? p.count : kMaxDishes;
    for (int32_t i = 0; i < n; ++i) {
        const auto& e = p.entries[i];
        if (e.index >= kMaxDishes) continue;
        g_host[e.index] = e.value;
        g_held[e.index] = true;
        if (resolved) D::WriteCalibration(e.index, e.value);
    }
}

// HOST: performs what it can of a client's intent and answers with every live dish named, as it now holds it, read
// after the writes. Once anything was performed the answer goes to all, as the server box's upgrade lane broadcasts its
// canonical after an applied op (server_upgrade_sync.cpp:301) and MTA a confirmed request (CGame.cpp:3196-3197); a
// refused entry rides along, harmless, since every other client already holds that value. When nothing was
// performed it goes to the author alone, as both answer a refusal (server_upgrade_sync.cpp:255;
// CGame.cpp:3042-3043), so the budget bounds what a client can make the host write and send to the others; its
// author hears at most one answer per intent. A slot not yet in its world is not heard, as MTA hears a joined player
// only (CGame.cpp:3024-3025): its announce is pinned to this lane, ahead of any intent (session_lanes.h).
void OnDishCalibIntent(const coop::net::DishCalibPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || senderSlot == 0 || senderSlot >= coop::net::kMaxPeers) return;
    Bucket& b = g_budget[senderSlot];
    const uint64_t now = ::GetTickCount64();
    const bool worldReady = s->IsSlotWorldReady(senderSlot);
    if (!worldReady || !D::EnsureResolved()) {
        if (now >= b.nextSayMs) {
            b.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("dish_calib_sync: slot %u's precision intent dropped -- %s", static_cast<unsigned>(senderSlot),
                    worldReady ? "the dishes are not resolved here" : "that slot has not announced its world");
        }
        return;
    }
    const bool within = TakeToken(senderSlot);
    D::DishCalibration rows[kMaxDishes];
    int32_t m = D::ReadCalibrations(rows, kMaxDishes);
    bool named[kMaxDishes] = {};
    int performed = 0, overBudget = 0, notFinite = 0, noDish = 0;
    const int32_t n = p.count <= kMaxDishes ? p.count : kMaxDishes;
    for (int32_t i = 0; i < n; ++i) {
        const auto& e = p.entries[i];
        bool live = false;
        for (int32_t j = 0; j < m && !live; ++j) live = rows[j].index == e.index;
        if (e.index >= kMaxDishes || !live) {  // the lane's index rule, and a dish this copy has
            ++noDish;
            continue;
        }
        named[e.index] = true;
        if (!within) {
            ++overBudget;
        } else if (!std::isfinite(e.value)) {
            ++notFinite;
        } else if (D::WriteCalibration(e.index, e.value)) {
            ++performed;
            g_counts.lastIndex = e.index;
            g_counts.lastValue = e.value;
        } else {
            ++noDish;
        }
    }
    const int refused = overBudget + notFinite + noDish;
    g_counts.intentsApplied += static_cast<uint64_t>(performed);
    g_counts.intentsRefused += static_cast<uint64_t>(refused);
    coop::net::DishCalibPayload out{};
    m = D::ReadCalibrations(rows, kMaxDishes);
    for (int32_t j = 0; j < m; ++j) {
        const int32_t idx = rows[j].index;
        if (idx < 0 || idx >= kMaxDishes || !named[idx]) continue;
        auto& e = out.entries[out.count++];
        e.index = static_cast<uint8_t>(idx);
        e.value = rows[j].value;
        if (performed > 0) g_sent[idx] = rows[j].value;  // said to all: the poll need not say it again
    }
    if (out.count > 0) {
        if (performed > 0) s->SendReliable(coop::net::ReliableKind::DishCalib, &out, sizeof(out));
        else s->SendReliableToSlot(senderSlot, coop::net::ReliableKind::DishCalib, &out, sizeof(out));
    }
    if (performed > 0)
        UE_LOGI("dish_calib_sync: HOST performed %d of slot %u's dish precision(s), dish %d = %.4f last", performed,
                static_cast<unsigned>(senderSlot), g_counts.lastIndex, g_counts.lastValue);
    b.overBudget += overBudget;
    b.notFinite += notFinite;
    b.noDish += noDish;
    SayRefusals(senderSlot, now, false);
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    SayRefusals(slot, ::GetTickCount64(), true);
    g_budget[slot] = Bucket{};
}

void OnDeskReplaced() {
    g_haveSent = false;
    g_inFlight.clear();
}

void OnSessionEnd() {
    g_haveSent = false;
    for (bool& h : g_held) h = false;
    g_inFlight.clear();
    g_counts = Counts{};
    for (Bucket& b : g_budget) b = Bucket{};
}

Counts LaneCounts() { return g_counts; }

}  // namespace coop::dish_calib_sync
