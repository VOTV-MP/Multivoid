// coop/dev/impact_damage_probe.cpp -- see coop/dev/impact_damage_probe.h.

#include "coop/dev/impact_damage_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_syncer.h"
#include "coop/props/prop_throw.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::impact_damage_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;

UE_FACT_FN(kRowImpact, "impact_damage_probe", L"comp_physicsImpact_C", L"impactDamage", Declared);

constexpr int      kTag = 0x49445052;  // 'IDPR'
constexpr uint32_t kMaxLines = 400;
constexpr uint64_t kRetryMs = 1000;

bool        g_registered = false;
bool        g_settled = false;
uint64_t    g_nextTryMs = 0;
uint32_t    g_lines = 0;
uint32_t    g_calls = 0;
bool        g_capSaid = false;

R::ParamOffset g_damageOff{L"damage"};
R::ParamOffset g_actorOff{L"actor"};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::impact_damage_probe);
    return s;
}

// A parameter's storage in the call: the caller's, for an out or reference parameter, else the frame's own slot.
const uint8_t* ParamBytes(const SG::Call& c, int32_t offset) {
    if (offset < 0 || !c.locals) return nullptr;
    const uint8_t* out = SG::OutParamPtr(c, offset);
    return out ? out : c.locals + offset;
}

// This machine authors the actor's motion: it holds it in the hand, it threw it, or its prop-syncer episode is open.
bool AuthoredHere(void* x) {
    if (!x || !R::IsLive(x)) return false;
    void* me = coop::players::Registry::Get().Local();
    if (me && ue_wrap::engine::IsMainPlayerGrabbing(me, x)) return true;
    if (coop::prop_throw::SeqFor(x) != 0) return true;
    const coop::element::ElementId eid = coop::remote_prop::ResolveMirrorEidByActor(x, true);
    if (eid == coop::element::kInvalidId) return false;
    return coop::prop_syncer::dev::EntryOf(static_cast<uint32_t>(eid)).state != coop::prop_syncer::dev::EntryState::None;
}

bool IsLocalPlayer(void* actor) {
    auto& reg = coop::players::Registry::Get();
    if (reg.IsLocal(actor)) return true;
    void* outer = R::OuterOf(actor);
    return outer && reg.IsLocal(outer);
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::string ClassOrNull(void* o) { return o && R::IsLive(o) ? Narrow(R::ClassNameOf(o)) : std::string("null"); }

std::string Caller(const SG::Call& c) {
    if (!c.callerObject || !c.callerFunction) return "native";
    return ClassOrNull(c.callerObject) + "." + Narrow(R::ToString(R::NameOf(c.callerFunction)));
}

SG::Verdict OnImpactPre(const SG::Call& c) {
    ++g_calls;
    if (g_lines >= kMaxLines) {
        if (!g_capSaid) {
            g_capSaid = true;
            UE_LOGI("impact_damage_probe: further calls counted only");
        }
        return SG::Verdict::Run;
    }
    ++g_lines;
    float damage = 0.f;
    void* actor = nullptr;
    if (const uint8_t* d = ParamBytes(c, g_damageOff.Of(c.function))) std::memcpy(&damage, d, sizeof(damage));
    if (const uint8_t* a = ParamBytes(c, g_actorOff.Of(c.function))) std::memcpy(&actor, a, sizeof(actor));
    if (actor && !R::IsLive(actor)) actor = nullptr;
    void* owner = R::OuterOf(c.object);
    const std::wstring key = owner && R::IsLive(owner) ? ue_wrap::prop::GetKeyString(owner) : std::wstring();
    UE_LOGI("impact_damage_probe: %s owner=%s key=%ls dmg=%.1f actor=%s actorIsLocal=%d actorIsPuppet=%d "
            "actorAuthoredHere=%d ownerAuthoredHere=%d fromOurCode=%d caller=%s",
            coop::roster::LocalIsHost() ? "host" : "client", ClassOrNull(owner).c_str(), key.c_str(),
            static_cast<double>(damage), ClassOrNull(actor).c_str(), actor && IsLocalPlayer(actor) ? 1 : 0,
            actor && coop::players::Registry::Get().IsPuppet(actor) ? 1 : 0, AuthoredHere(actor) ? 1 : 0,
            AuthoredHere(owner) ? 1 : 0, c.fromOurCode ? 1 : 0, Caller(c).c_str());
    return SG::Verdict::Run;
}

}  // namespace

// The shape of prop_throw's EnsureWatch: a refused registration is asked again each second while the gate is not
// armed; a watch whose name resolved dead is said once.
void Install() {
    if (!Enabled() || g_settled) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kRetryMs;
    if (!g_registered) g_registered = SG::WatchName(kRowImpact, kTag, &OnImpactPre, nullptr);
    if (!g_registered) {
        if (SG::IsInstalled()) {
            g_settled = true;
            UE_LOGE("impact_damage_probe: the gate refused the impactDamage watch -- no call is logged");
        }
        return;
    }
    SG::ResolvePendingNames();
    if (SG::NameWatchLive(kRowImpact, kTag)) {
        g_settled = true;
        UE_LOGI("impact_damage_probe: the comp_physicsImpact_C impactDamage watch is live");
    } else if (SG::NameWatchSettled(kRowImpact, kTag)) {
        g_settled = true;
        UE_LOGE("impact_damage_probe: the impactDamage watch is dead -- no call is logged");
    }
}

void OnDisconnect() {
    if (!Enabled()) return;
    if (g_calls != 0) UE_LOGI("impact_damage_probe: %u calls this session, %u printed", g_calls, g_lines);
    g_lines = 0;
    g_calls = 0;
    g_capSaid = false;
}

}  // namespace coop::dev::impact_damage_probe
