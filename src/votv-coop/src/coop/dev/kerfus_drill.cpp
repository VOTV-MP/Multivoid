// coop/dev/kerfus_drill.cpp -- see coop/dev/kerfus_drill.h.

#include "coop/dev/kerfus_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_snapshot.h"

#include "ue_wrap/actors/kerfus.h"
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
namespace UK = ue_wrap::kerfus;

// The Kerfus and its colour variants, each an exact class to the object index.
constexpr const wchar_t* kKerfusClasses[] = {
    L"p_kerfus_C", L"p_kerfus_p_C", L"p_kerfus_r_C", L"p_kerfus_y_C", L"p_kerfus_col_C", L"p_kerfus_col_gamer_C",
};
constexpr uint8_t  kActionToggle    = 8;       // on/off
constexpr float    kPressReachCm    = 250.f;   // inside the host's 400 uu reach test
constexpr float    kAwayMinCm       = 1000.f;  // a walk, or a possess, 10 to 30 m out
constexpr float    kAwayMaxCm       = 3000.f;
// The pile the client walks to, and the possess point, lie this far from the host, flat (the pick
// refuses nearer piles). The Kerfus stops about 1 m from the player it follows (its tick judges the
// approach by a 100 uu distance), so a settle by one player is never by the other.
constexpr float    kApartCm         = 1000.f;
constexpr float    kFollowCm        = 200.f;   // settled by a player: its 100 uu stop, the puppet's lag
constexpr float    kArriveCm        = 400.f;   // the possess's own arrival test
constexpr float    kMovedCm         = 500.f;   // the client's copy moved this far on the host's drive
constexpr uint64_t kSettleMs        = 5000;    // and stayed: one driving past leaves in under 2 s
constexpr uint64_t kHostSettleMs    = kSettleMs + 2000;  // the host's own settle: the client judges first
constexpr float    kStillCm         = 50.f;    // the client stood through the host's settle
constexpr float    kClientAwayCm    = 800.f;   // the client at its pile: 10 m flat from the host, less its arrival
constexpr uint64_t kFollowWindowMs  = 45000;   // from the client's arrival
constexpr uint64_t kStateWindowMs   = 15000;   // the host's state after a press
// While possessed its arrival check pins energy here (p_kerfus.cpp:619): the possess, seen in the game's own
// state. A Kerfus possessed while it sits 1 m from its player stalls there -- its wheel drive is scaled by
// its distance to that player, zero at 100 uu (:355-372) -- so the client walks back to let it leave.
constexpr float    kPinnedEnergy    = 66.61f;
constexpr float    kPinTolerance    = 0.5f;
constexpr uint64_t kPinWindowMs     = 30000;   // from the client's settle: the host's settle, the possess, its pin
constexpr uint64_t kWatchWindowMs   = 60000;   // from the client's walk back
constexpr uint64_t kPossessWindowMs = 150000;  // the host's own: the pin wait, the walk back, the watch
constexpr uint64_t kSampleMs        = 100;
constexpr uint64_t kReportMs        = 5000;
constexpr int      kWalkDeadlineS   = 60;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_saidArm = false;

bool IsEnabled_() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfus_drill);
    return s;
}

// ---- shared ----

// The first live Kerfus of the family that `accept` takes, with `arg`.
void* FindKerfus(bool (*accept)(void* obj, const void* arg), const void* arg) {
    for (const wchar_t* name : kKerfusClasses) {
        void* cls = OI::ClassByName(name);
        if (!cls) continue;
        struct Ctx { bool (*accept)(void*, const void*); const void* arg; void* found; } ctx{accept, arg, nullptr};
        OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
            auto* c = static_cast<Ctx*>(p);
            if (c->found || !obj) return;
            if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            if (c->accept(obj, c->arg)) c->found = obj;
        }, &ctx);
        if (ctx.found) return ctx.found;
    }
    return nullptr;
}

bool IsWireMirror(void* obj, const void*) {
    auto& reg = EL::Registry::Get();
    EL::Element* el = reg.Get(reg.EidForActor(obj));
    return el && el->IsMirror();
}

