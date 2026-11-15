// coop/world/ufo_shield_sync.cpp -- see coop/world/ufo_shield_sync.h.

#include "coop/world/ufo_shield_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::ufo_shield_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;

UE_FACT_FN(kRowSetActive, "ufo_shield_sync", L"ufoshieldshader_C", L"SetActive", Declared);
UE_FACT_FN(kRowFlyAway, "ufo_shield_sync", L"ufoshieldshader_C", L"flyAway", Declared);

constexpr int kTagSetActive = 0x55465300;  // 'UFS'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;

// HOST: the last SetActive this world's saucer took, and the world it was in.
bool     g_hostActive = false;
uint32_t g_hostGen = 0;

// CLIENT: the host bit last applied (-1: none yet), and the world it was applied in.
int      g_applied = -1;
uint32_t g_appliedGen = 0;

void OnSetActivePost(const SG::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || !call.locals) return;
    static int32_t s_off = -2;
    if (s_off == -2) s_off = R::FindParamOffset(call.function, L"bEnabled");
    if (s_off < 0) return;
    const bool active = call.locals[s_off] != 0;
    g_hostGen = ue_wrap::world_identity::Generation();
    if (active != g_hostActive) UE_LOGI("ufo_shield: the host's saucer is %s", active ? "ACTIVE" : "off");
    g_hostActive = active;
}

// This world's saucer: the level holds one. Null while the class or the level is not up.
void* FindSaucer() {
    void* cls = ue_wrap::object_index::ClassByName(L"ufoshieldshader_C");
    if (!cls) return nullptr;
    struct Seek {
        void* world;
        void* found;
    } seek{ue_wrap::world_identity::CurrentWorld(), nullptr};
    if (!seek.world) return nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        Seek* k = static_cast<Seek*>(ctx);
        if (!k->found && obj && R::IsLive(obj) && ue_wrap::world_identity::WorldOf(obj) == k->world) k->found = obj;
    }, &seek);
    return seek.found;
}

// The verb called on the client's saucer. True when the call ran.
bool CallOnSaucer(const ue_wrap::facts::Function& row, bool hasArg, bool arg) {
    void* saucer = FindSaucer();
    if (!saucer) return false;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::ResolveOn(row, R::ClassOf(saucer));
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn.Fn());
    if (!f.valid()) return false;
    if (hasArg && !f.Set(L"bEnabled", arg)) return false;
    return ue_wrap::Call(saucer, f);
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    if (SG::WatchName(kRowSetActive, kTagSetActive, nullptr, &OnSetActivePost))
        UE_LOGI("ufo_shield: the saucer's SetActive is watched (the host stamps it into the weather state)");
    else
        UE_LOGE("ufo_shield: the watch on ufoshieldshader_C::SetActive was refused -- a client's saucer never shows");
}

bool HostActive() {
    return g_hostActive && g_hostGen == ue_wrap::world_identity::Generation();
}

void ApplyFromHost(const coop::net::WeatherStatePayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client || !ue_wrap::game_thread::IsGameThread()) return;
    const int host = (payload.flags2 & coop::net::fog_flags2::kUfoShieldActive) ? 1 : 0;
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_appliedGen) {
        g_appliedGen = gen;
        g_applied = -1;
    }
    if (host == g_applied) return;
    // A first state that says off finds the saucer as the level left it: hidden.
    if (host == 0 && g_applied == -1) {
        g_applied = 0;
        return;
    }
    const bool ran = host ? CallOnSaucer(kRowSetActive, true, true) : CallOnSaucer(kRowFlyAway, false, false);
    if (!ran) return;  // no saucer yet (the level is loading): the next state asks again
    g_applied = host;
    UE_LOGI("ufo_shield: host saucer %s -> the client's %s", host ? "ACTIVE" : "off",
            host ? "is set active" : "flies away");
}

void OnDisconnect() {
    g_hostActive = false;
    g_hostGen = 0;
    g_applied = -1;
    g_appliedGen = 0;
}

}  // namespace coop::ufo_shield_sync
