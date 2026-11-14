// harness/autotest/autotest_orb.cpp -- TEST-ONLY: the black orb's encounter is the host's, and a client's copy takes
// its part. A verdict is decided by an event; a bound passing with none is INVALID. Armed by the row orb_drill on both
// peers (VOTVCOOP_CAN_DIR). The host mints each orb on the client's puppet, whose body touches it
// where it stands, and the client holds its view with the control rotation, which streams to the host as its aim.
//   1     The client first walks 30 m from the host (both start at one spot, and the host's own body would touch the orb).
//         Round 1, the client's view away from the orb: the host's verdict must be spared and no Death sent. The
//         client's copy must have run the host's Begin by the time the host's destroy reaches it (the destroy follows
//         the Begin on the same ordered lane: the fence), else FAIL there. Round 2, the view toward the orb: the
//         host's verdict must be caught, a Death sent, and the client must answer it.
//         `rig: VERDICT orb PASS|FAIL|INVALID`. Red: orb_begin_send_off (host).
// A joiner during an encounter is owed nothing (protocol.h, OrbEvent), so there is no join arm.

#include "harness/autotest.h"
#include "disc_drills.h"      // co-located private headers (src tree, not include/)
#include "gt_wait.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/orb_authority.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace PS = harness::autotest::peer_signal;
namespace RV = coop::rig_verdict;
namespace OA = coop::orb_authority;
using disc_drill::OnGameThread;

constexpr const char* kTag = "orb";
constexpr int kStepSec = 120;
constexpr int kEncounterSec = 60;   // INVALID bound: the encounter's ~5 s and its verdict reach the peers
constexpr float kAwayCm = 3000.f;   // the client's distance from the host: past the orb's overlap sphere

