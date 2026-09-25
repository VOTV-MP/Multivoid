// coop/dev/kerfus_drill.cpp -- see coop/dev/kerfus_drill.h.

#include "coop/dev/kerfus_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_snapshot.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

namespace coop::dev::kerfus_drill {
namespace {

namespace EL = coop::element;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace R  = ue_wrap::reflection;

// The Kerfus and its colour variants, each an exact class to the object index.
constexpr const wchar_t* kKerfusClasses[] = {
    L"p_kerfus_C", L"p_kerfus_p_C", L"p_kerfus_r_C", L"p_kerfus_y_C", L"p_kerfus_col_C", L"p_kerfus_col_gamer_C",
};
constexpr uint8_t kActionToggle   = 8;       // on/off
constexpr float   kPressReachCm   = 250.f;   // inside the host's 400 uu reach test
constexpr float   kMovedCm        = 500.f;   // the client's copy moved this far on the host's drive
constexpr float   kHostWalkMinCm  = 1000.f;
constexpr float   kHostWalkMaxCm  = 3000.f;
constexpr int     kWalkDeadlineS  = 60;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_saidArm = false;

bool IsEnabled_() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfus_drill);
    return s;
}

// ---- shared ----

struct Fields {
    bool resolved = false;
    int32_t activeOff = -1;
    uint8_t activeMask = 0;
};
Fields g_f;

bool ReadActive(void* k, bool& out) {
    if (!g_f.resolved) {
        void* cls = R::ClassOf(k);
        if (!cls || !R::FindBoolProperty(cls, L"active", g_f.activeOff, g_f.activeMask)) return false;
        g_f.resolved = true;
    }
    out = (static_cast<const uint8_t*>(k)[g_f.activeOff] & g_f.activeMask) != 0;
    return true;
}

// The first live Kerfus of the family; on a client, the first one bound as a wire mirror.
void* FindKerfus(bool mirrorOnly) {
    for (const wchar_t* name : kKerfusClasses) {
        void* cls = OI::ClassByName(name);
        if (!cls) continue;
        struct Ctx { bool mirrorOnly; void* found; } ctx{mirrorOnly, nullptr};
        OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
            auto* c = static_cast<Ctx*>(p);
            if (c->found || !obj) return;
            if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            if (c->mirrorOnly) {
                auto& reg = EL::Registry::Get();
                EL::Element* el = reg.Get(reg.EidForActor(obj));
                if (!el || !el->IsMirror()) return;
            }
            c->found = obj;
        }, &ctx);
        if (ctx.found) return ctx.found;
    }
    return nullptr;
}

// Whether any live Kerfus is on: the host cannot tell which one the client pressed.
bool AnyKerfusOn() {
    for (const wchar_t* name : kKerfusClasses) {
        void* cls = OI::ClassByName(name);
        if (!cls) continue;
        struct Ctx { bool on; } ctx{false};
        OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
            auto* c = static_cast<Ctx*>(p);
            if (c->on || !obj) return;
            if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
            bool active = false;
            if (ReadActive(obj, active) && active) c->on = true;
        }, &ctx);
        if (ctx.on) return true;
    }
    return false;
}

bool PressToggle(void* kerfus) {
    void* player = coop::players::Registry::Get().Local();
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(kerfus), L"actionOptionIndex");
    if (!player || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"player", player) || !f.Set<uint8_t>(L"action", kActionToggle)) return false;
    return ue_wrap::Call(kerfus, f);
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The director blocks, so a walk runs on a worker; a step polls its state.
struct Walk {
    coop::director::DirectorGoal goal;
    std::atomic<int> state{0};  // 0 walking, 1 reached, 2 failed
};
std::shared_ptr<Walk> g_walk;

DWORD WINAPI WalkThread(LPVOID arg) {
    auto* holder = static_cast<std::shared_ptr<Walk>*>(arg);
    std::shared_ptr<Walk> w = *holder;
    delete holder;
    coop::director::ControlManager mgr;
    coop::director::AddWalkToProcesses(mgr, w->goal);
    mgr.Run(w->goal, kWalkDeadlineS);
    w->state.store(w->goal.reached ? 1 : 2);
    return 0;
}