bool IsOn(void* obj) {
    bool on = false;
    return UK::ReadActive(obj, on) && on;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

// On, and within `cm` of `at`: the Kerfus the client pressed, settled by it.
struct Near {
    ue_wrap::FVector at;
    float cm;
};
bool IsOnNear(void* obj, const void* arg) {
    const auto* n = static_cast<const Near*>(arg);
    ue_wrap::FVector k{};
    return IsOn(obj) && E::TryGetActorLocation(obj, k) && Dist(k, n->at) <= n->cm;
}

// A slot's body here: the local player, or a puppet once it has taken a pose (before that it stands
// at a placeholder).
bool BodyAt(int slot, ue_wrap::FVector& at) {
    auto& reg = coop::players::Registry::Get();
    if (slot == reg.LocalPeerId()) {
        void* me = reg.Local();
        return me && E::TryGetActorLocation(me, at);
    }
    coop::RemotePlayer* rp = reg.Puppet(static_cast<uint8_t>(slot));
    return rp && rp->valid() && rp->HasPose() && E::TryGetActorLocation(rp->GetActor(), at);
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The director blocks, so a walk runs on a worker; a step polls its state.
std::shared_ptr<coop::director::BackgroundWalk> g_walk;

void StartWalk(const ue_wrap::FVector& to, float reachCm) {
    g_walk = coop::director::StartBackgroundWalk(to, reachCm, kWalkDeadlineS);
}

int WalkState() { return g_walk ? g_walk->state.load() : 2; }

// ---- host ----

// The host stays where the save put it, so a Kerfus that follows player 0 comes to it. Once the Kerfus
// has settled by the client, the host possesses it -- the haunting's own verb -- to a pile 10 to 30 m
// from the client by the shortest route from there, and 10 m from the host: it must drive there, arrive
// and go off, which its path goal decides.
enum class HostStep { WaitJoin, WaitSettled, Possessed, Idle };
HostStep g_host = HostStep::WaitJoin;
int g_joined = -1;
void* g_hostKerfus = nullptr;
int32_t g_hostKerfusIdx = -1;
ue_wrap::FVector g_possessAt{};
ue_wrap::FVector g_clientAt{};  // where the client stood when the host's settle began
uint64_t g_hostSampleMs = 0, g_hostSince = 0, g_hostStepMs = 0;

void HostInvalid(const char* why) {
    g_host = HostStep::Idle;
    UE_LOGW("[KERFUS-DRILL] INVALID (host) -- %s", why);
}

void PossessAway(void* k, const ue_wrap::FVector& client, uint64_t ms) {
    // The route is judged from the client's body, where the Kerfus stands.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(g_joined));
    void* from = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    ue_wrap::FVector host{};
    coop::director::DirectorGoal goal;
    if (!from || !BodyAt(0, host) ||
        !coop::director::PickReachablePile(from, kAwayMinCm, kAwayMaxCm, goal, &host, kApartCm)) {
        HostInvalid("no nav-reachable pile 10 to 30 m from the client and 10 m from me to possess it to");
        return;
    }
    g_hostKerfus = k;
    g_hostKerfusIdx = R::InternalIndexOf(k);
    g_possessAt = goal.targetPos;
    if (!UK::RunPossess(k, g_possessAt)) { HostInvalid("the possess call did not run"); return; }
    UE_LOGI("[KERFUS-DRILL] host: the Kerfus settled by slot %d -- possessing it to (%.0f,%.0f,%.0f), %.0f cm from "
            "the client", g_joined, g_possessAt.X, g_possessAt.Y, g_possessAt.Z, Dist(g_possessAt, client));
    g_hostStepMs = ms;
    g_host = HostStep::Possessed;
}

void TickHost(coop::net::Session* s, uint64_t ms) {
    if (g_host == HostStep::Idle) return;
    if (g_host == HostStep::WaitJoin) {
        for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers); ++i)
            if (s->IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i)) {
                g_joined = i;
                g_host = HostStep::WaitSettled;
                UE_LOGI("[KERFUS-DRILL] host: slot %d's join is over -- staying where the save put me; a Kerfus that "
                        "follows player 0 comes here", i);
                return;
            }
        return;
    }
    if (ms - g_hostSampleMs < kSampleMs) return;
    g_hostSampleMs = ms;
    if (g_host == HostStep::WaitSettled) {
        // A Kerfus that is on and settled by the client, the client standing at its pile, apart from me: one
        // trailing a client still on its walk decides nothing. The client's own verdict bounds this wait.
        ue_wrap::FVector cp{}, hp{};
        void* k = nullptr;
        if (BodyAt(g_joined, cp) && BodyAt(0, hp) && Dist(cp, hp) >= kClientAwayCm) {
            const Near byClient{cp, kFollowCm};
            k = FindKerfus(&IsOnNear, &byClient);
        }
        if (!k) {
            g_hostSince = 0;
        } else if (!g_hostSince || Dist(cp, g_clientAt) >= kStillCm) {
            g_hostSince = ms;  // the settle starts, or starts again where the client now stands
            g_clientAt = cp;
        } else if (ms - g_hostSince >= kHostSettleMs) {
            PossessAway(k, cp, ms);
        }
        return;
    }
    // Possessed: it arrives and goes off, or the window ends.
    if (!R::IsLiveByIndex(g_hostKerfus, g_hostKerfusIdx)) { HostInvalid("the possessed Kerfus is gone"); return; }
    ue_wrap::FVector kp{};
    const float fromPoint = E::TryGetActorLocation(g_hostKerfus, kp) ? Dist(kp, g_possessAt) : -1.f;
    if (!IsOn(g_hostKerfus)) {
        UE_LOGI("[KERFUS-DRILL] host: the possess arrived -- it went off %.0f cm from the point", fromPoint);
        g_host = HostStep::Idle;
    } else if (ms - g_hostStepMs >= kPossessWindowMs) {
        UE_LOGW("[KERFUS-DRILL] host: the possess has not arrived after %llu s -- it is %.0f cm from the point",
                static_cast<unsigned long long>(kPossessWindowMs / 1000), fromPoint);
        g_host = HostStep::Idle;
    }
}

