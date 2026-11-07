// coop/props/hacksaw_cut.cpp -- see coop/props/hacksaw_cut.h.

#include "coop/props/hacksaw_cut.h"

#include "coop/net/session.h"
#include "coop/props/container_break.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::props::hacksaw_cut {
namespace {

namespace R  = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace GT = ue_wrap::game_thread;
namespace ci = ue_wrap::container_inventory;

UE_FACT_FN(kRowBeginDeferred, "hacksaw_cut", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);

constexpr int kMaxTries = 600;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_hooked = false;
int g_tries = 0;
void* g_sawCls = nullptr;     // prop_hacksaw_C, resolved on first sight of a birth from its graph
int32_t g_sawingOff = -2;     // prop_hacksaw_C.sawingProp; -2 unresolved, -1 absent
void* g_cut = nullptr;        // the container a cut handed to the host, until its destroy
int32_t g_cutIndex = -1;
uint32_t g_asked = 0, g_dissolved = 0, g_held = 0;

bool IsSaw(void* obj) {
    if (!obj) return false;
    void* cls = R::ClassOf(obj);
    if (g_sawCls) return cls == g_sawCls;
    if (!cls || R::ClassNameOf(obj) != L"prop_hacksaw_C") return false;
    g_sawCls = cls;
    g_sawingOff = R::FindPropertyOffset(cls, L"sawingProp");
    return true;
}

void* SawingProp(void* saw) {
    if (g_sawingOff < 0) return nullptr;
    void* p = *reinterpret_cast<void**>(static_cast<uint8_t*>(saw) + g_sawingOff);
    return p && R::IsLive(p) ? p : nullptr;
}

// A birth the saw's graph makes opens the cut of a container it saws; any other birth while the cut is open is that
// container's spill on this copy.
void OnBegin(void* /*context*/, void* sourceObject, void* result) {
    if (!result || !coop::spawn_authority::IsActiveClientSession()) return;
    if (IsSaw(sourceObject)) {
        void* prop = SawingProp(sourceObject);
        if (prop && prop != g_cut && ci::IsContainer(prop) &&
            coop::props::container_break::RequestFromClient(prop, /*fire*/ false, /*sawn*/ true)) {
            g_cut = prop;
            g_cutIndex = R::InternalIndexOf(prop);
            ++g_asked;
            UE_LOGI("hacksaw_cut: this player's saw cuts container '%ls' -- the host breaks its own copy (#%u)",
                    R::ClassNameOf(prop).c_str(), g_asked);
        }
        return;
    }
    if (!g_cut || !R::IsLiveByIndex(g_cut, g_cutIndex)) return;
    const int32_t index = R::InternalIndexOf(result);
    if (void** r = static_cast<void**>(UH::CurrentResult())) *r = nullptr;
    GT::Post([result, index] {
        if (R::IsLiveByIndex(result, index)) ue_wrap::engine::DestroyActor(result);
    });
    ++g_dissolved;
    UE_LOGI("hacksaw_cut: the sawn container's local spill of '%ls' dissolved -- the host's spill comes instead (#%u)",
            R::ClassNameOf(result).c_str(), g_dissolved);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_hooked || g_tries >= kMaxTries) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBeginDeferred);
    if (fn && UH::InstallPostHook(fn, &OnBegin)) {
        g_hooked = true;
        UE_LOGI("hacksaw_cut: birth seam installed on BeginDeferredActorSpawnFromClass");
    } else if (++g_tries >= kMaxTries) {
        UE_LOGW("hacksaw_cut: BeginDeferredActorSpawnFromClass would not take a post hook -- a container a client "
                "saws spills on that client alone");
    }
}

bool TakesDestroy(void* actor) {
    if (!actor || actor != g_cut) return false;
    g_cut = nullptr;
    g_cutIndex = -1;
    ++g_held;
    UE_LOGI("hacksaw_cut: the sawn container's destroy is not sent -- the host's break destroys it (#%u)", g_held);
    return true;
}

void Tick() {
    g_cut = nullptr;
    g_cutIndex = -1;
}

void OnDisconnect() {
    if (g_asked || g_dissolved || g_held)
        UE_LOGI("hacksaw_cut: session end -- asked=%u, spill births dissolved=%u, destroys held=%u", g_asked,
                g_dissolved, g_held);
    g_asked = g_dissolved = g_held = 0;
    g_cut = nullptr;
    g_cutIndex = -1;
    g_session.store(nullptr);
}

}  // namespace coop::props::hacksaw_cut
