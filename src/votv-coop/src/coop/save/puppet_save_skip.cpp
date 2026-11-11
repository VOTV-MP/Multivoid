// coop/save/puppet_save_skip.cpp -- see coop/save/puppet_save_skip.h.

#include "coop/save/puppet_save_skip.h"

#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::puppet_save_skip {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr int kTag = 0x50535330;  // 'PSS0'

UE_FACT_FN(kRowIgnoreSave, "puppet_save_skip", L"mainPlayer_C", L"ignoreSave", Declared);

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;
std::atomic<uint64_t> g_answered{0};

// The body only writes its out parameter, so answering it and cancelling the body is the whole call.
sg::Verdict OnIgnoreSavePre(const sg::Call& c) {
    if (!c.object || !coop::players::Registry::Get().IsPuppetBody(c.object)) return sg::Verdict::Run;
    static void*   sFn = nullptr;
    static int32_t sOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sOff = R::FindParamOffset(c.function, L"ignoreSave");
    }
    uint8_t* const out = sOff >= 0 ? sg::OutParamPtr(c, sOff) : nullptr;
    if (!out) return sg::Verdict::Run;
    *reinterpret_cast<bool*>(out) = true;
    if (g_answered.fetch_add(1, std::memory_order_relaxed) == 0)
        UE_LOGI("puppet_save_skip: a puppet answered ignoreSave=true -- the world save leaves it out");
    return sg::Verdict::Cancel;
}

}  // namespace

void Install() {
    if (g_settled) return;
    if (g_reg == Reg::Pending)
        g_reg = sg::WatchName(kRowIgnoreSave, kTag, &OnIgnoreSavePre, nullptr) ? Reg::Registered : Reg::Refused;
    if (g_reg == Reg::Registered) sg::ResolvePendingNames();
    if (g_reg == Reg::Registered && sg::NameWatchLive(kRowIgnoreSave, kTag)) {
        g_settled = true;
        UE_LOGI("puppet_save_skip: the ignoreSave watch on mainPlayer_C is LIVE");
    } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowIgnoreSave, kTag)) {
        g_settled = true;
        UE_LOGE("puppet_save_skip: the ignoreSave watch is dead -- the host's save gains a record per puppet");
    }
}

uint64_t AnsweredCount() { return g_answered.load(std::memory_order_relaxed); }

}  // namespace coop::puppet_save_skip
