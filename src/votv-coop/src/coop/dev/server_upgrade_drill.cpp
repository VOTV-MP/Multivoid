// coop/dev/server_upgrade_drill.cpp -- see coop/dev/server_upgrade_drill.h.

#include "coop/dev/server_upgrade_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/standpoints.h"
#include "coop/interactables/server_upgrade_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::server_upgrade_drill {
namespace {

namespace SB = ue_wrap::serverbox;
namespace SU = coop::server_upgrade_sync;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;

constexpr float    kReachCm       = 150.f;  // the director's stop, within the use key's reach
constexpr int      kWalkDeadlineS = 300;    // the director's own bound on one walk
constexpr uint64_t kAckBoundMs    = 30000;  // an op's canonical is a round trip away
constexpr uint64_t kHoldBoundMs   = 5000;   // the pickup and the hand's actor settle within a few ticks
constexpr float    kBayReachCm    = 180.f;  // a route must end this near the box's upgrade bay, flat
constexpr size_t   kMaxCandidates = 12;     // the nearest boxes, by the flat distance, asked for a route
constexpr float    kRefundCm      = 400.f;  // a refund lands at its author's body: an upgrade this near is one

enum class Step : uint8_t { Arm, Walk, Hold, Install, InstallAck, Aim, TakeOut, TakeOutAck, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
int      g_session = 1;  // this process's sessions, counted by their ends
ue_wrap::CachedObjRef g_box;
int32_t  g_boxIdx = -1;
int32_t  g_level0 = 0;
uint64_t g_adoptedAt = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
coop::director::AimFan g_aim;
bool     g_hostDone = false;
bool     g_hostArmed = false;
int      g_nearBefore = 0;  // upgrades near the player, not in its hand, before the install (refuse mode)

const std::string& Mode() {
    static const std::string s = coop::config::ResolveString(::coop::config_registry::rows::server_upgrade_drill);
    return s;
}
bool Enabled() { return Mode() == "apply" || Mode() == "refuse"; }
bool Refuse() { return Mode() == "refuse"; }

void Go(Step s) {
    g_step = s;
    g_stepMs = ::GetTickCount64();
}

bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }

void Abandon(const char* why) {
    UE_LOGW("[SRV-UPG-DRILL] ABANDONED on the client in session %d: %s", g_session, why);
    g_step = Step::Done;
}

void Fail(const char* why) {
    UE_LOGW("[SRV-UPG-DRILL] FAIL in session %d: %s", g_session, why);
    g_step = Step::Done;
}

void* HeldActor(void* player) {
    E::MainPlayerGrabState gs{};
    return (player && E::ReadMainPlayerGrabState(player, gs)) ? gs.holdingActor : nullptr;
}

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// The upgrades within reach of the player, its hand's own left out.
int UpgradesNear(void* player) {
    struct Ctx { ue_wrap::FVector at; void* held; int n; } ctx{{}, HeldActor(player), 0};
    if (!E::TryGetActorLocation(player, ctx.at)) return 0;
    SB::ForEachUpgrade([](void* c, void* u) {
        auto* x = static_cast<Ctx*>(c);
        ue_wrap::FVector p{};
        if (u == x->held || !E::TryGetActorLocation(u, p)) return;
        const float dx = p.X - x->at.X, dy = p.Y - x->at.Y, dz = p.Z - x->at.Z;
        if (dx * dx + dy * dy + dz * dz <= kRefundCm * kRefundCm) ++x->n;
    }, &ctx);
    return ctx.n;
}

// The box that can take an upgrade whose upgrade bay the shortest NavMesh route reaches: a route that
// ends short of the bay, or none at all, leaves the director grinding against a wall. Picked as the
// director picks a pile, by the route, never by the straight line.
bool PickBox(void* player, ue_wrap::FVector& bayOut) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(player, at)) return false;
    std::vector<void*> servers;
    SB::ReadServers(servers);
    struct Cand { size_t idx; int32_t lvl; ue_wrap::FVector bay; float flat; };
    std::vector<Cand> cands;
    for (size_t i = 0; i < servers.size(); ++i) {
        int32_t lvl = 0;
        void* comp = servers[i] ? SB::TakeOutComponent(servers[i]) : nullptr;
        if (!comp || !SB::ReadUpgrades(servers[i], lvl) || lvl >= SB::kMaxUpgrades) continue;
        if (Refuse() && lvl < 1) continue;  // the refused take-out needs one to take
        const ue_wrap::FVector bay = E::GetComponentLocation(comp);
        cands.push_back({i, lvl, bay, Flat(bay, at)});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.flat < b.flat; });
    if (cands.size() > kMaxCandidates) cands.resize(kMaxCandidates);
    float bestLen = 1e30f;
    for (const Cand& c : cands) {
        float len = 0.f;
        if (!coop::director::RouteInLegs(player, at, c.bay, kBayReachCm, &len) || len >= bestLen) continue;
        bestLen = len;
        g_boxIdx = static_cast<int32_t>(c.idx);
        g_level0 = c.lvl;
        g_box.Set(servers[c.idx]);
        bayOut = c.bay;
    }
    return g_boxIdx >= 0;
}