// ---- client ----

enum class ClientStep {
    WaitQuiet, WalkOn, PressOn, WaitOn, WalkAway, Follow, WaitPinned, WalkBack, Watch, Done, Invalid
};
ClientStep g_client = ClientStep::WaitQuiet;
void* g_kerfus = nullptr;
int32_t g_kerfusIdx = -1;
uint32_t g_eid = 0;
bool g_stood = false;
bool g_moved = false;  // the client's copy moved kMovedCm while the host's drive stream held it
ue_wrap::FVector g_awayStart{};  // where the Kerfus stood when this client walked away: where it pressed it
ue_wrap::FVector g_settleAt{};   // where the Kerfus settled by this client
uint64_t g_stepMs = 0, g_sampleMs = 0, g_reportMs = 0, g_byMeSince = 0, g_byHostSince = 0, g_movedSampleMs = 0;

void ClientInvalid(const char* why) {
    g_client = ClientStep::Invalid;
    UE_LOGW("[KERFUS-DRILL] INVALID (client) -- %s", why);
}

void ClientDone(bool pass, const char* why) {
    g_client = ClientStep::Done;
    UE_LOGI("[KERFUS-DRILL] client DONE %s -- %s", pass ? "PASS" : "FAIL", why);
}

bool KerfusLive() { return g_kerfus && R::IsLiveByIndex(g_kerfus, g_kerfusIdx); }

// Walk to the Kerfus: the host measures a press from the sender's body.
bool WalkToKerfus() {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(g_kerfus, at)) return false;
    StartWalk(at, kPressReachCm);
    return true;
}

// The Kerfus's distance to this client and to the host; false when a place does not read.
bool Distances(float& toMe, float& toHost) {
    ue_wrap::FVector k{}, m{}, h{};
    const int me = coop::players::Registry::Get().LocalPeerId();
    if (!E::TryGetActorLocation(g_kerfus, k) || !BodyAt(me, m) || !BodyAt(0, h)) return false;
    toMe = Dist(k, m);
    toHost = Dist(k, h);
    return true;
}

