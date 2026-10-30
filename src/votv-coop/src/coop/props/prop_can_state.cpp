// coop/props/prop_can_state.cpp -- see coop/props/prop_can_state.h.

#include "coop/props/prop_can_state.h"

#include "coop/net/session.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::props::prop_can_state {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowInit, "prop_can_state", nullptr, L"init", AnyClass);
constexpr int kTag = 0x43414E53;  // 'CANS'

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

// What makes a class a can, asked once per class; a variant (prop_cannedFood_0_C) is a child of the base.
std::unordered_map<void*, bool> g_isCanClass;

struct CanState {
    bool    opened = false;
    int32_t foods = 0;
    bool operator==(const CanState& o) const { return opened == o.opened && foods == o.foods; }
};
std::unordered_map<std::wstring, CanState> g_known;    // by the can's Key
std::unordered_map<void*, CanState> g_classDefaults;   // by class

uint32_t g_published = 0, g_landed = 0;

bool IsCan(void* object) {
    void* const cls = R::ClassOf(object);
    if (!cls) return false;
    auto it = g_isCanClass.find(cls);
    if (it != g_isCanClass.end()) return it->second;
    bool can = false;
    for (void* c = cls; c && !can; c = R::SuperStructOf(c)) can = R::NameEquals(R::NameOf(c), L"prop_cannedFood_C");
    g_isCanClass.emplace(cls, can);
    return can;
}

bool ReadState(void* object, CanState& out) {
    static R::InstanceOffset sOpened{L"opened"}, sFoods{L"foods"};
    const int32_t openedOff = sOpened.Of(object), foodsOff = sFoods.Of(object);
    if (openedOff < 0 || foodsOff < 0) return false;
    const auto* base = static_cast<const uint8_t*>(object);
    out.opened = base[openedOff] != 0;
    out.foods = *reinterpret_cast<const int32_t*>(base + foodsOff);
    return true;
}

// What a can this peer has not seen starts as: its class's defaults.
CanState DefaultsOf(void* object) {
    void* const cls = R::ClassOf(object);
    auto it = g_classDefaults.find(cls);
    if (it != g_classDefaults.end()) return it->second;
    CanState d{};
    if (void* cdo = R::FindClassDefaultObject(R::ClassNameOf(object).c_str())) ReadState(cdo, d);
    g_classDefaults.emplace(cls, d);
    return d;
}

void OnInitPost(const sg::Call& c) {
    if (!c.object || !IsCan(c.object)) return;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (key.empty() || key == L"None") return;   // mid-construction: the birth paths carry it
    CanState now{};
    if (!ReadState(c.object, now)) return;
    auto it = g_known.find(key);
    const CanState before = it != g_known.end() ? it->second : DefaultsOf(c.object);
    g_known[key] = now;
    if (now == before) return;
    // A record landing here is the far side's change arriving, not a new one.
    if (ue_wrap::save_record::IsApplying()) {
        ++g_landed;
        UE_LOGI("prop_can_state: a record landed on '%ls' -> opened=%d foods=%d", key.c_str(), now.opened ? 1 : 0,
                now.foods);
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    // A joining client's load churn is the host's state arriving through the save, as in prop_record_refresh.
    if (s->role() == coop::net::Role::Client && !coop::join_membership_sweep::HasLoadTailQuiesced()) return;
    const bool sent = coop::prop_save_data::Publish(s, c.object, key);
    if (sent) ++g_published;
    UE_LOGI("prop_can_state: '%ls' changed here: opened %d->%d foods %d->%d -- record %s (#%u)", key.c_str(),
            before.opened ? 1 : 0, now.opened ? 1 : 0, before.foods, now.foods, sent ? "published" : "NOT sent",
            g_published);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_reg != Reg::Pending) return;
    g_reg = sg::WatchName(kRowInit, kTag, nullptr, &OnInitPost) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    if (g_settled || g_reg == Reg::Pending) return;
    if (g_reg == Reg::Registered) sg::ResolvePendingNames();
    if (g_reg == Reg::Registered && sg::NameWatchLive(kRowInit, kTag)) {
        g_settled = true;
        UE_LOGI("prop_can_state: the watch on every class's init is live");
    } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowInit, kTag)) {
        g_settled = true;
        UE_LOGE("prop_can_state: the watch on every class's init is dead -- a can's lid and contents stay on the "
                "machine that changed them");
    }
}

void OnDisconnect() {
    if (g_published || g_landed)
        UE_LOGI("prop_can_state: session end -- published=%u landed=%u", g_published, g_landed);
    g_known.clear();
    g_published = g_landed = 0;
}

}  // namespace coop::props::prop_can_state
