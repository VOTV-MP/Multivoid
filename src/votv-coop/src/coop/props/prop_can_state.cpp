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
#include "ue_wrap/engine/world_identity.h"
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

struct CanState {
    bool    opened = false;
    int32_t foods = 0;
    bool operator==(const CanState& o) const { return opened == o.opened && foods == o.foods; }
};
std::unordered_map<std::wstring, CanState> g_known;    // by the can's Key

// Where `opened` lives on a class: its byte and its bit among the class's packed bools (off < 0: not declared).
struct BoolSlot { int32_t off = -1; uint8_t mask = 0; };

// Everything asked of a class once: whether it is a can (a variant, prop_cannedFood_0_C, is a child of the base),
// where its two fields live, and the state a fresh one starts as. A class a world reload frees can come back at the
// address of another, so the table goes with the world (SyncWorld).
struct ClassInfo {
    bool     can = false;
    BoolSlot opened;
    int32_t  foodsOff = -1;
    bool     haveDefaults = false;
    bool     missLogged = false;
    CanState defaults;
};
std::unordered_map<void*, ClassInfo> g_class;
uint32_t g_world = 0;   // the world_identity generation the tables above belong to

uint32_t g_published = 0, g_landed = 0;

// Per-world tables go with the world, keyed on the generation as vent_crawler_sync keys its show.
void SyncWorld() {
    // Generation() bumps only when CurrentWorld() is observed to change; a can's init in a load burst can run before
    // the pump's tick has looked, so look here first (world_identity.h:55-58).
    ue_wrap::world_identity::CurrentWorld();
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_world) return;
    g_world = gen;
    g_class.clear();
    g_known.clear();
}

ClassInfo& InfoOf(void* cls) {
    auto it = g_class.find(cls);
    if (it != g_class.end()) return it->second;
    ClassInfo info;
    for (void* c = cls; c && !info.can; c = R::SuperStructOf(c))
        info.can = R::NameEquals(R::NameOf(c), L"prop_cannedFood_C");
    if (info.can) {
        // `opened` is a Blueprint bool: one bit of a byte it may share with its neighbours, so it is read through its mask.
        if (!R::FindBoolProperty(cls, L"opened", info.opened.off, info.opened.mask)) info.opened = BoolSlot{};
        info.foodsOff = R::FindPropertyOffset(cls, L"foods");
        UE_LOGI("prop_can_state: class %ls: opened @0x%X mask 0x%02X, foods @0x%X",
                R::ToString(R::NameOf(cls)).c_str(), info.opened.off, info.opened.mask, info.foodsOff);
    }
    return g_class.emplace(cls, info).first->second;
}

bool IsCan(void* object) {
    void* const cls = R::ClassOf(object);
    return cls && InfoOf(cls).can;
}

bool ReadState(void* object, CanState& out) {
    const ClassInfo& info = InfoOf(R::ClassOf(object));
    if (!info.can || info.opened.off < 0 || info.opened.mask == 0 || info.foodsOff < 0) return false;
    const auto* base = static_cast<const uint8_t*>(object);
    out.opened = (base[info.opened.off] & info.opened.mask) != 0;
    out.foods = *reinterpret_cast<const int32_t*>(base + info.foodsOff);
    return true;
}

// What a can this peer has not seen starts as: its class's defaults. A class whose default object cannot be read is
// said once and asked again at the next can, never remembered as {false, 0}: that would make every fresh can look changed.
CanState DefaultsOf(void* object) {
    ClassInfo& info = InfoOf(R::ClassOf(object));
    if (info.haveDefaults) return info.defaults;
    // A miss walks the object array once per class per world (the table goes with the world), not on every new can.
    if (info.missLogged) return CanState{};
    CanState d{};
    void* const cdo = R::FindClassDefaultObject(R::ClassNameOf(object).c_str());
    if (cdo && ReadState(cdo, d)) {
        info.defaults = d;
        info.haveDefaults = true;
        return d;
    }
    if (!info.missLogged) {
        info.missLogged = true;
        UE_LOGW("prop_can_state: no readable default object for class %ls -- a fresh can is compared with opened=0 foods=0",
                R::ClassNameOf(object).c_str());
    }
    return d;
}

void OnInitPost(const sg::Call& c) {
    SyncWorld();
    if (!c.object || !IsCan(c.object)) return;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (key.empty() || key == L"None") return;   // mid-construction: the birth paths carry it
    CanState now{};
    if (!ReadState(c.object, now)) return;
    auto it = g_known.find(key);
    const CanState before = it != g_known.end() ? it->second : DefaultsOf(c.object);
    // What this peer knows moves with what it sent or saw: a publish that was declined leaves the old value, so the
    // next init compares again and offers the change again.
    if (now == before) {
        g_known[key] = now;
        return;
    }
    // A record landing here is the far side's change arriving, not a new one. Not `fromOurCode` as in
    // prop_record_refresh: the drill runs a can's own verbs through our CallFunction, so that flag would be true for the
    // change under test too; IsApplying is true only inside ApplyRecord.
    if (ue_wrap::save_record::IsApplying()) {
        g_known[key] = now;
        ++g_landed;
        UE_LOGI("prop_can_state: a record landed on '%ls' -> opened=%d foods=%d", key.c_str(), now.opened ? 1 : 0,
                now.foods);
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) {   // nothing to tell anyone; a join's snapshot carries the can
        g_known[key] = now;
        return;
    }
    // A joining client's load churn is the host's state arriving through the save, as in prop_record_refresh.
    if (s->role() == coop::net::Role::Client && !coop::join_membership_sweep::HasLoadTailQuiesced()) {
        g_known[key] = now;
        return;
    }
    const bool sent = coop::prop_save_data::Publish(s, c.object, key);
    if (sent) {
        g_known[key] = now;
        ++g_published;
    }
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

Counts CountsNow() { return {g_published, g_landed}; }

void OnDisconnect() {
    if (g_published || g_landed)
        UE_LOGI("prop_can_state: session end -- published=%u landed=%u", g_published, g_landed);
    g_published = g_landed = 0;
}

}  // namespace coop::props::prop_can_state