void StartWalk(const ue_wrap::FVector& to, float reachCm) {
    g_walk = std::make_shared<Walk>();
    g_walk->goal.targetPos = to;
    g_walk->goal.reachCm = reachCm;
    g_walk->goal.epoch = coop::director::WalkEpoch();  // the walk ends with this session (EndWalks)
    auto* arg = new std::shared_ptr<Walk>(g_walk);
    if (HANDLE t = ::CreateThread(nullptr, 0, &WalkThread, arg, 0, nullptr)) ::CloseHandle(t);
    else { delete arg; g_walk->state.store(2); }
}

int WalkState() { return g_walk ? g_walk->state.load() : 2; }

// ---- host ----

enum class HostStep { WaitJoin, WaitOn, Walking, Idle };
HostStep g_host = HostStep::WaitJoin;

void TickHost(coop::net::Session* s) {
    switch (g_host) {
    case HostStep::WaitJoin:
        for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers); ++i)
            if (s->IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i)) {
                UE_LOGI("[KERFUS-DRILL] host: slot %d's join is over -- waiting for a Kerfus to be turned on", i);
                g_host = HostStep::WaitOn;
                return;
            }
        return;
    case HostStep::WaitOn: {
        if (!AnyKerfusOn()) return;
        void* player = coop::players::Registry::Get().Local();
        coop::director::DirectorGoal goal;
        if (!player || !coop::director::PickReachablePile(player, kHostWalkMinCm, kHostWalkMaxCm, goal)) {
            UE_LOGW("[KERFUS-DRILL] host INVALID -- no nav-reachable pile 10 to 30 m away to walk to");
            g_host = HostStep::Idle;
            return;
        }
        UE_LOGI("[KERFUS-DRILL] host: the Kerfus is on -- walking to a pile at (%.0f,%.0f,%.0f) so it follows",
                goal.targetPos.X, goal.targetPos.Y, goal.targetPos.Z);
        StartWalk(goal.targetPos, goal.reachCm);
        g_host = HostStep::Walking;
        return;
    }
    case HostStep::Walking: {
        const int w = WalkState();
        if (w == 0) return;
        UE_LOGI("[KERFUS-DRILL] host: the walk %s", w == 1 ? "arrived" : "did not arrive (the Kerfus still followed it)");
        g_host = HostStep::Idle;
        return;
    }
    default: return;
    }
}

// ---- client ----

enum class ClientStep { WaitQuiet, WalkOn, PressOn, WaitOn, WaitMoved, WalkOff, PressOff, WaitOff, Done, Invalid };
ClientStep g_client = ClientStep::WaitQuiet;
void* g_kerfus = nullptr;
int32_t g_kerfusIdx = -1;
uint32_t g_eid = 0;
ue_wrap::FVector g_onPos{};
bool g_stood = false;
bool g_parkedSeen = false;  // the host's drive stream held this copy while it moved

void ClientInvalid(const char* why) {
    g_client = ClientStep::Invalid;
    UE_LOGW("[KERFUS-DRILL] client INVALID -- %s", why);
}

bool KerfusLive() { return g_kerfus && R::IsLiveByIndex(g_kerfus, g_kerfusIdx); }

// Walk to the Kerfus: the host measures a press from the sender's body.
bool WalkToKerfus() {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(g_kerfus, at)) return false;
    StartWalk(at, kPressReachCm);
    return true;
}

