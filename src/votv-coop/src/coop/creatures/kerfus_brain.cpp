// coop/creatures/kerfus_brain.cpp -- see coop/creatures/kerfus_brain.h.

#include "coop/creatures/kerfus_brain.h"

#include "coop/net/session.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <cstddef>

namespace coop::kerfus_brain {
namespace {

namespace UK = ue_wrap::kerfus;
namespace sg = ue_wrap::script_gate;


// The brain's bodies (p_kerfus.cpp, bp_cfg): the tick (energy drain and charge, the wheel's
// torque, the stuck jump), the two timer events that re-path and unstick it, the movement functions,
// the server job, the laptop's jump, the haunting, and the cord events, whose one effect is `charging`.
// The water events set only the body's damping, and upd() only its looks and sounds, so both stay.
constexpr const wchar_t* kBrain[] = {
    L"ReceiveTick", L"checkJump", L"updatePath", L"movePawnTo", L"findBrokenServer", L"task",
    L"jump", L"RCjump", L"possess", L"possessTimer", L"cordPlugged", L"cordUnplugged",
};
constexpr size_t kBrainCount = sizeof(kBrain) / sizeof(kBrain[0]);
constexpr int kTagBrainBase = 0x4B465300;  // 'KFS' + the row
constexpr int kTagServerFix = 0x4B4653FF;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched[kBrainCount] = {};
bool g_fixWatched = false;
bool g_saidLive = false;

// Per session: the refusals by row, and whether a row's first refusal was said.
unsigned long long g_refused[kBrainCount] = {};
bool g_said[kBrainCount] = {};
unsigned long long g_fixRefused = 0;

bool OnClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

sg::Verdict OnBrainPre(const sg::Call& c) {
    if (!OnClient()) return sg::Verdict::Run;
    const size_t row = static_cast<size_t>(c.tag - kTagBrainBase);
    if (row < kBrainCount) {
        ++g_refused[row];
        if (!g_said[row]) {
            g_said[row] = true;
            UE_LOGI("kerfus_brain: refused the Kerfus's %ls on this client -- the host runs its brain (said once "
                    "a session)", kBrain[row]);
        }
    }
    return sg::Verdict::Cancel;
}

// A server box's fix() called from a Kerfus's own frame. The server job's bodies are refused above,
// so this fires only for one that began before its watch went live; the fix is the host's to make.
sg::Verdict OnServerFixPre(const sg::Call& c) {
    if (!OnClient() || !c.callerObject) return sg::Verdict::Run;
    if (!UK::IsKerfus(c.callerObject)) return sg::Verdict::Run;
    ++g_fixRefused;
    UE_LOGW("kerfus_brain: refused a server fix() from a Kerfus's own body on this client (#%llu) -- a server "
            "job that began before the brain's watch went live", g_fixRefused);
    return sg::Verdict::Cancel;
}

void Register() {
    for (size_t i = 0; i < kBrainCount; ++i)
        if (!g_watched[i])
            g_watched[i] = sg::WatchClassName(UK::kClassName, kBrain[i], kTagBrainBase + static_cast<int>(i),
                                              &OnBrainPre, nullptr);
    if (!g_fixWatched)
        g_fixWatched = sg::WatchClassName(L"serverBox_C", L"fix", kTagServerFix, &OnServerFixPre, nullptr);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
}

void Tick() {
    if (g_saidLive) return;
    Register();
    sg::ResolvePendingNames();
    for (size_t i = 0; i < kBrainCount; ++i)
        if (!g_watched[i] || !sg::ClassNameWatchLive(UK::kClassName, kBrain[i], kTagBrainBase + static_cast<int>(i)))
            return;
    if (!g_fixWatched || !sg::ClassNameWatchLive(L"serverBox_C", L"fix", kTagServerFix)) return;
    g_saidLive = true;
    UE_LOGI("kerfus_brain: the Kerfus's %zu brain bodies and the server-fix guard are watched -- a client refuses "
            "them", kBrainCount);
}

void OnDisconnect() {
    unsigned long long total = g_fixRefused;
    for (size_t i = 0; i < kBrainCount; ++i) total += g_refused[i];
    if (total)
        UE_LOGI("kerfus_brain: session end -- %llu brain bodies refused on this client (tick %llu, server fixes %llu)",
                total, g_refused[0], g_fixRefused);
    for (size_t i = 0; i < kBrainCount; ++i) { g_refused[i] = 0; g_said[i] = false; }
    g_fixRefused = 0;
}

}  // namespace coop::kerfus_brain
