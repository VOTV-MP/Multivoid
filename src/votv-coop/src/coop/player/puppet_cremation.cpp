// coop/player/puppet_cremation.cpp -- see coop/player/puppet_cremation.h.
//
// The cremator's tick (cremator_C, per collected actor): burn-health -= dt * windup; at <= 0, an actor that
// implements int_objects is asked crematorAction(&action). action == true returns from the tick; false adds the
// actor's fuel to gasAmount and then destroys it. A puppet answers true: its cremation is its owner's, as its
// damage and its fire already are (coop/player/player_damage.cpp). The answer is the game's own interface, one
// call before anything is changed.
//
// Side effect, the game's own semantics for that answer: the tick returns at the puppet's index, so while the
// puppet sits in the firebox at burn-health <= 0, the actors after it in the cremator's list are not consumed.
//
// Why not at the destroy: the tick adds the actor's fuel to gasAmount (saved) BEFORE it calls K2_DestroyActor and
// returns after it, so a refused destroy leaves the puppet at health <= 0 in the list and every following frame
// adds its fuel again and retries the destroy. The question is upstream of both.

#include "coop/player/puppet_cremation.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::puppet_cremation {
namespace {

namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowCrematorAction, "puppet_cremation", L"mainPlayer_C", L"crematorAction", Declared);
constexpr int kTag = 0x50435243;  // 'PCRC'

bool g_watched = false;
std::atomic<uint32_t> g_answered{0};

sg::Verdict OnCrematorActionPre(const sg::Call& c) {
    // IsPuppet alone, the test the ignite refusal uses: no session or local-pawn early-out in front of it.
    if (!c.object || !coop::players::Registry::Get().IsPuppet(c.object)) return sg::Verdict::Run;
    static void*   sFn = nullptr;
    static int32_t sOutOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sOutOff = R::FindParamOffset(c.function, L"action");
    }
    uint8_t* out = sOutOff >= 0 ? sg::OutParamPtr(c, sOutOff) : nullptr;
    if (!out) {
        static std::atomic<bool> sSaid{false};
        if (!sSaid.exchange(true, std::memory_order_relaxed))
            UE_LOGE("puppet_cremation: crematorAction has no 'action' out parameter here (offset %d) -- a cremator "
                    "destroys a peer's puppet", sOutOff);
        return sg::Verdict::Run;
    }
    *out = 1;  // a bool out parameter
    const uint32_t n = ++g_answered;
    // Said once per (puppet, cremator): the tick asks again every frame while the puppet sits at burn-health <= 0
    // (measured: 60 a second, for as long as it stays), so a periodic line would run for that whole stay. The key is
    // the two addresses, so a recycled address (a new puppet in the same slot, same cremator) can suppress one
    // line: log-only, accepted.
    static void* sSaidPuppet = nullptr;
    static void* sSaidCremator = nullptr;
    if (c.object != sSaidPuppet || c.callerObject != sSaidCremator) {
        sSaidPuppet = c.object;
        sSaidCremator = c.callerObject;
        UE_LOGI("puppet_cremation: a peer's puppet %p answered cremator %p true (#%u in all) -- its cremation is its "
                "owner's", c.object, c.callerObject, n);
    }
    return sg::Verdict::Cancel;  // the body would write false over the answer
}

}  // namespace

void Install() {
    if (g_watched) return;
    g_watched = true;
    if (!sg::WatchName(kRowCrematorAction, kTag, &OnCrematorActionPre, nullptr))
        UE_LOGE("puppet_cremation: the crematorAction watch did not register -- a cremator destroys a peer's puppet");
}

}  // namespace coop::puppet_cremation
