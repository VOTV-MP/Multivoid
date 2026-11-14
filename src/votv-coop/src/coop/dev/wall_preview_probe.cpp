// coop/dev/wall_preview_probe.cpp -- [dev] a wall builder's aiming preview on both peers (VOTVCOOP_WALL_PREVIEW_PROBE=1).
//
// CLIENT, after it is connected: at 15 s takes a wall builder into its hand through the drill hand (the hold slot and
// the game's updateHold), trying the item names it may have; at 20 s presses the tool's LMB through its own
// playerHandUse_LMB, as the left button does, and at 23 s turns 30 degrees, so the cube stretches; at 26 s cancels it
// through playerHandUse_RMB, as the right button does. BOTH, every half second from 15 s to 50 s: this peer's own tool
// and every hand mirror that is one -- editing, the cube's visibility, world location and scale, and pivot_a
// (`[WALLPV]`).

#include "coop/dev/wall_preview_probe.h"

#include "coop/config/config.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cstdint>

namespace coop::dev::wall_preview_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_WALL_PREVIEW_PROBE") == "1";
    return on;
}

bool IsTool(void* obj) {
    void* const base = OI::ClassByName(L"prop_wallbuilder_C");
    return obj && base && R::IsLive(obj) && R::IsDescendantOfAny(R::ClassOf(obj), &base, 1);
}

void* Comp(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

int Bool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) ? 1 : 0;
}

int Visible(void* comp) {
    ue_wrap::ParamFrame f(comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"IsVisible") : nullptr);
    return f.valid() && ue_wrap::Call(comp, f) ? (f.Get<bool>(L"ReturnValue") ? 1 : 0) : -1;
}

ue_wrap::FVector Scale(void* comp) {
    ue_wrap::ParamFrame f(comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"K2_GetComponentScale") : nullptr);
    if (!f.valid() || !ue_wrap::Call(comp, f)) return {};
    return f.Get<ue_wrap::FVector>(L"ReturnValue");
}

void Say(const char* who, void* tool) {
    void* const cube = Comp(tool, L"cube");
    void* const a = Comp(tool, L"pivot_a");
    const ue_wrap::FVector c = cube ? E::GetComponentLocation(cube) : ue_wrap::FVector{};
    const ue_wrap::FVector s = Scale(cube);
    const ue_wrap::FVector p = a ? E::GetComponentLocation(a) : ue_wrap::FVector{};
    ue_wrap::FVector ta{};
    E::TryGetActorLocation(tool, ta);
    UE_LOGI("[WALLPV] %hs editing=%d isHeld=%d cube visible=%d at (%.0f,%.0f,%.0f) scale (%.2f,%.2f,%.2f) pivot_a (%.0f,%.0f,%.0f) "
            "[tool %ls at (%.0f,%.0f,%.0f), cube %ls, pivot_a %ls, pivot_b %ls]", who,
            Bool(tool, L"editing"), Bool(tool, L"isHeld"), Visible(cube), c.X, c.Y, c.Z, s.X, s.Y, s.Z, p.X, p.Y, p.Z,
            R::ClassNameOf(tool).c_str(), ta.X, ta.Y, ta.Z, cube ? R::ClassNameOf(cube).c_str() : L"-",
            a ? R::ClassNameOf(a).c_str() : L"-", Comp(tool, L"pivot_b") ? R::ClassNameOf(Comp(tool, L"pivot_b")).c_str() : L"-");
}

bool CallLmb(void* tool, const wchar_t* verb) {
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::ParamFrame f(tool ? R::FindDispatchFunctionCached(R::ClassOf(tool), verb) : nullptr);
    return me && f.valid() && f.Set<void*>(L"player", me) && ue_wrap::Call(tool, f);
}

// The game's Custom Content rule, in this process's rules and never saved: the wall builder's LMB refuses without it.
void AllowCustomContent() {
    void* const instance = R::FindObjectByClass(L"mainGameInstance_C");
    void* const cls = instance ? R::ClassOf(instance) : nullptr;
    const int32_t rules = cls ? R::FindPropertyOffset(cls, L"gameRules") : -1;
    void* const type = cls ? R::PropertyInnerStruct(cls, L"gameRules") : nullptr;
    const int32_t rule = type ? R::FindPropertyOffsetByPrefix(type, L"customContent_") : -1;
    if (rules >= 0 && rule >= 0) static_cast<uint8_t*>(instance)[rules + rule] = 1;
    UE_LOGI("[WALLPV] the Custom Content rule set on in this process (ok=%d)", rules >= 0 && rule >= 0 ? 1 : 0);
}

// The player's view turned to `pitch` and `yaw` more, so the tool's trace lands on the floor before it.
void Look(float pitch, float addYaw) {
    void* const me = coop::players::Registry::Get().Local();
    void* const pc = me ? E::GetController(me) : nullptr;
    if (!pc) return;
    const ue_wrap::FRotator r = E::GetControlRotation(pc);
    E::SetControlRotation(pc, {pitch, r.Yaw + addYaw, 0.f});
}

uint64_t g_startAt = 0, g_reportAt = 0;
int g_step = 0;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[WALLPV] probe armed role=%s", host ? "host" : "client");
        AllowCustomContent();
    }
    const uint64_t t = Now() - g_startAt;
    void* const hand = coop::hand_item::LocalHandActor();
    if (!host && g_step == 0 && t >= 15000u) {
        g_step = 1;
        const wchar_t* const names[] = {L"wallbuilder", L"wallBuilder", L"wall_builder", L"builder"};
        for (const wchar_t* n : names) {
            const bool ok = coop::dev::drill_hand::TakeIntoHandNow(n, L"prop_wallbuilder_C");
            UE_LOGI("[WALLPV] client: took '%ls' into the hand (ok=%d, holds a wall builder=%d)", n, ok ? 1 : 0,
                    IsTool(coop::hand_item::LocalHandActor()) ? 1 : 0);
            if (IsTool(coop::hand_item::LocalHandActor())) break;
        }
    }
    if (!host && g_step == 1 && t >= 18000u && t < 20000u) Look(-75.f, 0.f);  // down at the floor, held as a player looks
    if (!host && g_step == 1 && t >= 20000u) {
        g_step = 2;
        UE_LOGI("[WALLPV] client: pressed the tool's LMB (ran=%d)", CallLmb(hand, L"playerHandUse_LMB") ? 1 : 0);
    }
    if (!host && g_step == 2 && t >= 23000u) {
        g_step = 3;
        Look(-75.f, 30.f);
        UE_LOGI("[WALLPV] client: turned 30 degrees");
    }
    if (!host && g_step == 3 && t >= 26000u) {
        g_step = 4;
        UE_LOGI("[WALLPV] client: cancelled through the tool's RMB (ran=%d)", CallLmb(hand, L"playerHandUse_RMB") ? 1 : 0);
    }
    if (t < 15000u || t > 50000u || Now() < g_reportAt) return;
    g_reportAt = Now() + 500;
    if (IsTool(hand)) Say("own", hand);
    for (uint8_t s = 0; s < coop::net::kMaxPeers; ++s) {
        void* const m = coop::hand_item::MirrorActorForSlot(s);
        if (!IsTool(m)) continue;
        char who[24];
        snprintf(who, sizeof(who), "mirror slot=%u", s);
        Say(who, m);
    }
}

}  // namespace coop::dev::wall_preview_probe
