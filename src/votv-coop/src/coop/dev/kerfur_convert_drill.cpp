// coop/dev/kerfur_convert_drill.cpp -- see coop/dev/kerfur_convert_drill.h.

#include "coop/dev/kerfur_convert_drill.h"

#include "coop/config/config.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_snapshot.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::dev::kerfur_convert_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
using Clock = std::chrono::steady_clock;

constexpr const wchar_t* kNpcClass     = L"kerfurOmega_C";
constexpr const wchar_t* kPropClass    = L"prop_kerfurOmega_C";   // dropKerfurProp's default prop class
constexpr const wchar_t* kCarriedDisc  = L"prop_floppyDisc_R_C";  // floppyType 0 (lib_C::floppyFromType)
constexpr const wchar_t* kLocalDisc    = L"prop_floppyDisc_G_C";  // the client's own, told apart by class
constexpr uint8_t kActionTurnOn = 8;  // the prop's event graph calls spawnKerfuro on this action alone
// The observation window after the turn-off is applied: the old ghost custody destroyed an un-adopted
// disc 4 s after its claim, so a disc alive 6 s after the apply outlived it. A window, not a readiness
// wait: the defect under test is a timeout.
constexpr auto kSurviveWindow = std::chrono::seconds(6);

enum class HostStep { WaitJoin, WaitNpc, Idle };
enum class ClientStep { WaitQuiet, WaitKerfur, WaitOffApplied, Observe, WaitOnApplied, Done, Invalid };

std::atomic<coop::net::Session*> g_session{nullptr};
HostStep   g_host   = HostStep::WaitJoin;
ClientStep g_client = ClientStep::WaitQuiet;
bool g_saidArm = false;
int  g_kerfursBefore = 0;  // host: kerfur NPCs alive at the spawn

// Client state.
std::vector<uint32_t> g_npcBefore, g_propBefore, g_carriedBefore;  // mirror eids present before each step
uint32_t g_npcEid = 0, g_propEid = 0;
void*    g_localDisc = nullptr;
int32_t  g_localDiscIdx = -1;
Clock::time_point g_offAppliedAt{};
int  g_carriedArrived = 0;
// Did the form stand when the client's own verb call returned? A client that converts locally has
// destroyed it inside the call; one whose gate refused the verb still has it until the host's
// KerfurConvert arrives.
bool g_offStood = false, g_onStood = false;

bool IsEnabled_() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfur_convert_drill);
    return s;
}

// ---- host ----

// The fields a kerfur's dropKerfurProp reads for its carried disc, resolved once.
struct DiscFields { void* cls = nullptr; int32_t hasOff = -1; uint8_t hasMask = 0; int32_t typeOff = -1; };
DiscFields g_disc;
bool g_saidNoFields = false;

bool ResolveDiscFields() {
    if (g_disc.cls) return true;
    void* cls = R::FindClass(kNpcClass);
    if (!cls) return false;  // the class loads with the world
    DiscFields d;
    d.cls = cls;
    d.typeOff = R::FindPropertyOffset(cls, L"floppyType");
    if (!R::FindBoolProperty(cls, L"hasFloppy", d.hasOff, d.hasMask) || d.typeOff < 0) {
        if (!g_saidNoFields) UE_LOGW("[KERFUR-DRILL] host INVALID -- the kerfur's floppy fields did not resolve");
        g_saidNoFields = true;
        return false;
    }
    g_disc = d;
    return true;
}

// Every live kerfur NPC carries a red disc: hasFloppy and floppyType 0. Returns how many there are.
int GiveEveryKerfurADisc() {
    struct Ctx { int n; } ctx{0};
    OI::ForEachInstance(g_disc.cls, [](void* p, void* obj, int32_t index) {
        if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        auto* base = static_cast<uint8_t*>(obj);
        base[g_disc.hasOff] = static_cast<uint8_t>(base[g_disc.hasOff] | g_disc.hasMask);
        *reinterpret_cast<int32_t*>(base + g_disc.typeOff) = 0;
        ++static_cast<Ctx*>(p)->n;
    }, &ctx);
    return ctx.n;
}