// Whether a canonical from the host arrived since the op and shows the box at `level`.
bool Acked(void* box, int32_t level) {
    int32_t cur = -1;
    return SU::CanonicalsAdopted() > g_adoptedAt && SB::ReadUpgrades(box, cur) && cur == level;
}

void ClientTick(void* player) {
    switch (g_step) {
    case Step::Arm: {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        ue_wrap::FVector bay{};
        if (!PickBox(player, bay)) {
            Abandon("no server box that can take an upgrade has an upgrade bay a NavMesh route reaches");
            return;
        }
        UE_LOGI("[SRV-UPG-DRILL] client: box %d, its upgrade bay at (%.0f, %.0f, %.0f), holds %d upgrade(s); walking "
                "to it", g_boxIdx, bay.X, bay.Y, bay.Z, g_level0);
        g_walk = coop::director::StartBackgroundWalk(bay, kReachCm, kWalkDeadlineS);
        Go(Step::Walk);
        return;
    }
    case Step::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        if (st == 2) { Abandon("the walk to the box did not arrive"); return; }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(player, at)) { Abandon("the player's place is unread"); return; }
        void* prop = SB::SpawnUpgradeProp({at.X, at.Y, at.Z + 60.f});
        bool collected = false;
        if (!prop || !E::CallMainPlayerHoldObject(player, prop, collected) || !collected) {
            Abandon("an upgrade could not be spawned and taken into the hand");
            return;
        }
        Go(Step::Hold);
        return;
    }
    case Step::Hold: {
        void* held = HeldActor(player);
        if (!held || !SB::IsUpgradeClass(R::ClassOf(held))) {
            if (Expired(kHoldBoundMs)) Abandon("the hand does not hold the upgrade after the pickup");
            return;
        }
        Go(Step::Install);
        return;
    }
    case Step::Install: {
        void* box = g_box.Get();
        void* held = HeldActor(player);
        if (!box) { Abandon("the box died"); return; }
        // Read again: a canonical may have moved the box during the walk.
        if (!SB::ReadUpgrades(box, g_level0) || g_level0 >= SB::kMaxUpgrades || (Refuse() && g_level0 < 1)) {
            Abandon("the box can no longer take this drill's install");
            return;
        }
        g_nearBefore = UpgradesNear(player);
        g_adoptedAt = SU::CanonicalsAdopted();
        if (!SB::CallInstall(box, player, held, ue_wrap::fname_utils::StringToFName(L"serverUpg_1"))) {
            Abandon("the box's install did not run");
            return;
        }
        int32_t lvl = -1;
        if (!SB::ReadUpgrades(box, lvl) || lvl != g_level0 + 1) {
            Abandon("the install ran but the box's own count did not move");
            return;
        }
        UE_LOGI("[SRV-UPG-DRILL] client: installed into box %d, %d -> %d on this copy", g_boxIdx, g_level0, lvl);
        Go(Step::InstallAck);
        return;
    }
    case Step::InstallAck: {
        void* box = g_box.Get();
        if (!box) { Abandon("the box died"); return; }
        const int32_t want = Refuse() ? g_level0 : g_level0 + 1;
        if (!Acked(box, want) || (Refuse() && UpgradesNear(player) <= g_nearBefore)) {
            if (Expired(kAckBoundMs))
                Fail(Refuse() ? "no canonical put the box back after the refused install, or no refund came, within 30 s"
                              : "no canonical from the host showed the install within 30 s");
            return;
        }
        UE_LOGI("[SRV-UPG-DRILL] client: the host's canonical shows box %d at %d%s", g_boxIdx, want,
                Refuse() ? ", and the refused install's refund came" : "");
        g_level0 = want;
        g_aim = coop::director::AimFan{};
        Go(Step::Aim);
        return;
    }
    case Step::Aim: {
        void* box = g_box.Get();
        void* comp = box ? SB::TakeOutComponent(box) : nullptr;
        if (!comp) { Abandon("the box's take-out is unread"); return; }
        bool looks = false;
        SB::ReadLooksAtTakeOut(box, looks);
        switch (g_aim.Tick(player, E::GetComponentLocation(comp), looks)) {
        case coop::director::AimFan::State::Working: return;
        case coop::director::AimFan::State::Failed:
            Abandon("no pose of the fan put the box's own look on its take-out");
            return;
        case coop::director::AimFan::State::Aimed: break;
        }
        UE_LOGI("[SRV-UPG-DRILL] client: the box reads the player on its take-out at fan pose %d", g_aim.Poses());
        Go(Step::TakeOut);
        return;
    }
    case Step::TakeOut: {
        void* box = g_box.Get();
        if (!box) { Abandon("the box died"); return; }
        g_adoptedAt = SU::CanonicalsAdopted();
        if (!SB::CallTakeOut(box, player)) { Abandon("the box's take-out did not run"); return; }
        int32_t lvl = -1;
        if (!SB::ReadUpgrades(box, lvl) || lvl != g_level0 - 1) {
            Abandon("the take-out ran but the box's own count did not move");
            return;
        }
        void* held = HeldActor(player);
        UE_LOGI("[SRV-UPG-DRILL] client: took an upgrade out of box %d, %d -> %d on this copy; the hand holds %s",
                g_boxIdx, g_level0, lvl, held && SB::IsUpgradeClass(R::ClassOf(held)) ? "an upgrade" : "no upgrade");
        Go(Step::TakeOutAck);
        return;
    }
    case Step::TakeOutAck: {
        void* box = g_box.Get();
        if (!box) { Abandon("the box died"); return; }
        // Applied, the take-out leaves the box one lower; refused, the canonical puts it back and the upgrade the
        // take-out handed over leaves the hand.
        const int32_t want = Refuse() ? g_level0 : g_level0 - 1;
        void* held = HeldActor(player);
        const bool handed = held && SB::IsUpgradeClass(R::ClassOf(held));
        if (!Acked(box, want) || (Refuse() && handed)) {
            if (Expired(kAckBoundMs))
                Fail(Refuse() ? "no canonical put the box back after the refused take-out, or its upgrade stayed in "
                                "the hand, within 30 s"
                              : "no canonical from the host showed the take-out within 30 s");
            return;
        }
        UE_LOGI("[SRV-UPG-DRILL] client DONE in session %d (%s): box %d at %d, the host's canonical followed both "
                "ops%s -- PASS", g_session, Mode().c_str(), g_boxIdx, want,
                Refuse() ? ", the install refunded and the take-out's upgrade gone from the hand" : "");
        g_step = Step::Done;
        return;
    }
    case Step::Done:
        return;
    }
}

void HostTick() {
    if (!g_hostArmed) {
        g_hostArmed = true;
        SU::DebugRefuseOps(Refuse());
    }
    if (g_hostDone) return;
    const SU::HostCounts c = SU::HostOpCounts();
    if (Refuse() ? c.refused < 2 : (c.installs < 1 || c.takeOuts < 1)) return;
    g_hostDone = true;
    UE_LOGI("[SRV-UPG-DRILL] host DONE in session %d: applied %llu install(s) and %llu take-out(s) from the client, "
            "%llu refused", g_session, static_cast<unsigned long long>(c.installs),
            static_cast<unsigned long long>(c.takeOuts), static_cast<unsigned long long>(c.refused));
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) { HostTick(); return; }
    if (g_step == Step::Done) return;
    if (void* player = coop::players::Registry::Get().Local()) ClientTick(player);
}

void OnDisconnect() {
    ++g_session;
    g_step = Step::Arm;
    g_box.Reset();
    g_boxIdx = -1;
    g_level0 = 0;
    g_adoptedAt = 0;
    g_walk.reset();
    g_aim = coop::director::AimFan{};
    g_hostDone = false;
    g_hostArmed = false;
    g_nearBefore = 0;
}

}  // namespace coop::dev::server_upgrade_drill
