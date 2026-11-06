// coop/props/prop_tool_flags.cpp -- see coop/props/prop_tool_flags.h.

#include "coop/props/prop_tool_flags.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_hold_claim.h"    // IsArbitrated: a keyed Aprop_C, no swinger, no trash body
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_wire_parity.h"   // PhysFlagsOf
#include "coop/props/remote_prop.h"        // KeyToWString

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"  // ParamOffset
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace coop::prop_tool_flags {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace pf = coop::net::propspawn_flags;

// Every class's init: a tool's is the press, and the class test below keeps the others to one map lookup.
UE_FACT_FN(kRowInit, "prop_tool_flags", nullptr, L"init", AnyClass);
constexpr int kTag = 0x54464C47;  // 'TFLG'

constexpr uint8_t kFlagBits = pf::kStatic | pf::kFrozen | pf::kSleep;

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

// A class is a toolgun tool when its name starts tool_; asked once per class.
std::unordered_map<void*, bool> g_toolClass;

// The press in flight: the tool, its target and the target's flags before the body.
void*                 g_pressTool = nullptr;
ue_wrap::CachedObjRef g_pressTarget;
uint8_t               g_pressBefore = 0;

uint32_t g_sent = 0, g_applied = 0, g_same = 0, g_heldHere = 0, g_unresolved = 0;

bool IsToolClass(void* object) {
    void* const cls = R::ClassOf(object);
    if (!cls) return false;
    auto it = g_toolClass.find(cls);
    if (it == g_toolClass.end()) it = g_toolClass.emplace(cls, R::ClassNameOf(object).rfind(L"tool_", 0) == 0).first;
    return it->second;
}

sg::Verdict OnInitPre(const sg::Call& c) {
    if (!c.object || !IsToolClass(c.object)) return sg::Verdict::Run;
    g_pressTool = nullptr;
    g_pressTarget.Reset();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    static R::ParamOffset sToolgun{L"toolgun"};
    const int32_t off = sToolgun.Of(c.function);
    void* toolgun = nullptr;
    if (off >= 0 && c.locals) std::memcpy(&toolgun, c.locals + off, sizeof(toolgun));
    ue_wrap::hit_result::Hit hit;
    if (!toolgun || !R::IsLive(toolgun) || !ue_wrap::hit_result::ReadField(toolgun, L"outHit", &hit)) return sg::Verdict::Run;
    if (!hit.blocking || !coop::prop_hold_claim::IsArbitrated(hit.actor)) return sg::Verdict::Run;
    g_pressTool = c.object;
    g_pressTarget.Set(hit.actor);
    g_pressBefore = coop::prop_wire_parity::PhysFlagsOf(hit.actor) & kFlagBits;
    return sg::Verdict::Run;
}

void OnInitPost(const sg::Call& c) {
    if (!g_pressTool || c.object != g_pressTool) return;
    g_pressTool = nullptr;
    void* const target = g_pressTarget.Get();
    g_pressTarget.Reset();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!target || !s || !s->connected()) return;
    const uint8_t flags = coop::prop_wire_parity::PhysFlagsOf(target);
    if ((flags & kFlagBits) == g_pressBefore) return;   // a tool that moves no flag (the remover, a calibration)
    const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(target);
    coop::net::PropToolFlagsPayload p{};
    for (size_t i = 0; i < keyW.size() && i < sizeof(p.key.data); ++i)
        p.key.data[p.key.len++] = static_cast<char>(keyW[i]);   // save keys are ASCII
    p.physFlags = static_cast<uint8_t>((flags & kFlagBits) | pf::kLiveState);
    const bool sent = s->SendReliable(coop::net::ReliableKind::PropToolFlags, &p, sizeof(p));
    ++g_sent;
    UE_LOGI("prop_tool_flags: %ls set '%ls' static=%d frozen=%d sleep=%d -- PropToolFlags %s (#%u)",
            R::ClassNameOf(c.object).c_str(), keyW.c_str(), (flags & pf::kStatic) ? 1 : 0,
            (flags & pf::kFrozen) ? 1 : 0, (flags & pf::kSleep) ? 1 : 0, sent ? "sent" : "had no peer", g_sent);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_reg != Reg::Pending) return;
    g_reg = sg::WatchName(kRowInit, kTag, &OnInitPre, &OnInitPost) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    if (g_settled || g_reg == Reg::Pending) return;
    if (g_reg == Reg::Registered) sg::ResolvePendingNames();
    if (g_reg == Reg::Registered && sg::NameWatchLive(kRowInit, kTag)) {
        g_settled = true;
        UE_LOGI("prop_tool_flags: the watch on every class's init is live");
    } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowInit, kTag)) {
        g_settled = true;
        UE_LOGE("prop_tool_flags: the watch on every class's init is dead -- a toolgun's freeze, unfreeze or "
                "make-static stays on its user's copy");
    }
}

void OnToolFlags(int senderSlot, const coop::net::PropToolFlagsPayload& p) {
    const std::wstring keyW = coop::remote_prop::KeyToWString(p.key);
    void* const actor = coop::prop_key_index::Find(keyW);
    if (!actor || !coop::prop_hold_claim::IsArbitrated(actor)) {
        ++g_unresolved;
        UE_LOGI("prop_tool_flags: slot %d flags for '%ls' name no prop here -- dropped", senderSlot, keyW.c_str());
        return;
    }
    // This player's hold owns its copy, and the release hands its flags to every peer.
    void* const local = coop::players::Registry::Get().Local();
    if (local && ue_wrap::engine::IsMainPlayerGrabbing(local, actor)) {
        ++g_heldHere;
        UE_LOGI("prop_tool_flags: slot %d flags for '%ls' -- this player holds it; left to the hold", senderSlot,
                keyW.c_str());
        return;
    }
    const bool wantStatic = (p.physFlags & pf::kStatic) != 0;
    const bool wantFrozen = (p.physFlags & pf::kFrozen) != 0;
    const bool wantSleep  = (p.physFlags & pf::kSleep) != 0;
    if (ue_wrap::prop::IsStatic(actor) == wantStatic && ue_wrap::prop::IsFrozen(actor) == wantFrozen &&
        ue_wrap::prop::IsSleeping(actor) == wantSleep) {
        ++g_same;
        return;
    }
    if (!ue_wrap::prop::CallBaseSetPropProps(actor, wantStatic, wantFrozen, wantSleep)) {
        UE_LOGW("prop_tool_flags: slot %d flags for '%ls' -- the game's setter did not dispatch; the copy keeps its "
                "flags", senderSlot, keyW.c_str());
        return;
    }
    ++g_applied;
    UE_LOGI("prop_tool_flags: slot %d set '%ls' static=%d frozen=%d sleep=%d (#%u)", senderSlot, keyW.c_str(),
            wantStatic ? 1 : 0, wantFrozen ? 1 : 0, wantSleep ? 1 : 0, g_applied);
}

void OnDisconnect() {
    if (g_sent || g_applied || g_same || g_heldHere || g_unresolved)
        UE_LOGI("prop_tool_flags: session end -- sent=%u applied=%u same=%u held_here=%u unresolved=%u", g_sent,
                g_applied, g_same, g_heldHere, g_unresolved);
    g_pressTool = nullptr;
    g_pressTarget.Reset();
    g_sent = g_applied = g_same = g_heldHere = g_unresolved = 0;
}

}  // namespace coop::prop_tool_flags