// From its arm on, the host keeps every kerfur NPC carrying a red disc, so the one the client turns off
// carries one whichever it is: element ids are recycled and the host's spawn can reach the client before
// the client's load tail is quiet, so the client cannot tell the new kerfur from an old one. The spawn,
// once a client's join is over, makes sure one exists. SpawnKerfurOmega posts its spawn, and the object
// index trails the engine by a tick or more, so the new kerfur is counted, and given its disc, a few
// ticks after it is born; the client, which acts only after its load tail is quiet, asks seconds later.
void TickHost(coop::net::Session* s) {
    if (!ResolveDiscFields()) return;
    const int kerfurs = GiveEveryKerfurADisc();
    switch (g_host) {
    case HostStep::WaitJoin: {
        int slot = -1;
        for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers) && slot < 0; ++i)
            if (s->IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i)) slot = i;
        if (slot < 0) return;
        g_kerfursBefore = kerfurs;
        UE_LOGI("[KERFUR-DRILL] host: slot %d's join is over; spawning a kerfur (%d already carry a red disc)",
                slot, kerfurs);
        coop::dev::spawn_npc::SpawnKerfurOmega();
        g_host = HostStep::WaitNpc;
        return;
    }
    case HostStep::WaitNpc:
        if (kerfurs <= g_kerfursBefore) return;
        UE_LOGI("[KERFUR-DRILL] host: the new kerfur is up; %d kerfur NPC(s) carry a red disc (floppyType 0)",
                kerfurs);
        g_host = HostStep::Idle;
        return;
    default: return;
    }
}

// ---- client ----

void ClientInvalid(const char* why) {
    g_client = ClientStep::Invalid;
    UE_LOGW("[KERFUR-DRILL] client INVALID -- %s", why);
}

// The live mirrors of exactly one class: its instances from the object index, each resolved to its
// element, so a call costs the class's instances, never every mirror of its kind.
std::vector<EL::Element*> MirrorsOf(const wchar_t* className) {
    std::vector<EL::Element*> out;
    void* cls = OI::ClassByName(className);
    if (!cls) return out;  // not loaded: no instance, so no mirror
    OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
        if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
        auto& reg = EL::Registry::Get();
        EL::Element* el = reg.Get(reg.EidForActor(obj));
        if (el && el->IsMirror() && el->GetActor() == obj) static_cast<std::vector<EL::Element*>*>(p)->push_back(el);
    }, &out);
    return out;
}

std::vector<uint32_t> EidsOf(const std::vector<EL::Element*>& els) {
    std::vector<uint32_t> out;
    for (auto* el : els) out.push_back(static_cast<uint32_t>(el->GetId()));
    return out;
}

EL::Element* FirstNew(const std::vector<EL::Element*>& now, const std::vector<uint32_t>& before) {
    for (auto* el : now)
        if (std::find(before.begin(), before.end(), static_cast<uint32_t>(el->GetId())) == before.end()) return el;
    return nullptr;
}

EL::Element* ByEid(const std::vector<EL::Element*>& els, uint32_t eid) {
    for (auto* el : els)
        if (static_cast<uint32_t>(el->GetId()) == eid) return el;
    return nullptr;
}

bool CallTurnOff(void* npc) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(npc), L"dropKerfurProp");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(npc, f);
}

bool CallTurnOn(void* prop) {
    void* player = coop::players::Registry::Get().Local();
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"actionOptionIndex");
    if (!player || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    // The hit and the looked-at component stay zeroed: the prop's graph reads the action alone.
    if (!f.Set<void*>(L"player", player) || !f.Set<uint8_t>(L"action", kActionTurnOn)) return false;
    return ue_wrap::Call(prop, f);
}