// Once it is on: a pile 10 to 30 m from this client that the pick keeps 10 m from the host.
void StartWalkAway() {
    void* me = coop::players::Registry::Get().Local();
    ue_wrap::FVector host{};
    if (!me || !BodyAt(0, host) || !E::TryGetActorLocation(g_kerfus, g_awayStart)) {
        ClientInvalid("this client's, the host's or the Kerfus's place did not read");
        return;
    }
    coop::director::DirectorGoal goal;
    if (!coop::director::PickReachablePile(me, kAwayMinCm, kAwayMaxCm, goal, &host, kApartCm)) {
        ClientInvalid("no nav-reachable pile 10 to 30 m away and 10 m from the host");
        return;
    }
    UE_LOGI("[KERFUS-DRILL] client: the host turned it on -- walking to a pile at (%.0f,%.0f,%.0f), %.0f cm from "
            "the host, so it follows me", goal.targetPos.X, goal.targetPos.Y, goal.targetPos.Z,
            Dist(goal.targetPos, host));
    StartWalk(goal.targetPos, goal.reachCm);
    g_client = ClientStep::WalkAway;
}

// Once this client stands at its pile: the Kerfus settles by it, settles by the host (FAIL, the
// defect), or by neither inside the window (INVALID). Each settle holds 5 s, well clear of the other
// player, so a Kerfus driving past or stalled between the two decides nothing.
void TickFollow(uint64_t ms) {
    float toMe = 0.f, toHost = 0.f;
    if (!Distances(toMe, toHost)) { ClientInvalid("a place did not read"); return; }
    const bool byMe = toMe <= kFollowCm && toHost > 2.f * kFollowCm;
    const bool byHost = toHost <= kFollowCm && toMe > 2.f * kFollowCm;
    g_byMeSince = byMe ? (g_byMeSince ? g_byMeSince : ms) : 0;
    g_byHostSince = byHost ? (g_byHostSince ? g_byHostSince : ms) : 0;
    const double after = static_cast<double>(ms - g_stepMs) / 1000.0;
    if (g_byMeSince && ms - g_byMeSince >= kSettleMs) {
        UE_LOGI("[KERFUS-DRILL] client: it settled %.0f cm from me, %.1f s after I arrived (%.0f cm from the host) "
                "-- waiting for the host to possess it", toMe, after, toHost);
        if (!E::TryGetActorLocation(g_kerfus, g_settleAt)) { ClientInvalid("the Kerfus's place did not read"); return; }
        g_stepMs = ms;
        g_client = ClientStep::WaitPinned;
        return;
    }
    if (g_byHostSince && ms - g_byHostSince >= kSettleMs) {
        UE_LOGI("[KERFUS-DRILL] client: it settled %.0f cm from the host, %.1f s after I arrived (%.0f cm from me)",
                toHost, after, toMe);
        ClientDone(false, "the Kerfus this client turned on follows the host");
        return;
    }
    if (ms - g_stepMs >= kFollowWindowMs) {
        UE_LOGW("[KERFUS-DRILL] client: after %llu s it is %.0f cm from me and %.0f cm from the host",
                static_cast<unsigned long long>(kFollowWindowMs / 1000), toMe, toHost);
        ClientInvalid("the Kerfus settled by neither player inside the window");
        return;
    }
    if (ms - g_reportMs >= kReportMs) {
        g_reportMs = ms;
        UE_LOGI("[KERFUS-DRILL] client: it is %.0f cm from me, %.0f cm from the host", toMe, toHost);
    }
}

