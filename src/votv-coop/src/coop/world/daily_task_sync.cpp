// coop/world/daily_task_sync.cpp -- see the header + the L7 design doc (D3).

#include "coop/world/daily_task_sync.h"

#include "coop/net/session.h"

#include "ue_wrap/daily_task.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hot_path_guard.h"
#include "ue_wrap/log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace coop::daily_task_sync {
namespace {

namespace DT = ue_wrap::daily_task;
namespace GT = ue_wrap::game_thread;

constexpr uint64_t kPollMs = 1000;  // host change-hash cadence (writers fire a few times/day)

std::atomic<coop::net::Session*> g_session{nullptr};
bool     g_installLogged = false;
uint64_t g_nextPoll = 0;
uint64_t g_lastHash = 0;
bool     g_haveHash = false;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// FNV-1a over the payload bytes (the payload IS the canonical serialized form).
uint64_t HashPayload(const coop::net::TaskNewStatePayload& p) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(p); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// Serialize the live taskNew into the wire payload. SEND-side clamp + WARN
// (never silent truncation). Returns false while taskNew is unresolvable.
bool FillPayload(coop::net::TaskNewStatePayload& p) {
    DT::View v{};
    if (!DT::Read(v)) return false;
    std::memset(&p, 0, sizeof(p));
    p.active    = v.active ? 1 : 0;
    p.rewardSig = v.rewardSig;
    p.rewardSat = v.rewardSat;
    p.reelBig   = v.reelBig;
    p.reelSmall = v.reelSmall;
    auto fill = [](const int32_t* src, int32_t num, int16_t* dst, int32_t cap,
                   uint8_t& outCount, const char* name) {
        int32_t n = num;
        if (n > cap) {
            UE_LOGW("[task] %s count %d exceeds wire cap %d -- clamped (extend the cap!)",
                    name, n, cap);
            n = cap;
        }
        for (int32_t i = 0; i < n; ++i) {
            int32_t x = src[i];
            if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
            dst[i] = static_cast<int16_t>(x);
        }
        outCount = static_cast<uint8_t>(n);
    };
    fill(v.sigRequired,    v.sigRequiredNum,    p.sigRequired,    coop::net::kTaskSigCap,  p.sigRequiredCount,    "sigRequired");
    fill(v.sigCompleted,   v.sigCompletedNum,   p.sigCompleted,   coop::net::kTaskSigCap,  p.sigCompletedCount,   "sigCompleted");
    fill(v.requiredDishes, v.requiredDishesNum, p.requiredDishes, coop::net::kTaskDishCap, p.requiredDishesCount, "requiredDishes");
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_installLogged) {
        g_installLogged = true;
        UE_LOGI("[task] daily_task_sync installed (TaskNewState=103 host 1 Hz change-hash mirror)");
    }
}

void Tick() {
    UE_ASSERT_GAME_THREAD("daily_task_sync::Tick");
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const uint64_t now = NowMs();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollMs;
    coop::net::TaskNewStatePayload p{};
    if (!FillPayload(p)) return;  // taskNew unresolvable (menu / booting) -- retry next poll
    const uint64_t h = HashPayload(p);
    if (g_haveHash && h == g_lastHash) return;
    const bool first = !g_haveHash;
    g_lastHash = h;
    g_haveHash = true;
    if (first) return;  // baseline-on-first-sight: the joiner got this state in the save transfer
    s->SendReliable(coop::net::ReliableKind::TaskNewState, &p, sizeof(p));
    UE_LOGI("[task] taskNew changed -- broadcast (active=%u sigReq=%u sigDone=%u dishes=%u "
            "reelBig=%.1f reelSmall=%.1f)",
            p.active, p.sigRequiredCount, p.sigCompletedCount, p.requiredDishesCount,
            p.reelBig, p.reelSmall);
}

void OnTaskNewState(const coop::net::TaskNewStatePayload& p, uint8_t senderSlot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() == coop::net::Role::Host || senderSlot != 0) {
        UE_LOGW("[task] TaskNewState from slot=%u dropped (host-authored kind)", senderSlot);
        return;
    }
    if (p.sigRequiredCount > coop::net::kTaskSigCap ||
        p.sigCompletedCount > coop::net::kTaskSigCap ||
        p.requiredDishesCount > coop::net::kTaskDishCap) {
        UE_LOGW("[task] TaskNewState counts over cap (%u/%u/%u) -- dropped",
                p.sigRequiredCount, p.sigCompletedCount, p.requiredDishesCount);
        return;
    }
    const coop::net::TaskNewStatePayload cp = p;
    GT::Post([cp]() {
        // Widen i16 -> the engine's int32 arrays; ONE GT task = the whole apply is atomic
        // w.r.t. every BP reader (all synchronous -- censused: getSigObj, kerfurOmega.findTask).
        int32_t buf[64];
        auto apply = [&buf](DT::Which which, const int16_t* src, uint8_t count) {
            for (uint8_t i = 0; i < count; ++i) buf[i] = src[i];
            return DT::WriteArray(which, buf, count);
        };
        const bool okScalars = DT::WriteScalars(cp.active != 0, cp.rewardSig, cp.rewardSat,
                                                cp.reelBig, cp.reelSmall);
        const bool okA = apply(DT::Which::SigRequired,    cp.sigRequired,    cp.sigRequiredCount);
        const bool okB = apply(DT::Which::SigCompleted,   cp.sigCompleted,   cp.sigCompletedCount);
        const bool okC = apply(DT::Which::RequiredDishes, cp.requiredDishes, cp.requiredDishesCount);
        if (!okScalars || !okA || !okB || !okC) {
            UE_LOGW("[task] TaskNewState apply incomplete (scalars=%d arrays=%d/%d/%d) -- "
                    "taskNew unresolvable or an array rebuild failed; next broadcast retries",
                    okScalars, okA, okB, okC);
            return;
        }
        UE_LOGI("[task] taskNew mirrored (active=%u sigReq=%u sigDone=%u dishes=%u "
                "reelBig=%.1f reelSmall=%.1f)",
                cp.active, cp.sigRequiredCount, cp.sigCompletedCount, cp.requiredDishesCount,
                cp.reelBig, cp.reelSmall);
    });
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("daily_task_sync::OnDisconnect");
    g_nextPoll = 0;
    g_lastHash = 0;
    g_haveHash = false;
    UE_LOGI("[task] daily_task_sync reset (disconnect)");
}

}  // namespace coop::daily_task_sync