void TickClient() {
    const auto now = Clock::now();
    switch (g_client) {
    case ClientStep::WaitQuiet: {
        if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;
        UE_LOGI("[KERFUR-DRILL] client: load tail quiet; taking the first kerfur NPC mirror (%zu here now)",
                MirrorsOf(kNpcClass).size());
        g_client = ClientStep::WaitKerfur;
        return;
    }
    case ClientStep::WaitKerfur: {
        const auto npcs = MirrorsOf(kNpcClass);
        if (npcs.empty()) return;
        EL::Element* npc = npcs.front();
        g_npcEid = static_cast<uint32_t>(npc->GetId());
        g_propBefore = EidsOf(MirrorsOf(kPropClass));
        g_carriedBefore = EidsOf(MirrorsOf(kCarriedDisc));
        ue_wrap::FVector loc{};
        if (!E::TryGetActorLocation(npc->GetActor(), loc)) { ClientInvalid("the kerfur mirror's location did not read"); return; }
        void* discCls = R::FindClass(kLocalDisc);
        if (!discCls) { ClientInvalid("the green disc class is not loaded"); return; }
        g_localDisc = E::SpawnActor(discCls, ue_wrap::FVector{loc.X + 200.f, loc.Y, loc.Z});
        if (!g_localDisc) { ClientInvalid("the green disc did not spawn"); return; }
        g_localDiscIdx = R::InternalIndexOf(g_localDisc);
        UE_LOGI("[KERFUR-DRILL] client: kerfur mirror eid=%u; a green disc of our own %p lies 2 m from it; "
                "turning the kerfur off (dropKerfurProp)", g_npcEid, g_localDisc);
        void* const npcActor = npc->GetActor();
        const int32_t npcIdx = npc->GetInternalIdx();
        if (!CallTurnOff(npcActor)) { ClientInvalid("the dropKerfurProp call did not dispatch"); return; }
        g_offStood = R::IsLiveByIndex(npcActor, npcIdx);
        g_client = ClientStep::WaitOffApplied;
        return;
    }
    case ClientStep::WaitOffApplied: {
        if (ByEid(MirrorsOf(kNpcClass), g_npcEid)) return;  // the NPC mirror still stands
        EL::Element* prop = FirstNew(MirrorsOf(kPropClass), g_propBefore);
        if (!prop) return;
        g_propEid = static_cast<uint32_t>(prop->GetId());
        g_offAppliedAt = now;
        UE_LOGI("[KERFUR-DRILL] client: turn-off applied -- the NPC mirror eid=%u is gone, the prop mirror is "
                "eid=%u; watching the green disc for %lld s", g_npcEid, g_propEid,
                static_cast<long long>(kSurviveWindow.count()));
        g_client = ClientStep::Observe;
        return;
    }
    case ClientStep::Observe: {
        const auto carried = MirrorsOf(kCarriedDisc);
        int arrived = 0;
        for (auto* el : carried)
            if (std::find(g_carriedBefore.begin(), g_carriedBefore.end(), static_cast<uint32_t>(el->GetId())) ==
                g_carriedBefore.end()) ++arrived;
        g_carriedArrived = arrived;
        if (now - g_offAppliedAt < kSurviveWindow) return;
        EL::Element* prop = ByEid(MirrorsOf(kPropClass), g_propEid);
        if (!prop) { ClientInvalid("the prop mirror is gone before the turn-on"); return; }
        g_npcBefore = EidsOf(MirrorsOf(kNpcClass));
        UE_LOGI("[KERFUR-DRILL] client: turning the prop back on (actionOptionIndex, action %u)",
                static_cast<unsigned>(kActionTurnOn));
        void* const propActor = prop->GetActor();
        const int32_t propIdx = prop->GetInternalIdx();
        if (!CallTurnOn(propActor)) { ClientInvalid("the actionOptionIndex call did not dispatch"); return; }
        g_onStood = R::IsLiveByIndex(propActor, propIdx);
        g_client = ClientStep::WaitOnApplied;
        return;
    }
    case ClientStep::WaitOnApplied: {
        if (ByEid(MirrorsOf(kPropClass), g_propEid)) return;  // the prop mirror still stands
        EL::Element* npc = FirstNew(MirrorsOf(kNpcClass), g_npcBefore);
        if (!npc) return;
        const bool discAlive = g_localDisc && R::IsLiveByIndex(g_localDisc, g_localDiscIdx);
        const bool pass = discAlive && g_carriedArrived == 1 && g_offStood && g_onStood;
        UE_LOGI("[KERFUR-DRILL] client: turn-on applied -- the prop mirror eid=%u is gone, the NPC mirror is eid=%u",
                g_propEid, static_cast<uint32_t>(npc->GetId()));
        UE_LOGI("[KERFUR-DRILL] client: the green disc is %s; the carried red disc arrived %d time(s) (want 1)",
                discAlive ? "ALIVE" : "GONE", g_carriedArrived);
        UE_LOGI("[KERFUR-DRILL] client: when its own call returned, the NPC %s and the prop %s (want both "
                "standing: the host converts)", g_offStood ? "STOOD" : "WAS DESTROYED HERE",
                g_onStood ? "STOOD" : "WAS DESTROYED HERE");
        UE_LOGI("[KERFUR-DRILL] client DONE %s", pass ? "PASS" : "FAIL");
        g_client = ClientStep::Done;
        return;
    }
    default: return;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!IsEnabled_()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled_()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_saidArm) {
        g_saidArm = true;
        UE_LOGI("[KERFUR-DRILL] %s armed", s->role() == coop::net::Role::Host ? "host" : "client");
    }
    if (s->role() == coop::net::Role::Host) TickHost(s);
    else if (s->connected()) TickClient();
}

void OnDisconnect() {
    g_host = HostStep::WaitJoin;
    g_client = ClientStep::WaitQuiet;
    g_saidArm = false;
    g_kerfursBefore = 0;
    g_disc = DiscFields{};
    g_saidNoFields = false;
    g_npcBefore.clear();
    g_propBefore.clear();
    g_carriedBefore.clear();
    g_npcEid = g_propEid = 0;
    g_localDisc = nullptr;
    g_localDiscIdx = -1;
    g_carriedArrived = 0;
    g_offStood = g_onStood = false;
}

}  // namespace coop::dev::kerfur_convert_drill
