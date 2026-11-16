// harness/autotest/autotest_chipper_feed.cpp -- TEST-ONLY: a client feeds a running woodchipper a prop it still drives.
//
// The host walks to a measured level square and sets a chipper on it, runs it and lays a paper beside it; the client's
// director walks to the paper, grabs it and walks to a stand within reach of the chipper's eat box, where it writes the held
// paper's pose into the box's slot while holding it (VOTVCOOP_CHIPPER_FEED_ARM=held, the default), or over the slot, to be
// thrown down into it by the game's own throw (thrown). The host's copy is then its client's, out of the simulation,
// which the chipper's own canChip refuses: the host's gate (coop/interactables/woodchipper_gate) sets that copy simulating
// for the chipper's resume, so the chipper eats it as it eats any prop. The host says `rig: VERDICT chipper-feed
// PASS|FAIL|INVALID`, each decided by the gate's record of the resume that read the paper: PASS when a client drove the
// copy, the gate flipped it and the chipper took it; FAIL when a client drove the copy on a chipper that was on, idle and
// fuelled and the chipper did not take it; INVALID when the copy was not driven or the chipper not ready at the resume,
// the carry never reached the box, or a bound runs out. The red is the host's chipper_driven_flip_off: nothing is
// flipped, the resume refuses the copy (simulating=0), and the verdict is FAIL at that resume. VOTVCOOP_CHIPPER_FEED_MANUAL=1
// stops once the client holds the paper for a person to put it in the cone (up to 10 minutes). Gated by
// VOTVCOOP_RUN_CHIPPER_FEED; the peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "chipper_drill_common.h"  // co-located private header
#include "level_ground.h"          // co-located private header: level open ground round the host

#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/interactables/woodchipper_gate.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/remote_prop.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

using namespace chipper_drill;

// VOTVCOOP_CHIPPER_FEED_MANUAL=1: the drill stops once the client holds the paper, and a person playing the client puts
// it in the chipper's cone; both peers then wait up to 10 minutes for it to go.
bool Manual() { return coop::config::ReadEnv("VOTVCOOP_CHIPPER_FEED_MANUAL") == "1"; }
constexpr int kManualSec = 600;

// VOTVCOOP_CHIPPER_FEED_ARM: held (the paper is placed in the box and held there) or thrown (thrown down into the slot).
bool Thrown() { return coop::config::ReadEnv("VOTVCOOP_CHIPPER_FEED_ARM") == "thrown"; }

