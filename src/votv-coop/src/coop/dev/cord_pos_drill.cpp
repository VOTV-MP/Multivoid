// coop/dev/cord_pos_drill.cpp -- see coop/dev/cord_pos_drill.h. The drill's switch, its entry points and the pieces
// both roles share; the roles are cord_pos_drill_host.cpp and cord_pos_drill_client.cpp.

#include "coop/dev/cord_pos_drill.h"

#include "cord_pos_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <atomic>
#include <cmath>
#include <string>

namespace coop::dev::cord_pos_drill {
namespace {

namespace D = detail;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::cord_pos_drill);
    return s;
}

}  // namespace

namespace detail {

bool RestClock::Update(void* actor, uint64_t nowMs, uint64_t needMs) {
    namespace E = ue_wrap::engine;
    ue_wrap::FVector lin{}, ang{};
    if (!E::GetActorRootPhysicsVelocity(actor, lin, ang)) lin = E::GetActorVelocity(actor);
    const float speed = std::sqrt(lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z);
    if (speed >= kRestVelCmS) {
        quietSince = 0;
        return false;
    }
    if (!quietSince) quietSince = nowMs;
    return nowMs - quietSince >= needMs;
}

uint64_t NowMs() { return coop::net::NowMs(); }

std::string KeyOf(void* maker) {
    const std::wstring w = maker ? ue_wrap::prop::GetKeyString(maker) : std::wstring();
    if (w.empty() || w == L"None") return "?";
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

std::string RestPrefix(const std::string& key) { return "cordpos rest " + key + " "; }

}  // namespace detail

void Install(coop::net::Session* s) {
    if (!Enabled()) return;
    g_session.store(s, std::memory_order_release);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (s->role() == coop::net::Role::Host) D::HostTick(s);
    else if (s->connected()) D::ClientTick(s);
}

void OnDisconnect() {
    if (!Enabled()) return;
    D::HostReset();
    D::ClientReset();
}

void OnMirrorBound(uint32_t eid, void* actor) {
    if (!Enabled()) return;
    D::ClientOnBound(eid, actor);
}

void OnMirrorHidden(uint32_t eid, void* actor) {
    if (!Enabled()) return;
    D::ClientOnHidden(eid, actor);
}

}  // namespace coop::dev::cord_pos_drill