// The live orb copy this machine holds, a host-minted one with an element id.
void* FindOrb() {
    struct Ctx { void* found = nullptr; } ctx;
    if (void* cls = ue_wrap::object_index::ClassByName(L"npc_orborb_C"))
        ue_wrap::object_index::ForEachInstance(cls, [](void* p, void* obj, int32_t) {
            auto& x = *static_cast<Ctx*>(p);
            if (x.found || !obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            if (EL::Registry::Get().EidForActor(obj) != EL::kInvalidId) x.found = obj;
        }, &ctx);
    return ctx.found;
}

// CLIENT: face the orb's copy, or turn the back on it, through the control rotation, which the host reads as this player's
// aim. Without a copy yet the view looks straight down (toward the orb at this player's feet) or up (away from it).
void HoldView(bool toward) {
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    ue_wrap::FRotator rot{toward ? -85.f : 85.f, 0.f, 0.f};
    ue_wrap::FVector at{};
    if (void* const orb = FindOrb(); orb && E::TryGetActorLocation(orb, at)) {
        const ue_wrap::FVector cam = E::GetCameraLocation();
        const float dx = at.X - cam.X, dy = at.Y - cam.Y, dz = at.Z - cam.Z;
        const float flat = std::sqrt(dx * dx + dy * dy);
        const float yaw = std::atan2(dy, dx) * 57.2958f;
        const float pitch = std::atan2(dz, flat) * 57.2958f;
        rot = toward ? ue_wrap::FRotator{pitch, yaw, 0.f} : ue_wrap::FRotator{-pitch, yaw + 180.f, 0.f};
    }
    E::SetControlRotation(E::GetController(me), rot);
}

// HOST: an orb on the client's puppet. False when the puppet or the mint is not there.
bool MintOnClient() {
    bool minted = false;
    OnGameThread([&] {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
        ue_wrap::FVector at{};
        if (rp && rp->valid() && E::TryGetActorLocation(rp->GetActor(), at)) minted = OA::DevMintOrbAt({at.X, at.Y, at.Z + 20.f});
    });
    return minted;
}

OA::Counters Counts() {
    OA::Counters c;
    OnGameThread([&] { c = OA::Read(); });
    return c;
}

// ---- the host ----

// One round: the client is told its view, says it holds it, the host mints and waits for its own verdict.
bool HostRound(int n, bool toward) {
    const std::string tag = std::to_string(n);
    PS::Mark(("view_" + tag).c_str(), toward ? "toward" : "away");
    if (!PS::Await(("holding_" + tag).c_str(), kStepSec)) return false;
    const OA::Counters before = Counts();
    if (!MintOnClient()) {
        RV::Say(kTag, RV::Outcome::Invalid, "round %d: no orb could be minted on the client's puppet", n);
        return false;
    }
    if (!WaitGT("the host's verdict on the orb", kEncounterSec, [&] { return OA::Read().caught + OA::Read().spared > before.caught + before.spared; }))
        return false;
    const OA::Counters after = Counts();
    if (after.lastToucher != 1) {
        RV::Say(kTag, RV::Outcome::Invalid, "round %d: the orb's encounter was begun by slot %u, not by the client (slot 1)", n,
                static_cast<unsigned>(after.lastToucher));
        return false;
    }
    UE_LOGI("orb-drill: HOST round %d (view %hs): caught +%u, spared +%u, deaths sent +%u", n, toward ? "toward" : "away",
            after.caught - before.caught, after.spared - before.spared, after.deathsSent - before.deathsSent);
    const bool right = toward ? (after.caught == before.caught + 1 && after.deathsSent == before.deathsSent + 1)
                              : (after.spared == before.spared + 1 && after.deathsSent == before.deathsSent);
    if (!right) {
        RV::Say(kTag, RV::Outcome::Fail, "round %d: the client's view %s the orb and the host's verdict was %s (deaths sent +%u)", n,
                toward ? "toward" : "away from", after.spared > before.spared ? "spared" : "caught", after.deathsSent - before.deathsSent);
        PS::Mark(("end_" + tag).c_str(), "F");
        return false;
    }
    PS::Mark(("end_" + tag).c_str(), "P");
    return true;
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    if (!WaitGT("the client's slot world-ready", 120, [] { return harness::session_runtime::Session().IsSlotWorldReady(1); })) return;
    if (!WaitGT("the client's puppet", 60, [] {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
            return rp && rp->valid();
        }))
        return;
    // Both players start at one spot, and the host's own body would touch an orb minted there: the client walks away first.
    ue_wrap::FVector hostAt{};
    OnGameThread([&] { E::TryGetActorLocation(coop::players::Registry::Get().Local(), hostAt); });
    char at[96];
    std::snprintf(at, sizeof(at), "%.1f %.1f %.1f", hostAt.X, hostAt.Y, hostAt.Z);
    PS::Mark("host_at", at);
    if (!PS::Await("client_away", 480)) return;
    if (!HostRound(1, false) || !HostRound(2, true)) return;
    std::string done;
    if (!PS::AwaitText("client_done", done, kStepSec)) return;
    if (done[0] != 'P') return;   // the client said its FAIL where it was decided
    const OA::Counters c = Counts();
    RV::Say(kTag, RV::Outcome::Pass, "spared with the view away, caught with it toward; the host sent %u begin(s) and %u death(s)",
            c.beginsSent, c.deathsSent);
    UE_LOGI("orb-drill: DONE on host");
}

// ---- the client ----

// Hold the view every poll until `done` holds on the game thread.
template <class Done>
bool HoldUntil(const char* what, bool toward, Done done) {
    return WaitGT(what, kEncounterSec, [&] {
        HoldView(toward);
        return done();
    });
}

bool ClientBody() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return false;
    PS::Mark("client_ready");

    // Both players start at one spot: this client walks 30 m from the host, so the host's own body never touches an orb
    // minted on this client's puppet. The route is the director's, to a navmesh point.
    std::string hostText;
    if (!PS::AwaitText("host_at", hostText, kStepSec)) return false;
    ue_wrap::FVector host{};
    if (sscanf_s(hostText.c_str(), "%f %f %f", &host.X, &host.Y, &host.Z) != 3) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host's place '%s' does not parse", hostText.c_str());
        return false;
    }
    ue_wrap::FVector goal{};
    bool found = false;
    OnGameThread([&] {
        void* const me = coop::players::Registry::Get().Local();
        const float dir[4][2] = {{1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}, {0.f, -1.f}};
        for (const auto& d : dir)
            if (!found && E::ProjectToNav(me, {host.X + d[0] * kAwayCm, host.Y + d[1] * kAwayCm, host.Z}, {1000.f, 1000.f, 20000.f}, &goal))
                found = true;
    });
    if (!found) {
        RV::Say(kTag, RV::Outcome::Invalid, "no navmesh point %.0f cm from the host to walk this client to", kAwayCm);
        return false;
    }
    if (!meet::WalkToPoint(kTag, goal, 300.f, "away from the host")) return false;
    PS::Mark("client_away");

    // Round 1: the view away. The copy must be seen, and its begin must have run when the host's destroy takes it.
    std::string view;
    if (!PS::AwaitText("view_1", view, kStepSec)) return false;
    PS::Mark("holding_1");
    bool seen = false;
    if (!HoldUntil("the orb copy, then its end", false, [&] {
            if (FindOrb()) seen = true;
            return seen && !FindOrb();
        }))
        return false;
    const OA::Counters c = Counts();
    if (c.beginsRan == 0) {
        RV::Say(kTag, RV::Outcome::Fail, "the host's destroy reached the orb copy and its begin never ran here (ran=%u, dropped=%u)",
                c.beginsRan, c.beginsDropped);
        PS::Mark("client_done", "F");
        return false;
    }
    UE_LOGI("orb-drill: CLIENT round 1: the copy's begin ran=%u dropped=%u by the host's destroy", c.beginsRan, c.beginsDropped);
    std::string end;
    if (!PS::AwaitText("end_1", end, kStepSec) || end[0] != 'P') return false;

    // Round 2: the view toward. The host's Death must arrive and be answered.
    if (!PS::AwaitText("view_2", view, kStepSec)) return false;
    PS::Mark("holding_2");
    if (!HoldUntil("the host's Death", true, [] { return OA::Read().deathsAnswered >= 1; })) return false;
    PS::Mark("client_done", "P");
    UE_LOGI("orb-drill: DONE on client");
    return true;
}

}  // namespace

DWORD WINAPI OrbDrillThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (IsClientRole()) ClientBody();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
