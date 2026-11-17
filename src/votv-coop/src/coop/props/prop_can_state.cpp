// coop/props/prop_can_state.cpp -- see coop/props/prop_can_state.h.

#include "coop/props/prop_can_state.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/hand_record_sync.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
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
// What this peer knows of each can, by its Key, and the actor it was read on. A Key outlives its actor: a hand's drop
// spawns the can anew under the same Key; the host's copy of a client's drop is born with its class's defaults until the
// client's record lands.
struct Known {
    CanState state;
    ue_wrap::CachedObjRef actor;
    uint32_t published = 0;  // records published for this Key
};
std::unordered_map<std::wstring, Known> g_known;

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
uint32_t g_withheld = 0;   // a change of a can whose author's record is owed, not sent
uint32_t g_rebirths = 0;          // an init of a new actor under a Key this peer knew
uint32_t g_rebirthsCarried = 0;   // ... whose state already equalled what this peer knew

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

// Whether the host awaits the author's record of this Key from a client: the can is the host's copy of that client's drop.
bool RecordOwed(const std::wstring& key) {
    for (uint8_t s = 1; s < coop::net::kMaxPeers; ++s)
        if (coop::prop_save_data::AwaitsRecordFrom(key, s)) return true;
    return false;
}

void OnInitPost(const sg::Call& c) {
    SyncWorld();
    if (!c.object || !IsCan(c.object)) return;
    // A can in the hand opened or eaten from keeps its hold and changes its state: its record is read again for the hand
    // mirrors. Before the key and publish returns, which a held can mid-construction or a quiet session would skip.
    coop::hand_record_sync::NoteHeldItemChanged(c.object);
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (key.empty() || key == L"None") return;   // mid-construction: the birth paths carry it
    CanState now{};
    if (!ReadState(c.object, now)) return;
    auto it = g_known.find(key);
    // A new actor under a known Key is the can born again -- a hand's drop, a place, a respawn -- and its state is the
    // birth's: the host's copy of a client's drop is born with its class's defaults, and the author's record follows it
    // (coop/props/prop_save_data). It is adopted here, never published as this peer's change, which put a dropped
    // can back to full and shut on every peer. Until that record lands the host publishes nothing for the Key (below).
    // [dev] the can drill's red arm (can_adopt_off): the adoption is not made, and a new actor is compared with what
    // this peer knew, as before it.
    static const bool adoptOff = coop::config::ResolveFlag(::coop::config_registry::rows::can_adopt_off);
    // Counted before the red arm's test, so the drill reads whether a rebirth ever carries anything but its state.
    const bool reborn = it != g_known.end() && !it->second.actor.Is(c.object);
    if (reborn) {
        ++g_rebirths;
        if (it->second.state == now) ++g_rebirthsCarried;
    }
    if (reborn && !adoptOff) {
        if (!(it->second.state == now))
            UE_LOGI("prop_can_state: '%ls' born again here as opened=%d foods=%d -- adopted, never published as a change",
                    key.c_str(), now.opened ? 1 : 0, now.foods);
        it->second.state = now;
        it->second.actor.Set(c.object);
        return;
    }
    const CanState before = it != g_known.end() ? it->second.state : DefaultsOf(c.object);
    // What this peer knows moves with what it sent or saw: a publish that was declined leaves the old value, so the
    // next init compares again and offers the change again.
    auto know = [&] {
        Known& k = g_known[key];
        k.state = now;
        k.actor.Set(c.object);
    };
    if (now == before) {
        know();
        return;
    }
    // A record landing here is the far side's change arriving, not a new one. Not `fromOurCode` as in
    // prop_record_refresh: the drill runs a can's own verbs through our CallFunction, so that flag would be true for the
    // change under test too; IsApplying is true only inside ApplyRecord.
    if (ue_wrap::save_record::IsApplying()) {
        know();
        ++g_landed;
        UE_LOGI("prop_can_state: a record landed on '%ls' -> opened=%d foods=%d", key.c_str(), now.opened ? 1 : 0,
                now.foods);
        return;
    }
    // The host's copy of a client's drop awaits that client's record, and the host's own wait for it says so
    // (prop_save_data::AwaitsRecordFrom, cleared when the record lands, its sender goes, it times out or the session
    // ends): a change here before it lands is not sent, and not known either, so the record that lands is the one that
    // stands. A hand actor and a host-authored drop are owed nothing and publish their changes at once.
    if (RecordOwed(key)) {
        if ((++g_withheld & (g_withheld - 1)) == 0)
            UE_LOGI("prop_can_state: '%ls' changed here (opened %d->%d foods %d->%d) while its author's record is owed -- "
                    "not sent (#%u)", key.c_str(), before.opened ? 1 : 0, now.opened ? 1 : 0, before.foods, now.foods,
                    g_withheld);
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) {   // nothing to tell anyone; a join's snapshot carries the can
        know();
        return;
    }
    // A joining client's load churn is the host's state arriving through the save, as in prop_record_refresh.
    if (s->role() == coop::net::Role::Client && !coop::join_membership_sweep::HasLoadTailQuiesced()) {
        know();
        return;
    }
    const bool sent = coop::prop_save_data::Publish(s, c.object, key);
    if (sent) {
        know();
        ++g_known[key].published;
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

Counts CountsNow() { return {g_published, g_landed, g_withheld, g_rebirths, g_rebirthsCarried}; }

unsigned PublishedFor(const std::wstring& key) {
    const auto it = g_known.find(key);
    return it == g_known.end() ? 0u : it->second.published;
}

void OnDisconnect() {
    if (g_published || g_landed || g_withheld || g_rebirths)
        UE_LOGI("prop_can_state: session end -- published=%u landed=%u withheld=%u "
                "rebirths=%u (born carrying the known state=%u)",
                g_published, g_landed, g_withheld, g_rebirths, g_rebirthsCarried);
    g_published = g_landed = g_withheld = g_rebirths = g_rebirthsCarried = 0;
}

}  // namespace coop::props::prop_can_state
