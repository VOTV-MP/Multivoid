// coop/dev/fire_ignite_drill.cpp -- see coop/dev/fire_ignite_drill.h.

#include "coop/dev/fire_ignite_drill.h"

#include "explosion_drill_source.h"  // HeldOf, WriteBool, WriteInt, ReadBool (co-located private header)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/explosion_sync.h"
#include "coop/world/salt_rocket_flight.h"
#include "coop/world/salt_rocket_ignite.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_fire.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::fire_ignite_drill {
namespace {

namespace DC  = coop::dev::drill_chat;
namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace XD  = coop::dev::explosion_drill::detail;
namespace SRF = coop::salt_rocket_flight;
namespace SRI = coop::salt_rocket_ignite;

constexpr const wchar_t* kRocketCls    = L"prop_saltrocket_C";
constexpr const wchar_t* kRocketPath   = L"/Game/objects/prop_saltrocket.prop_saltrocket_C";
constexpr const wchar_t* kLighterCls   = L"prop_lighter_C";
constexpr const wchar_t* kLighterPath  = L"/Game/objects/prop_lighter.prop_lighter_C";
constexpr float    kAheadCm       = 150.f;   // before the acting player, along its forward
constexpr float    kUpCm          = 50.f;    // above its location, so the rocket settles on the floor
constexpr float    kFlownCm       = 100.f;   // a copy that flew moved at least this far from its rest
constexpr uint64_t kCheckEveryMs  = 250;
constexpr uint64_t kSettleBoundMs = 10000;  // the spawn (host) or the find (client) to the rocket at rest
constexpr int      kRestChecks    = 4;      // ...that many reads in a row, a check apart, within a centimetre
constexpr uint64_t kFindBoundMs   = 20000;  // CLIENT: world-ready to the rocket's mirror
constexpr uint64_t kHoldBoundMs   = 5000;   // CLIENT: the take to the lighter in the hand
constexpr uint64_t kLifeBoundMs   = 60000;  // the light (or what the light caused) to the rocket's end
// The client says this once its start set is recorded; the host spawns the rocket only after it.
constexpr const char* kClientReadyLine = "fire-ignite client-ready";

enum class Arm : uint8_t { Off, Client, Host };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::fire_ignite_drill);
        return v == "client" ? Arm::Client : v == "host" ? Arm::Host : Arm::Off;
    }();
    return a;
}
const char* ArmName() { return ArmOf() == Arm::Client ? "client" : "host"; }

std::atomic<coop::net::Session*> g_session{nullptr};

uint64_t Now() { return ::GetTickCount64(); }

// A failure line; each role prints one FAIL or ABANDONED and is then done.
void FailLine(const char* role, const char* fmt, va_list ap) {
    char what[200];
    std::vsnprintf(what, sizeof(what), fmt, ap);
    UE_LOGW("[FIRE-IGNITE] FAIL %s %s", role, what);
}

// ---- one rocket's rest, read a check apart ----

struct Settle {
    int              reads = 0;
    uint64_t         nextMs = 0;
    ue_wrap::FVector last{};
};
enum class Rested : uint8_t { Working, Still, Gone };

// Still once the rocket moved less than a centimetre over kRestChecks reads in a row. `at` holds the last read.
Rested PollRest(Settle& st, void* rocket, ue_wrap::FVector& at) {
    const uint64_t now = Now();
    if (now < st.nextMs) return Rested::Working;
    st.nextMs = now + kCheckEveryMs;
    if (!rocket || !E::TryGetActorLocation(rocket, at)) return Rested::Gone;
    const float moved = std::fabs(at.X - st.last.X) + std::fabs(at.Y - st.last.Y) + std::fabs(at.Z - st.last.Z);
    st.reads = moved < 1.f ? st.reads + 1 : 0;
    st.last = at;
    return st.reads >= kRestChecks ? Rested::Still : Rested::Working;
}

// ===================================================================================================================
// HOST
// ===================================================================================================================

enum class HStep : uint8_t { Arm, Settle, Watch, Done };
HStep                  g_h = HStep::Arm;
uint64_t               g_hStepMs = 0, g_hLitMs = 0;
ue_wrap::CachedObjRef  g_hRocket;
Settle                 g_hSettle;
SRF::Counters          g_flight0{};
SRI::Counters          g_ignite0{};
uint32_t               g_captured0 = 0;
bool                   g_clientReady = false;   // the client said its start set is recorded

void HNext(HStep s) { g_h = s; g_hStepMs = Now(); }

void HFail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    FailLine("host", fmt, ap);
    va_end(ap);
    g_h = HStep::Done;
}
void HAbandon(const char* what) {
    UE_LOGW("[FIRE-IGNITE] ABANDONED host %s", what);
    g_h = HStep::Done;
}