// After it settled by this client the host possesses it toward a pile 10 m away. The possess shows here
// as the haunting's energy pin; then this client walks back to where it pressed the Kerfus.
void TickPinned(uint64_t ms) {
    float energy = 0.f;
    if (!UK::ReadEnergy(g_kerfus, energy)) { ClientInvalid("the Kerfus's energy did not read"); return; }
    if (std::fabs(energy - kPinnedEnergy) < kPinTolerance) {
        UE_LOGI("[KERFUS-DRILL] client: the host possessed it (its energy is pinned at %.1f) -- walking back to where "
                "I pressed it", energy);
        StartWalk(g_awayStart, kPressReachCm);
        g_client = ClientStep::WalkBack;
        return;
    }
    if (ms - g_stepMs >= kPinWindowMs) ClientInvalid("no possess seen: its energy never showed the haunting's pin");
}

// Walking back and after: the possessed Kerfus must go off away from where it settled, its copy having moved
// on the host's drive stream (PASS). One whose path goal still answers this client follows it back and
// settles by it, on (FAIL); neither inside the window is INVALID.
void TickLeft(uint64_t ms) {
    bool on = false;
    ue_wrap::FVector k{};
    if (!UK::ReadActive(g_kerfus, on) || !E::TryGetActorLocation(g_kerfus, k)) {
        ClientInvalid("the Kerfus's `active` or place did not read");
        return;
    }
    if (!on) {
        const float fromSettle = Dist(k, g_settleAt);
        UE_LOGI("[KERFUS-DRILL] client: it went off %.0f cm from where it settled by me; it followed me %s the host's "
                "drive stream", fromSettle, g_moved ? "moving on" : "WITHOUT moving on");
        if (!g_moved) ClientDone(false, "this client's copy did not move on the host's drive stream");
        else if (fromSettle < kAwayMinCm - kArriveCm)
            ClientDone(false, "it went off where it settled, not at the possess point");
        else ClientDone(true, "its press stood, it followed this client, and the host's possess took it away");
        return;
    }
    if (g_client == ClientStep::WalkBack) {
        const int w = WalkState();
        if (w == 0) return;
        if (w == 2) { ClientInvalid("the walk back did not arrive"); return; }
        g_stepMs = g_reportMs = ms;
        g_byMeSince = 0;
        g_client = ClientStep::Watch;
        return;
    }
    float toMe = 0.f, toHost = 0.f;
    if (!Distances(toMe, toHost)) { ClientInvalid("a place did not read"); return; }
    const bool byMe = toMe <= kFollowCm && toHost > 2.f * kFollowCm;
    g_byMeSince = byMe ? (g_byMeSince ? g_byMeSince : ms) : 0;
    if (g_byMeSince && ms - g_byMeSince >= kSettleMs) {
        UE_LOGI("[KERFUS-DRILL] client: after the possess it settled %.0f cm from me again, on", toMe);
        ClientDone(false, "the host's possess did not take it: it followed this client back");
        return;
    }
    if (ms - g_stepMs >= kWatchWindowMs) {
        UE_LOGW("[KERFUS-DRILL] client: %llu s after the walk back it is on, %.0f cm from me",
                static_cast<unsigned long long>(kWatchWindowMs / 1000), toMe);
        ClientInvalid("after the possess it neither went off nor followed this client back");
        return;
    }
    if (ms - g_reportMs >= kReportMs) {
        g_reportMs = ms;
        UE_LOGI("[KERFUS-DRILL] client: it is on, %.0f cm from me, %.0f cm from where it settled", toMe,
                Dist(k, g_settleAt));
    }
}