void TickClient() {
    if (g_client != ClientStep::WaitQuiet && g_client != ClientStep::Done && g_client != ClientStep::Invalid &&
        !KerfusLive()) {
        ClientInvalid("the Kerfus mirror is gone");
        return;
    }
    bool on = false;
    switch (g_client) {
    case ClientStep::WaitQuiet: {
        if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;
        void* k = FindKerfus(/*mirrorOnly=*/true);
        if (!k) return;  // its mirror binds with the snapshot
        g_kerfus = k;
        g_kerfusIdx = R::InternalIndexOf(k);
        g_eid = static_cast<uint32_t>(EL::Registry::Get().EidForActor(k));
        if (!ReadActive(k, on)) { ClientInvalid("the Kerfus's `active` did not resolve"); return; }
        if (on) { ClientInvalid("the save's Kerfus is already on -- the drill starts from off"); return; }
        UE_LOGI("[KERFUS-DRILL] client: Kerfus mirror eid=%u %p is off -- walking to it", g_eid, g_kerfus);
        if (!WalkToKerfus()) { ClientInvalid("the Kerfus's place did not read"); return; }
        g_client = ClientStep::WalkOn;
        return;
    }
    case ClientStep::WalkOn:
    case ClientStep::WalkOff: {
        const int w = WalkState();
        if (w == 0) return;
        if (w == 2) { ClientInvalid("the walk to the Kerfus did not arrive"); return; }
        g_client = (g_client == ClientStep::WalkOn) ? ClientStep::PressOn : ClientStep::PressOff;
        return;
    }
    case ClientStep::PressOn:
        if (!PressToggle(g_kerfus)) { ClientInvalid("the actionOptionIndex call did not dispatch"); return; }
        // The press ran through this client's gate: its own copy must not have turned on.
        g_stood = ReadActive(g_kerfus, on) && !on;
        UE_LOGI("[KERFUS-DRILL] client: pressed it on -- here it %s when the call returned",
                g_stood ? "STAYED OFF" : "TURNED ON LOCALLY");
        if (!g_stood) {
            // The press ran on this client's own copy: the defect under test, measured at once.
            UE_LOGI("[KERFUS-DRILL] client DONE FAIL -- this client's own press turned its copy on");
            g_client = ClientStep::Done;
            return;
        }
        g_client = ClientStep::WaitOn;
        return;
    case ClientStep::WaitOn:
        if (!ReadActive(g_kerfus, on) || !on) return;
        if (!E::TryGetActorLocation(g_kerfus, g_onPos)) { ClientInvalid("the Kerfus's place did not read"); return; }
        UE_LOGI("[KERFUS-DRILL] client: the host turned it on -- watching it follow the host");
        g_client = ClientStep::WaitMoved;
        return;
    case ClientStep::WaitMoved: {
        if (coop::prop_drive_stream::IsParked(g_kerfus)) g_parkedSeen = true;
        ue_wrap::FVector now{};
        if (!E::TryGetActorLocation(g_kerfus, now) || Dist(now, g_onPos) < kMovedCm) return;
        UE_LOGI("[KERFUS-DRILL] client: it moved %.0f cm on the host's drive -- walking back to it",
                Dist(now, g_onPos));
        if (!WalkToKerfus()) { ClientInvalid("the Kerfus's place did not read"); return; }
        g_client = ClientStep::WalkOff;
        return;
    }
    case ClientStep::PressOff:
        if (!PressToggle(g_kerfus)) { ClientInvalid("the actionOptionIndex call did not dispatch"); return; }
        g_client = ClientStep::WaitOff;
        return;
    case ClientStep::WaitOff: {
        if (!ReadActive(g_kerfus, on) || on) return;
        const bool pass = g_stood && g_parkedSeen;
        UE_LOGI("[KERFUS-DRILL] client: the host turned it off -- its press stood, it moved %s the host's drive "
                "stream", g_parkedSeen ? "ON" : "WITHOUT");
        UE_LOGI("[KERFUS-DRILL] client DONE %s", pass ? "PASS" : "FAIL");
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
        UE_LOGI("[KERFUS-DRILL] %s armed", s->role() == coop::net::Role::Host ? "host" : "client");
    }
    if (s->role() == coop::net::Role::Host) TickHost(s);
    else if (s->connected()) TickClient();
}

void OnDisconnect() {
    g_saidArm = false;
    g_host = HostStep::WaitJoin;
    g_client = ClientStep::WaitQuiet;
    g_kerfus = nullptr;
    g_kerfusIdx = -1;
    g_eid = 0;
    g_stood = false;
    g_parkedSeen = false;
    g_f = Fields{};
}

}  // namespace coop::dev::kerfus_drill
