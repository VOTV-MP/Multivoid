// coop/dev/food_sleep_drill.cpp -- see coop/dev/food_sleep_drill.h.

#include "coop/dev/food_sleep_drill.h"

#include "coop/config/config.h"
#include "coop/gamefix/food_sleep_fix.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::dev::food_sleep_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace V = ue_wrap::vitals;

enum class Arm { Off, On, NoFix };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::food_sleep_drill);
        return v == "on" ? Arm::On : v == "nofix" ? Arm::NoFix : Arm::Off;
    }();
    return a;
}

bool Host() { return coop::roster::LocalIsHost(); }
char Who() { return Host() ? 'H' : 'C'; }

constexpr float kSleepStart = 40.f;
constexpr float kSleepArg   = 20.f;
constexpr float kTolerance  = 0.5f;  // the player's own tick drains sleep a little between a write and a read

uint64_t g_readyMs = 0;
bool     g_done = false;

bool JoinOver(coop::net::Session& s) {
    if (!Host())
        return coop::net_pump::HasAnnouncedWorldReady() &&
               coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

// One addFood on the machine's own player from sleep 40: the sleep it leaves, and the call's `full`.
bool Eat(void* player, float food, float coffee, bool bypassNoHunger, float& sleepAfter, bool& full) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), L"addFood");
    if (!fn) return false;
    if (!V::Write(V::Field::Sleep, kSleepStart) || !V::Write(V::Field::CoffeePower, coffee) ||
        !V::Write(V::Field::Food, food))
        return false;
    ue_wrap::ParamFrame f(fn);
    const uint8_t failType = 0;
    if (!f.valid() || !f.Set<float>(L"food", 1.f) || !f.Set<float>(L"sleep", kSleepArg) ||
        !f.Set<float>(L"health", 0.f) || !f.Set<void*>(L"asFood", nullptr) ||
        !f.Set<bool>(L"bypassNoHunger", bypassNoHunger) || !f.SetRaw(L"failType", &failType, 1) ||
        !ue_wrap::Call(player, f))
        return false;
    full = f.Get<bool>(L"full");
    return V::Read(V::Field::Sleep, &sleepAfter);
}

void Run() {
    g_done = true;
    void* player = coop::players::Registry::Get().Local();
    float sleep0 = 0.f, coffee0 = 0.f, food0 = 0.f;
    if (!player || !V::Read(V::Field::Sleep, &sleep0) || !V::Read(V::Field::CoffeePower, &coffee0) ||
        !V::Read(V::Field::Food, &food0)) {
        UE_LOGW("[FOOD-SLEEP-DRILL] [%c] FAIL fixture: no local player or its stats do not read", Who());
        return;
    }
    if (ArmOf() == Arm::NoFix) coop::gamefix::food_sleep_fix::SetEnabled(false);
    const uint64_t applied0 = coop::gamefix::food_sleep_fix::Applied();
    float plain = -1.f, tolerant = -1.f, refused = -1.f;
    bool fullPlain = true, fullTolerant = true, fullRefused = false;
    const bool ran = Eat(player, 50.f, 0.f, true, plain, fullPlain) &&
                     Eat(player, 50.f, 0.5f, true, tolerant, fullTolerant) &&
                     Eat(player, 100.f, 0.f, false, refused, fullRefused);
    V::Write(V::Field::Sleep, sleep0);
    V::Write(V::Field::CoffeePower, coffee0);
    V::Write(V::Field::Food, food0);
    const uint64_t applied = coop::gamefix::food_sleep_fix::Applied() - applied0;
    if (!ran) {
        UE_LOGW("[FOOD-SLEEP-DRILL] [%c] FAIL fixture: an addFood call or a stat write did not run", Who());
        return;
    }
    const bool pass = std::fabs(plain - (kSleepStart + kSleepArg)) <= kTolerance && !fullPlain &&
                      std::fabs(tolerant - (kSleepStart + kSleepArg * 0.5f)) <= kTolerance && !fullTolerant &&
                      std::fabs(refused - kSleepStart) <= kTolerance && fullRefused;
    if (pass)
        UE_LOGI("[FOOD-SLEEP-DRILL] [%c] PASS sleep 40 after an eat of sleep 20 reads %.2f (60 wanted), %.2f with coffee "
                "tolerance 0.5 (50 wanted), %.2f after a refused eat (40 wanted); the fix applied %llu time(s)",
                Who(), plain, tolerant, refused, static_cast<unsigned long long>(applied));
    else
        UE_LOGW("[FOOD-SLEEP-DRILL] [%c] FAIL sleep 40 after an eat of sleep 20 reads %.2f (60 wanted, full=%d), %.2f with "
                "coffee tolerance 0.5 (50 wanted, full=%d), %.2f after a refused eat (40 wanted, full=%d); the fix "
                "applied %llu time(s)",
                Who(), plain, fullPlain ? 1 : 0, tolerant, fullTolerant ? 1 : 0, refused, fullRefused ? 1 : 0,
                static_cast<unsigned long long>(applied));
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || g_done || !session->connected() || !JoinOver(*session)) return;
    if (!g_readyMs) g_readyMs = coop::net::NowMs();
    if (coop::net::NowMs() - g_readyMs < 4000) return;
    Run();
}

void OnDisconnect() {
    g_readyMs = 0;
    g_done = false;
}

}  // namespace coop::dev::food_sleep_drill