void TickClient() {
    if (g_client != ClientStep::WaitQuiet && g_client != ClientStep::Done && g_client != ClientStep::Invalid &&
        !KerfusLive()) {
        ClientInvalid("the Kerfus mirror is gone");
        return;
    }
    const uint64_t ms = ::GetTickCount64();
    if ((g_client == ClientStep::WalkAway || g_client == ClientStep::Follow) && !g_moved &&
        ms - g_movedSampleMs >= kSampleMs && coop::prop_drive_stream::IsParked(g_kerfus)) {
        g_movedSampleMs = ms;
        ue_wrap::FVector now{};
        if (E::TryGetActorLocation(g_kerfus, now) && Dist(now, g_awayStart) >= kMovedCm) g_moved = true;
    }
    bool on = false;
    switch (g_client) {
    case ClientStep::WaitQuiet: {
        if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;
        void* k = FindKerfus(&IsWireMirror, nullptr);
        if (!k) { ClientInvalid("no Kerfus mirror once the join's load tail quiesced"); return; }
        g_kerfus = k;
        g_kerfusIdx = R::InternalIndexOf(k);
        g_eid = static_cast<uint32_t>(EL::Registry::Get().EidForActor(k));
        if (!UK::ReadActive(k, on)) { ClientInvalid("the Kerfus's `active` did not resolve"); return; }
        if (on) { ClientInvalid("the save's Kerfus is already on -- the drill starts from off"); return; }
        UE_LOGI("[KERFUS-DRILL] client: Kerfus mirror eid=%u %p is off -- walking to it", g_eid, g_kerfus);
        if (!WalkToKerfus()) { ClientInvalid("the Kerfus's place did not read"); return; }
        g_client = ClientStep::WalkOn;
        return;
    }
    case ClientStep::WalkOn: {
        const int w = WalkState();
        if (w == 0) return;
        if (w == 2) { ClientInvalid("the walk to the Kerfus did not arrive"); return; }
        g_client = ClientStep::PressOn;
        return;
    }
    case ClientStep::PressOn:
        if (!UK::RunActionOptionIndex(g_kerfus, coop::players::Registry::Get().Local(), kActionToggle)) {
            ClientInvalid("the actionOptionIndex call did not run");
            return;
        }
        // The press ran through this client's gate: its own copy must not have turned on.
        g_stood = UK::ReadActive(g_kerfus, on) && !on;
        UE_LOGI("[KERFUS-DRILL] client: pressed it on -- here it %s when the call returned",
                g_stood ? "STAYED OFF" : "TURNED ON LOCALLY");
        if (!g_stood) {
            // The press ran on this client's own copy: the defect under test, measured at once.
            ClientDone(false, "this client's own press turned its copy on");
            return;
        }
        g_stepMs = ms;
        g_client = ClientStep::WaitOn;
        return;
    case ClientStep::WaitOn:
        if (UK::ReadActive(g_kerfus, on) && on) { StartWalkAway(); return; }
        if (ms - g_stepMs >= kStateWindowMs) ClientInvalid("the host's state did not turn it on after the press");
        return;
    case ClientStep::WalkAway: {
        const int w = WalkState();
        if (w == 0) return;
        if (w == 2) { ClientInvalid("the walk away did not arrive"); return; }
        g_stepMs = g_sampleMs = g_reportMs = ms;
        g_byMeSince = g_byHostSince = 0;
        g_client = ClientStep::Follow;
        return;
    }
    case ClientStep::Follow:
    case ClientStep::WaitPinned:
    case ClientStep::WalkBack:
    case ClientStep::Watch:
        if (ms - g_sampleMs < kSampleMs) return;
        g_sampleMs = ms;
        if (g_client == ClientStep::Follow) TickFollow(ms);
        else if (g_client == ClientStep::WaitPinned) TickPinned(ms);
        else TickLeft(ms);
        return;
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
    if (s->role() == coop::net::Role::Host) TickHost(s, ::GetTickCount64());
    else if (s->connected()) TickClient();
}

void OnDisconnect() {
    g_saidArm = false;
    g_host = HostStep::WaitJoin;
    g_joined = -1;
    g_hostKerfus = nullptr;
    g_hostKerfusIdx = -1;
    g_possessAt = ue_wrap::FVector{};
    g_clientAt = ue_wrap::FVector{};
    g_hostSampleMs = g_hostSince = g_hostStepMs = 0;
    g_client = ClientStep::WaitQuiet;
    g_kerfus = nullptr;
    g_kerfusIdx = -1;
    g_eid = 0;
    g_stood = false;
    g_moved = false;
    g_awayStart = ue_wrap::FVector{};
    g_settleAt = ue_wrap::FVector{};
    g_stepMs = g_sampleMs = g_reportMs = g_byMeSince = g_byHostSince = g_movedSampleMs = 0;
}

}  // namespace coop::dev::kerfus_drill
