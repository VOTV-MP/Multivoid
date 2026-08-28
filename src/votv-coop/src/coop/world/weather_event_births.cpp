// coop/weather_event_births.cpp -- see coop/weather_event_births.h.

#include "coop/world/weather_event_births.h"

#include "coop/net/session.h"
#include "coop/world/weather_fog.h"
#include "coop/world/weather_redsky.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cstdint>

namespace coop::weather_event_births {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_isClient{false};

bool g_hookInstalled = false;   // FinishSpawningActor POST hook (process-lifetime)
bool g_namesMinted   = false;   // the three class FNames resolved (GT-only mint)

// The suppressed birth classes, matched by FName index (int compares on the
// hot path -- FinishSpawningActor fires for EVERY actor spawn, so no string
// build here). Index 0..2: redSkyEvent_C / weatherFogController_C / blackFog_C.
constexpr int kNumClasses = 3;
R::FName g_classNames[kNumClasses] = {};
const wchar_t* const kClassNameStrs[kNumClasses] = {
    P::name::RedSkyEventClass,          // L"redSkyEvent_C"
    P::name::WeatherFogControllerClass, // L"weatherFogController_C"
    P::name::BlackFogClass,             // L"blackFog_C"
};

// Rate-latched suppression log (a stuck roll cannot spam the log).
uint32_t g_suppressed[kNumClasses] = {};

// FinishSpawningActor POST: destroy an UNCOMMANDED weather-event birth on a
// CLIENT. Chains after host_spawn_watcher / prop_drop_intent on the same
// UFunction (role-disjoint with the former, class-disjoint with both).
void OnFinishSpawnPost(void* /*context*/, void* /*srcObj*/, void* result) {
    if (!g_isClient.load(std::memory_order_acquire)) return;
    if (!GT::IsGameThread()) return;
    void* actor = result;
    if (!actor) return;
    void* cls = R::ClassOf(actor);
    if (!cls) return;
    const R::FName& n = R::NameOf(cls);
    int match = -1;
    for (int i = 0; i < kNumClasses; ++i) {
        if (n.ComparisonIndex == g_classNames[i].ComparisonIndex &&
            n.Number == g_classNames[i].Number) { match = i; break; }
    }
    if (match < 0) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    // Wire-commanded mirror births pass: each lane raises its echo flag around
    // its own reflected spawn Call. (blackFog_C has no apply lane yet -- every
    // client birth of it is organic by construction.)
    if (coop::weather_redsky::ApplyEchoActive()) return;
    if (coop::weather_fog::MirrorEchoActive()) return;
    if (!R::IsLive(actor)) return;
    E::DestroyActor(actor);
    const uint32_t nSup = ++g_suppressed[match];
    if (nSup <= 5 || (nSup % 25) == 0) {
        UE_LOGW("weather_births: CLIENT suppressed uncommanded %ls birth #%u "
                "(the newDay roll is host-owned RNG; EX_Local caller is "
                "PE-invisible -- destroyed at FinishSpawn)",
                kClassNameStrs[match], nSup);
    }
}

}  // namespace

bool Install(coop::net::Session* session, bool isHost) {
    g_session.store(session, std::memory_order_release);
    g_isClient.store(!isHost, std::memory_order_release);
    if (g_hookInstalled && g_namesMinted) return true;
    if (!GT::IsGameThread()) return false;  // FName mint dispatches ProcessEvent
    if (!g_namesMinted) {
        bool all = true;
        for (int i = 0; i < kNumClasses; ++i) {
            g_classNames[i] = ue_wrap::fname_utils::StringToFName(kClassNameStrs[i]);
            if (g_classNames[i].ComparisonIndex == 0) all = false;  // "None" = mint failed
        }
        if (!all) return false;
        g_namesMinted = true;
    }
    if (!g_hookInstalled) {
        void* statics = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
        void* cls = statics ? R::ClassOf(statics) : nullptr;
        void* fn = cls ? R::FindFunction(cls, P::name::FinishSpawningActorFn) : nullptr;
        if (!fn) return false;
        if (!ue_wrap::ufunction_hook::InstallPostHook(fn, &OnFinishSpawnPost)) {
            UE_LOGW("weather_births: FinishSpawningActor POST hook install FAILED -- retrying");
            return false;
        }
        g_hookInstalled = true;
        UE_LOGI("weather_births: FinishSpawningActor POST hook installed "
                "(client birth-catch for redSkyEvent_C/weatherFogController_C/blackFog_C)");
    }
    return true;
}

void OnDisconnect() {
    // Hook + minted names persist (process-lifetime; self-gated on session).
    for (int i = 0; i < kNumClasses; ++i) g_suppressed[i] = 0;
}

}  // namespace coop::weather_event_births