uint32_t Delta(uint32_t now, uint32_t base) { return now >= base ? now - base : 0; }

// The spawn: the rocket 150 cm before, and 50 cm above, the acting player (the client's puppet, or the host's own).
void HostArm(coop::net::Session* s) {
    // The client's line, latched on sight from the first tick: a live chat row retires after seconds.
    if (!g_clientReady && DC::PollDue()) g_clientReady = DC::ChatHasLive(kClientReadyLine);
    if (!s->IsSlotWorldReady(1) || !g_clientReady) return;
    void* actor = ArmOf() == Arm::Client ? coop::puppet_drive::Puppet(1).GetActor()
                                         : coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!actor || !R::IsLive(actor) || !E::TryGetActorLocation(actor, at)) return;   // the puppet spawns with its pose
    void* cls = XD::ResolveClass(kRocketCls, kRocketPath);
    if (!cls) return HAbandon("spawn");
    const ue_wrap::FVector fwd = E::GetActorForwardVector(actor);
    void* rocket = E::SpawnActor(cls, {at.X + fwd.X * kAheadCm, at.Y + fwd.Y * kAheadCm, at.Z + kUpCm});
    if (!rocket) return HAbandon("spawn");
    g_hRocket.Set(rocket);
    g_flight0 = SRF::GetCounters();
    g_ignite0 = SRI::GetCounters();
    g_captured0 = coop::explosion_sync::GetCounters().captured;
    UE_LOGI("[FIRE-IGNITE] host spawned arm=%s key='%ls'", ArmName(),
            ue_wrap::prop::GetInteractableKeyString(rocket).c_str());
    HNext(ArmOf() == Arm::Host ? HStep::Settle : HStep::Watch);
}

// Arm host: the rocket at rest, the host's own light, and the fuse it must have started.
void HostSettle() {
    void* rocket = g_hRocket.Get();
    ue_wrap::FVector at{};
    const Rested r = PollRest(g_hSettle, rocket, at);
    if (r == Rested::Gone) return HAbandon("settle");
    if (r == Rested::Working) {
        if (Now() - g_hStepMs > kSettleBoundMs) HAbandon("settle");
        return;
    }
    if (!ue_wrap::prop::CallAttemptIgnite(rocket)) return HAbandon("light");
    g_hLitMs = Now();
    UE_LOGI("[FIRE-IGNITE] host lit");
    if (Delta(SRF::GetCounters().joined, g_flight0.joined) != 1) return HFail("the light started no fuse");
    HNext(HStep::Watch);
}

void HostWatch() {
    const SRI::Counters ign = SRI::GetCounters();
    const SRF::Counters fl = SRF::GetCounters();
    for (int r = 0; r < static_cast<int>(SRI::Refusal::Count); ++r)
        if (ign.refused[r] > g_ignite0.refused[r])
            return HFail("refused %s", SRI::RefusalName(static_cast<SRI::Refusal>(r)));
    const uint32_t joined = Delta(fl.joined, g_flight0.joined);
    const uint32_t perf0 = Delta(ign.performed[0], g_ignite0.performed[0]);
    const uint32_t perf1 = Delta(ign.performed[1], g_ignite0.performed[1]);
    if (perf0 > 0 && joined == 0) return HFail("performed, no fuse");
    if (!g_hLitMs && perf0 + perf1 > 0) g_hLitMs = Now();   // arm client: the first light the host performed
    const uint32_t captured = Delta(coop::explosion_sync::GetCounters().captured, g_captured0);
    if (!g_hRocket.Get()) {   // the capture is synchronous with explode(), before the rocket's destroy
        if (joined < 1 || captured < 1) return HFail("the rocket died with fires=%u captured=%u", joined, captured);
        UE_LOGI("[FIRE-IGNITE] host DONE arm=%s fires=%u captured=%u performed=%u/%u lifeMs=%llu -- PASS", ArmName(),
                joined, captured, perf0, perf1,
                static_cast<unsigned long long>(g_hLitMs ? Now() - g_hLitMs : 0));
        g_h = HStep::Done;
        return;
    }
    if (g_hLitMs && Now() - g_hLitMs > kLifeBoundMs) HFail("the rocket never died");
}

void HostTick(coop::net::Session* s) {
    switch (g_h) {
    case HStep::Arm:    return HostArm(s);
    case HStep::Settle: return HostSettle();
    case HStep::Watch:  return HostWatch();
    case HStep::Done:   return;
    }
}

// ===================================================================================================================
// CLIENT
// ===================================================================================================================

