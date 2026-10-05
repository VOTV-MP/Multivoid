// coop/dev/damage_probe.cpp -- see coop/dev/damage_probe.h.
#include "coop/dev/damage_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::damage_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The verb wisp_attack_sync watches too; a second watch on the same name with its own tag.
constexpr const wchar_t* kDamageVerbName = L"Add Player Damage";
constexpr int kTag = 0x444D4750;  // 'DMGP'

std::wstring ClassOrNull(void* obj) { return obj ? R::ClassNameOf(obj) : std::wstring(L"null"); }

sg::Verdict OnDamagePre(const sg::Call& c) {
    if (!c.locals || !c.function) return sg::Verdict::Run;
    // The parameter offsets, per function: a name watch may match an overriding subclass's verb.
    static void* sFn = nullptr;
    static int32_t sAmountOff = -1;
    static int32_t sSourceOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sAmountOff = -1;
        for (const wchar_t* n : {L"damage", L"amount", L"dmg", L"value"}) {
            sAmountOff = R::FindParamOffset(c.function, n);
            if (sAmountOff >= 0) break;
        }
        sSourceOff = R::FindParamOffset(c.function, L"source");
    }
    float amount = 0.f;
    if (sAmountOff >= 0) std::memcpy(&amount, c.locals + sAmountOff, sizeof(amount));
    void* source = nullptr;
    if (sSourceOff >= 0) std::memcpy(&source, c.locals + sSourceOff, sizeof(source));

    auto& reg = coop::players::Registry::Get();
    std::string body;
    if (c.object && c.object == reg.Local()) {
        body = "LOCAL";
    } else if (c.object && reg.IsPuppet(c.object)) {
        body = "PUPPET slot=" + std::to_string(reg.PeerIdOfActor(c.object));
    } else {
        body = "OTHER";
    }
    UE_LOGI("[DMG] %s body=%ls amount=%s%.2f source=%ls caller=%ls.%ls depth=%d%s", body.c_str(),
            ClassOrNull(c.object).c_str(), sAmountOff >= 0 ? "" : "?", amount, ClassOrNull(source).c_str(),
            ClassOrNull(c.callerObject).c_str(),
            c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)).c_str() : L"<ProcessEvent>", c.depth,
            c.fromOurCode ? " [ours]" : "");
    return sg::Verdict::Run;
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::damage_probe);
    static bool s_watched = false;
    if (!s_on || s_watched) return;
    s_watched = true;
    if (sg::WatchName(kDamageVerbName, kTag, &OnDamagePre, nullptr))
        UE_LOGI("[DMG] probe armed on '%ls'", kDamageVerbName);
    else
        UE_LOGW("[DMG] probe could not watch '%ls'", kDamageVerbName);
}

}  // namespace coop::dev::damage_probe