// HOST: the verdict, decided at the chipper's resume of the paper, by the gate's record of it. A bound says INVALID only.
void JudgeFlip(const std::wstring& chipKey, const std::wstring& paperKey, bool manual) {
    using Clk = std::chrono::steady_clock;
    const Clk::time_point t0 = Clk::now();
    Clk::time_point fedAt{};
    bool fedSeen = false, inboxSeen = false;
    Clk::time_point inboxAt{}, lastPathAt{};
    int pathLines = 0;
    bool arrived = false;
    std::string fed;
    for (;;) {
        coop::woodchipper_gate::Intake in{};
        bool have = false, alive = false;
        OnGameThread([&] {
            have = coop::woodchipper_gate::IntakeOf(paperKey, in);
            alive = ByKey(paperKey) != nullptr;
        });
        using coop::rig_verdict::Outcome;
        if (have) {
            const std::string k = Narrow(paperKey);
            if (in.slot < 0)
                coop::rig_verdict::Say(kTag, Outcome::Invalid,
                                       "no client drove the host's copy of %s when the chipper's resume read it (simulating=%d "
                                       "took=%d): the case the flip is for did not arise", k.c_str(), in.simulating ? 1 : 0,
                                       in.took ? 1 : 0);
            else if (!in.ready)
                coop::rig_verdict::Say(kTag, Outcome::Invalid,
                                       "the chipper was not on, idle and fuelled at its resume of %s (slot %d drove the copy)",
                                       k.c_str(), in.slot);
            else if (in.simulating)
                coop::rig_verdict::Say(kTag, Outcome::Invalid,
                                       "the host's copy of %s already simulated at the resume (slot %d drove it): nothing to flip",
                                       k.c_str(), in.slot);
            else if (!in.took)
                coop::rig_verdict::Say(kTag, Outcome::Fail,
                                       "the chipper did not take %s at its resume: slot %d drove the copy, simulating=0, "
                                       "flipped=%d, the chipper on, idle and fuelled", k.c_str(), in.slot, in.flipped ? 1 : 0);
            else if (!in.flipped)
                coop::rig_verdict::Say(kTag, Outcome::Invalid,
                                       "the chipper took %s with no flip (slot %d): a take the gate did not make", k.c_str(),
                                       in.slot);
            else
                coop::rig_verdict::Say(kTag, Outcome::Pass,
                                       "the host's chipper took %s: slot %d drove its copy, which the gate set simulating for "
                                       "the resume", k.c_str(), in.slot);
            return;
        }
        std::string said;
        if (!inboxSeen && PS::Peek("inbox", said)) {
            inboxSeen = true;
            inboxAt = Clk::now();
            lastPathAt = inboxAt;
            UE_LOGI("chipper-feed: HOST the client reports: %s; no resume of the chipper has read %s yet", said.c_str(),
                    Narrow(paperKey).c_str());
        }
        // The client has put its paper at the slot and the chipper has not read it: where the host's copy is against the host's own
        // box, every 100 ms, said once as it arrives (within 30 cm of the box's centre) and at most 40 times in all (the first
        // four seconds, which hold a thrown paper's flight). A held copy is driven by a slot's hold; a thrown one is positioned by
        // its syncer's rows, with its simulation off, while the host's thrown lane lasts.
        if (inboxSeen && pathLines < 40 && Clk::now() - lastPathAt >= std::chrono::milliseconds(100)) {
            lastPathAt = Clk::now();
            FVector pos{}, local{}, half{}, boxAt{};
            bool inside = false, read = false, simulating = false;
            int driver = -1, syncer = 0;
            OnGameThread([&] {
                void* p = ByKey(paperKey);
                void* eat = EatOf(ByKey(chipKey));
                if (!p || !eat) return;
                read = E::TryGetActorLocation(p, pos);
                inside = read && PointInEatBox(eat, pos, &local, &half, &boxAt);
                driver = coop::remote_prop::DrivingSlotOf(p);
                const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(p);
                if (eid != coop::element::kInvalidId && eid != 0u) syncer = coop::prop_drive_host::ClientSyncerOf(eid);
                void* mesh = ue_wrap::prop::GetStaticMesh(p);
                simulating = mesh && E::IsComponentSimulatingPhysics(mesh);
            });
            const float away = Dist(pos, boxAt);
            ++pathLines;
            UE_LOGI("chipper-feed: HOST copy of %s at %.0f,%.0f,%.0f is %.0f cm from the host's box centre%s; in the box's axes "
                    "%.0f,%.0f,%.0f of half %.0f,%.0f,%.0f (inside=%d), held-driven by slot %d, synced by slot %d (0 = none), simulating=%d",
                    Narrow(paperKey).c_str(), pos.X, pos.Y, pos.Z, away, (!arrived && away <= 30.f) ? " -- ARRIVED" : "", local.X,
                    local.Y, local.Z, half.X, half.Y, half.Z, inside ? 1 : 0, driver, syncer, simulating ? 1 : 0);
            if (away <= 30.f) arrived = true;
        }
        if (!fedSeen && PS::Peek("fed", fed)) {
            fedSeen = true;
            fedAt = Clk::now();
            if (fed.rfind("held", 0) != 0 && fed.rfind("thrown", 0) != 0) {
                coop::rig_verdict::Say(kTag, Outcome::Invalid, "the client did not carry the paper to the box (%s)", fed.c_str());
                return;
            }
        }
        if (!alive && fedSeen) {
            coop::rig_verdict::Say(kTag, Outcome::Invalid,
                                   "%s is gone and the chipper's resume left no record of it", Narrow(paperKey).c_str());
            return;
        }
        // No resume of the chipper read the paper: the bound is the carry's own, then the chipper's.
        const int boundSec = manual ? kManualSec : (fedSeen || inboxSeen) ? kEatSec : kStepSec * 3;
        if (Clk::now() - (fedSeen ? fedAt : inboxSeen ? inboxAt : t0) > std::chrono::seconds(boundSec)) {
            coop::rig_verdict::Say(kTag, Outcome::Invalid,
                                   "no resume of the host's chipper read %s within %d s (client reported: %s)",
                                   Narrow(paperKey).c_str(), boundSec, fedSeen ? fed.c_str() : inboxSeen ? "held inside the box" : "nothing yet");
            return;
        }
        ::Sleep(100);
    }
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    auto invalid = [&](const char* fmt, auto... args) {
        PS::Mark("set", "none");
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, fmt, args...);
        UE_LOGI("chipper-feed: DONE on host");
    };
    FVector floorAt{};
    if (!level_ground::Find(floorAt, "VOTVCOOP_CHIPPER_FEED_AT")) return invalid("no level open ground round the host");
    // The host walks to the square's -X edge by the director (the route is the NavMesh's, staged when far), then sets the
    // chipper down; a bound or a failed walk says INVALID only.
    if (!WalkTo({floorAt.X - 300.f, floorAt.Y, floorAt.Z}, 60.f, 300, /*carry=*/false))
        return invalid("the director could not walk the host to the level square at %.0f,%.0f,%.0f", floorAt.X, floorAt.Y,
                       floorAt.Z);
    void* chipper = nullptr;
    OnGameThread([&] {
        void* chipCls = ue_wrap::asset_load::LoadObjectByPath(kChipperPath);
        // Upright, a hand's breadth above the ground: it lands on its feet, and its fall is the host's own birth stream, so
        // the client's copy rests where the host's does.
        if (chipCls) chipper = E::SpawnActor(chipCls, {floorAt.X, floorAt.Y, floorAt.Z + 60.f});
    });
    const std::wstring chipKey = chipper ? KeyOfSpawn(chipper) : std::wstring();
    if (chipKey.empty() || !AtRest(chipKey, kStepSec)) return invalid("the chipper could not be spawned and come to rest");
    ue_wrap::FRotator rot{};
    OnGameThread([&] {
        if (void* c = ByKey(chipKey)) E::TryGetActorRotation(c, rot);
    });
    UE_LOGI("chipper-feed: HOST chipper '%ls' stands at pitch %.1f roll %.1f", chipKey.c_str(), rot.Pitch, rot.Roll);
    if (std::fabs(rot.Pitch) > kUprightDeg || std::fabs(rot.Roll) > kUprightDeg)
        return invalid("the chipper does not stand upright on the ground at %.0f,%.0f,%.0f (pitch %.1f roll %.1f)", floorAt.X,
                       floorAt.Y, floorAt.Z, rot.Pitch, rot.Roll);
    void* paper = nullptr;
    OnGameThread([&] {
        void* paperCls = ue_wrap::asset_load::LoadObjectByPath(kPaperPath);
        void* c = ByKey(chipKey);
        void* eat = EatOf(c);
        if (!paperCls || !eat) return;
        const FVector e = E::GetComponentLocation(eat);
        paper = E::SpawnActor(paperCls, {e.X - 150.f, e.Y, e.Z + 30.f});  // on the ground beside it, the host's side
        W::WriteFuel(c, 100.f);
        W::WriteActive(c, true);
        W::CallCheckActive(c);
    });
    const std::wstring paperKey = paper ? KeyOfSpawn(paper) : std::wstring();
    if (paperKey.empty()) return invalid("the paper could not be spawned with a key");
    if (!AtRest(paperKey, kStepSec)) return invalid("the paper did not come to rest");
    UE_LOGI("chipper-feed: HOST chipper '%ls' on with fuel, paper '%ls' beside it", chipKey.c_str(), paperKey.c_str());
    FVector hostBox{};
    OnGameThread([&] {
        if (void* eat = EatOf(ByKey(chipKey))) hostBox = E::GetComponentLocation(eat);
    });
    PS::Mark("set", "chipper=" + Narrow(chipKey) + " paper=" + Narrow(paperKey));
    // The client's copy must stand where the host's does: the two boxes one place.
    std::string box;
    FVector clientBox{};
    if (!PS::AwaitText("client_box", box, kStepSec) ||
        sscanf_s(box.c_str(), "%f,%f,%f", &clientBox.X, &clientBox.Y, &clientBox.Z) != 3 || Dist(hostBox, clientBox) > 30.f) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "the client's chipper copy does not stand where the host's does (its box %s, the host's "
                               "%.0f,%.0f,%.0f)", box.c_str(), hostBox.X, hostBox.Y, hostBox.Z);
        PS::Mark("host_done");
        UE_LOGI("chipper-feed: DONE on host");
        return;
    }
    // The client takes the paper into its grab; the host answers once its copy is driven by that hold.
    std::string grabbed;
    if (!PS::AwaitText("grabbed", grabbed, kStepSec * 3) || grabbed.rfind("none", 0) == 0) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the client could not take the paper into its grab (%s)",
                               grabbed.c_str());
        PS::Mark("host_done");
        UE_LOGI("chipper-feed: DONE on host");
        return;
    }
    if (!PS::WaitFor("the host applies the client's hold of the paper", kStepSec, [&] {
            bool driven = false;
            OnGameThread([&] {
                void* p = ByKey(paperKey);
                driven = p && coop::remote_prop::DrivingSlotOf(p) >= 0;
            });
            return driven;
        })) {
        PS::Mark("host_done");
        UE_LOGI("chipper-feed: DONE on host");
        return;
    }
    PS::Mark("hold_applied");
    JudgeFlip(chipKey, paperKey, Manual());
    PS::Mark("host_done");
    UE_LOGI("chipper-feed: DONE on host");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("client_ready");
    std::string set;
    if (!PS::AwaitText("set", set, kStepSec * 3) || set == "none") return;  // the host walks to the square first
    const std::wstring chipKey = Field(set, "chipper="), paperKey = Field(set, "paper=");
    // This client's copy of the chipper: resolved, at rest where the host's birth stream set it, and showing it running.
    PS::WaitFor("this client's copy of the chipper resolves", kStepSec, [&] {
        bool ok = false;
        OnGameThread([&] { ok = EatOf(ByKey(chipKey)) != nullptr; });
        return ok;
    });
    AtRest(chipKey, kStepSec);
    FVector myBox{};
    OnGameThread([&] {
        if (void* eat = EatOf(ByKey(chipKey))) myBox = E::GetComponentLocation(eat);
    });
    char box[96];
    std::snprintf(box, sizeof(box), "%.1f,%.1f,%.1f", myBox.X, myBox.Y, myBox.Z);
    UE_LOGI("chipper-feed: CLIENT its chipper copy's box at %s", box);
    PS::Mark("client_box", box);
    PS::WaitFor("this client's copy of the chipper shows it running", kStepSec, [&] {
        bool running = false;
        OnGameThread([&] {
            float fuel = 0.f;
            bool active = false;
            running = W::Read(ByKey(chipKey), fuel, active) && active && fuel > 0.f;
        });
        return running;
    });
    // The grab, as the use key takes it, from a spot the director walked to; the host's own line that it applied the hold
    // (its copy is driven by this client's slot) is what the carry waits on.
    if (!GrabPaper(paperKey)) {
        PS::Mark("grabbed", "none: no aim the player's trace would take, in six tries");
        return;
    }
    PS::Mark("grabbed");
    if (!PS::Await("hold_applied", kStepSec)) return;
    if (Manual()) {
        UE_LOGI("chipper-feed: CLIENT HAND-OVER -- this player holds the paper; put it in the chipper's cone (%d s)",
                kManualSec);
        PS::Mark("fed", "held touches=by hand");
        PS::Await("host_done", kManualSec + 60);
        UE_LOGI("chipper-feed: DONE on client");
        return;
    }
    const bool thrown = Thrown();
    const CarryResult carried = CarryToBox(chipKey, paperKey, !thrown);
    // A thrown paper is in flight when the throw returns: this client's copy comes to rest before it is reported.
    if (thrown && carried == CarryResult::Done) AtRest(paperKey, kStepSec);
    UE_LOGI("chipper-feed: CLIENT the %s ended %d", thrown ? "throw" : "carry", static_cast<int>(carried));
    // The client says its own setup verdict by the carry's event, never by this chipper copy's overlap (it raises none): INVALID
    // when no stand or arc let the paper in, the hold or the throw failed, or the paper was held inside the box and nothing
    // came. Done and Gone (the paper destroyed on the way) are for the host's record to judge.
    if (carried == CarryResult::NoSide || carried == CarryResult::Lost ||
        carried == CarryResult::NotEaten) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "the %s did not reach the chipper (%s): closest approach %.0f cm to the box's centre (1e9 = never "
                               "measured)", thrown ? "throw" : "carry",
                               carried == CarryResult::NoSide ? "no stand let it place the paper"
                               : carried == CarryResult::NotEaten ? "held inside the box, the host's chipper took nothing"
                                                                  : "the hold or the throw failed",
                               static_cast<double>(g_carryClosestCm));
        PS::Mark("fed", "no side");
        return;
    }
    PS::Mark("fed", thrown ? "thrown" : "held");
    PS::Await("host_done", kManualSec);
    UE_LOGI("chipper-feed: DONE on client");
}

}  // namespace

DWORD WINAPI ChipperFeedThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