enum class CStep : uint8_t { Arm, Find, Settle, Take, Hold, Aim, Watch, Done };
CStep                              g_c = CStep::Arm;
uint64_t                           g_cStepMs = 0, g_cNextMs = 0, g_cRestMs = 0, g_cSentMs = 0;
std::vector<ue_wrap::CachedObjRef> g_start;   // the rockets this world held at its ready edge
ue_wrap::CachedObjRef              g_cRocket;
Settle                             g_cSettle;
ue_wrap::FVector                   g_rest{};
float                              g_movedCm = 0.f;
bool                               g_sentSaid = false;
coop::director::AimFan             g_fan;
SRF::Counters                      g_cFlight0{};
SRI::Counters                      g_cIgnite0{};

void CNext(CStep s) { g_c = s; g_cStepMs = Now(); }

void CFail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    FailLine("client", fmt, ap);
    va_end(ap);
    g_c = CStep::Done;
}
void CAbandon(const char* what) {
    UE_LOGW("[FIRE-IGNITE] ABANDONED client %s", what);
    g_c = CStep::Done;
}

struct Seen {
    const std::vector<ue_wrap::CachedObjRef>* start;
    void* found;
    int   outside;
};

// The live rockets outside the start set (the class default skipped): the drill's is the one.
void CountOutside(Seen& seen) {
    void* cls = ue_wrap::object_index::ClassByName(kRocketCls);
    if (!cls) return;
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* sn = static_cast<Seen*>(c);
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        for (const ue_wrap::CachedObjRef& old : *sn->start)
            if (old.Is(obj)) return;
        sn->found = obj;
        ++sn->outside;
    }, &seen);
}

void ClientArm() {
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    g_start.clear();
    if (void* cls = ue_wrap::object_index::ClassByName(kRocketCls)) {
        ue_wrap::object_index::ForEachInstance(cls, [](void*, void* obj, int32_t) {
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            ue_wrap::CachedObjRef ref;
            ref.Set(obj);
            g_start.push_back(ref);
        }, nullptr);
    }
    g_cFlight0 = SRF::GetCounters();
    g_cIgnite0 = SRI::GetCounters();
    DC::Say(kClientReadyLine);
    CNext(CStep::Find);
}

void ClientFind() {
    const uint64_t now = Now();
    if (now < g_cNextMs) return;
    g_cNextMs = now + kCheckEveryMs;
    Seen seen{&g_start, nullptr, 0};
    CountOutside(seen);
    if (seen.outside > 1) return CAbandon("two rockets");
    if (seen.outside == 0) {
        if (now - g_cStepMs > kFindBoundMs) CAbandon("no rocket");
        return;
    }
    g_cRocket.Set(seen.found);
    g_cSettle = Settle{};
    UE_LOGI("[FIRE-IGNITE] client: the rocket's mirror is here");
    CNext(CStep::Settle);
}

void ClientSettle() {
    ue_wrap::FVector at{};
    const Rested r = PollRest(g_cSettle, g_cRocket.Get(), at);
    if (r == Rested::Gone) return CAbandon("settle");
    if (r == Rested::Working) {
        if (Now() - g_cStepMs > kSettleBoundMs) CAbandon("settle");
        return;
    }
    g_rest = at;
    g_cRestMs = Now();
    CNext(ArmOf() == Arm::Client ? CStep::Take : CStep::Watch);
}

// The lighter's `uses`, -1 unread (the exported helpers read bools and floats only).
int LighterUses(void* lighter) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(lighter), L"uses");
    if (off < 0) return -1;
    int32_t v = 0;
    std::memcpy(&v, static_cast<const char*>(lighter) + off, sizeof(v));
    return v;
}

// The lighter's own left-click verb, called as the player's hand would call it.
bool CallLighterLmb(void* lighter, void* player) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(lighter), L"playerHandUse_LMB");
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(lighter, f);
}

void ClientTake() {
    if (!XD::ResolveClass(kLighterCls, kLighterPath)) return CAbandon("lighter class");
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"lighter", kLighterCls)) return CAbandon("take");
    CNext(CStep::Hold);
}

// The lighter in the hand, its lid opened and its uses set: a lit lighter needs both, and the written values are the
// ones the left click then reads.
void ClientHold(void* player) {
    void* lighter = XD::HeldOf(player, kLighterCls);
    if (!lighter) {
        if (Now() - g_cStepMs > kHoldBoundMs) CAbandon("hold");
        return;
    }
    XD::WriteBool(lighter, L"opened", true);
    XD::WriteInt(lighter, L"uses", 100);
    bool opened = false;
    if (!XD::ReadBool(lighter, L"opened", opened) || !opened || LighterUses(lighter) <= 0)
        return CAbandon("lighter");
    g_fan = coop::director::AimFan();
    CNext(CStep::Aim);
}

// The trace onto the rocket, then the lighter's left click; the light must have been taken as the player's own.
void ClientAim(void* player) {
    void* rocket = g_cRocket.Get();
    ue_wrap::FVector at{};
    if (!rocket || !E::TryGetActorLocation(rocket, at)) return CAbandon("aim");
    const bool aimed = E::ReadMainPlayerHitActor(player) == rocket;
    const auto st = g_fan.Tick(player, at, aimed);
    if (st == coop::director::AimFan::State::Failed) return CAbandon("aim");
    if (st != coop::director::AimFan::State::Aimed) return;
    void* lighter = XD::HeldOf(player, kLighterCls);
    if (!lighter) return CAbandon("hold");
    if (!CallLighterLmb(lighter, player)) return CAbandon("lighter");
    UE_LOGI("[FIRE-IGNITE] client lit");
    if (Delta(SRI::GetCounters().playerRefused, g_cIgnite0.playerRefused) < 1)
        return CFail("the lighter's light was not taken as the player's");
    CNext(CStep::Watch);
}

void ClientWatch() {
    const SRI::Counters ign = SRI::GetCounters();
    const uint32_t fires = Delta(SRF::GetCounters().clientFires, g_cFlight0.clientFires);
    const uint32_t sent = Delta(ign.sent, g_cIgnite0.sent);
    if (ArmOf() == Arm::Client) {
        if (Delta(ign.notAnnounced, g_cIgnite0.notAnnounced) + Delta(ign.pendingDead, g_cIgnite0.pendingDead) +
                Delta(ign.unspawned, g_cIgnite0.unspawned) > 0)
            return CFail("the light was dropped");
        if (sent >= 1 && !g_sentSaid) {
            g_sentSaid = true;
            g_cSentMs = Now();
            UE_LOGI("[FIRE-IGNITE] client sent");
        }
    }
    void* rocket = g_cRocket.Get();
    if (rocket) {
        ue_wrap::FVector at{};
        if (E::TryGetActorLocation(rocket, at)) {
            const float dx = at.X - g_rest.X, dy = at.Y - g_rest.Y, dz = at.Z - g_rest.Z;
            const float cm = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (cm > g_movedCm) g_movedCm = cm;
        }
        const uint64_t since = ArmOf() == Arm::Client ? g_cSentMs : g_cRestMs;
        if (since && Now() - since > kLifeBoundMs) CFail("the rocket never blew moved=%.0f", g_movedCm);
        return;
    }
    // Its destroy: the host's blast reached this copy.
    const bool ok = fires == 0 && (ArmOf() == Arm::Client ? sent >= 1 : g_movedCm >= kFlownCm);
    if (!ok) return CFail("moved=%.0f fires=%u sent=%u", g_movedCm, fires, sent);
    UE_LOGI("[FIRE-IGNITE] client DONE arm=%s moved=%.0f fires=%u sent=%u -- PASS", ArmName(), g_movedCm, fires, sent);
    g_c = CStep::Done;
}

void ClientTick() {
    if (g_c == CStep::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_c) {
    case CStep::Arm:    return ClientArm();
    case CStep::Find:   return ClientFind();
    case CStep::Settle: return ClientSettle();
    case CStep::Take:   return ClientTake();
    case CStep::Hold:   return ClientHold(player);
    case CStep::Aim:    return ClientAim(player);
    case CStep::Watch:  return ClientWatch();
    case CStep::Done:   return;
    }
}

}  // namespace

void Install(coop::net::Session* s) {
    if (ArmOf() == Arm::Off) return;
    g_session.store(s, std::memory_order_release);
}

void Tick() {
    if (ArmOf() == Arm::Off) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (ArmOf() == Arm::Off) return;
    g_h = HStep::Arm;
    g_hStepMs = g_hLitMs = 0;
    g_hRocket.Reset();
    g_hSettle = Settle{};
    g_flight0 = SRF::Counters{};
    g_ignite0 = SRI::Counters{};
    g_captured0 = 0;
    g_clientReady = false;
    g_c = CStep::Arm;
    g_cStepMs = g_cNextMs = g_cRestMs = g_cSentMs = 0;
    g_start.clear();
    g_cRocket.Reset();
    g_cSettle = Settle{};
    g_rest = {};
    g_movedCm = 0.f;
    g_sentSaid = false;
    g_fan = coop::director::AimFan();
    g_cFlight0 = SRF::Counters{};
    g_cIgnite0 = SRI::Counters{};
}

}  // namespace coop::dev::fire_ignite_drill
