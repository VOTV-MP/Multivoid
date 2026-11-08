// harness/autotest/autotest_chipper_feed.cpp -- TEST-ONLY: a client feeds a running woodchipper by hand.
//
// The host drops a chipper upright on level open ground (VOTVCOOP_CHIPPER_FEED_AT, or level_ground.h finds it), so its
// birth stream sets both copies down alike (their eat boxes must be one place), fuels it, switches it on and lays a
// paper beside it. The client grabs the paper as the use key does (coop/director/aimed_grab); the host's copy is then
// its stream's, out of the simulation, which the intake refuses, so the feed lane (coop/interactables/woodchipper_feed)
// is what makes the host's chipper eat it. VOTVCOOP_CHIPPER_FEED_MANUAL=1 stops there for a person to put the paper
// into the cone (up to 10 minutes): the measured arm. Without it the client walks round the chipper looking at its eat
// box until the box overlaps the carried paper, holds it 2 s and lets go; the cone opens on the side at another height
// than the box, so that carry can miss. The host says `rig: VERDICT chipper-feed PASS|FAIL|INVALID`: PASS when the paper
// is gone on both peers, with whether it went while carried and the lane's counts; INVALID when the chipper does not
// stand upright, the boxes stand apart or the paper never reached the box. Gated by VOTVCOOP_RUN_CHIPPER_FEED; the
// peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "level_ground.h"  // co-located private header: level open ground round the host
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"  // LookAt
#include "coop/interactables/woodchipper_feed.h"
#include "coop/interactables/woodchipper_gate.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/woodchipper.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor, controller, camera

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace W = ue_wrap::woodchipper;
using disc_drill::OnGameThread;
using ue_wrap::FVector;

constexpr const char* kTag = "chipper-feed";
constexpr const wchar_t* kChipperPath = L"/Game/objects/woodchipper.woodchipper_C";
constexpr const wchar_t* kPaperPath = L"/Game/objects/prop_paper.prop_paper_C";
constexpr int kStepSec = 120;
constexpr int kEatSec = 60;
constexpr float kUprightDeg = 5.f;
constexpr float kStepCm = 15.f;    // a walking step; a teleport much longer drops the grab

// VOTVCOOP_CHIPPER_FEED_MANUAL=1: the drill stops once the client holds the paper, and a person playing the client puts
// it in the chipper's cone; both peers then wait up to 10 minutes for it to go.
bool Manual() { return coop::config::ReadEnv("VOTVCOOP_CHIPPER_FEED_MANUAL") == "1"; }
constexpr int kManualSec = 600;

void* ByKey(const std::wstring& key) {
    void* a = nullptr;
    return coop::prop_key_index::Resolve(key, &a) == coop::prop_key_index::Read::Hit ? a : nullptr;
}

bool Gone(const std::wstring& key) {
    bool gone = false;
    OnGameThread([&] {
        void* a = nullptr;
        gone = coop::prop_key_index::Resolve(key, &a) == coop::prop_key_index::Read::Miss;
    });
    return gone;
}

std::wstring KeyOfSpawn(void* actor) {
    std::wstring key;
    for (int i = 0; i < 100 && key.empty(); ++i) {
        OnGameThread([&] {
            if (!R::IsLive(actor)) return;
            std::wstring k = ue_wrap::prop::GetKeyString(actor);
            if (!k.empty() && k != L"None") key = std::move(k);
        });
        if (key.empty()) ::Sleep(100);
    }
    return key;
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

std::wstring Field(const std::string& text, const char* name) {
    const size_t at = text.find(name);
    if (at == std::string::npos) return {};
    const size_t from = at + std::strlen(name), to = text.find(' ', from);
    const std::string v = text.substr(from, to == std::string::npos ? std::string::npos : to - from);
    return std::wstring(v.begin(), v.end());
}

void* EatOf(void* chipper) {
    const int32_t off = chipper ? R::FindPropertyOffset(R::ClassOf(chipper), L"eat") : -1;
    void* eat = nullptr;
    if (off >= 0) std::memcpy(&eat, static_cast<const uint8_t*>(chipper) + off, sizeof(eat));
    return eat;
}

// Whether the eat box overlaps the paper's mesh now, as the engine counts it (PrimitiveComponent::IsOverlappingComponent).
bool InBox(void* eat, void* paper) {
    void* mesh = paper ? ue_wrap::prop::GetStaticMesh(paper) : nullptr;
    void* fn = eat && mesh ? R::FindFunction(R::ClassOf(eat), L"IsOverlappingComponent") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"OtherComp", mesh) || !ue_wrap::Call(eat, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

float Dist(const FVector& a, const FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return std::sqrt(x * x + y * y + z * z);
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    FVector ground{};
    if (!level_ground::Find(ground, "VOTVCOOP_CHIPPER_FEED_AT")) {
        PS::Mark("set", "none");
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "no level open ground round the host");
        UE_LOGI("chipper-feed: DONE on host");
        return;
    }
    void* chipper = nullptr;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        void* chipCls = ue_wrap::asset_load::LoadObjectByPath(kChipperPath);
        if (!me || !chipCls) return;
        E::TeleportTo(me, ground, ue_wrap::FRotator{});
        // Upright, a hand's breadth above the ground, away from both players: it lands on its feet, and its fall is the
        // host's own birth stream, so the client's copy rests where the host's does.
        chipper = E::SpawnActor(chipCls, {ground.X + 400.f, ground.Y, ground.Z - 60.f});
    });
    ::Sleep(6000);  // it lands, its birth stream ends, and the end reaches the client
    ue_wrap::FRotator rot{};
    OnGameThread([&] {
        if (chipper && R::IsLive(chipper)) E::TryGetActorRotation(chipper, rot);
    });
    const std::wstring chipKey = chipper ? KeyOfSpawn(chipper) : std::wstring();
    UE_LOGI("chipper-feed: HOST chipper '%ls' stands at pitch %.1f roll %.1f", chipKey.c_str(), rot.Pitch, rot.Roll);
    if (chipKey.empty() || std::fabs(rot.Pitch) > kUprightDeg || std::fabs(rot.Roll) > kUprightDeg) {
        PS::Mark("set", "none");
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "the chipper does not stand upright on the ground at %.0f,%.0f,%.0f (pitch %.1f roll %.1f)",
                               ground.X, ground.Y, ground.Z, rot.Pitch, rot.Roll);
        UE_LOGI("chipper-feed: DONE on host");
        return;
    }
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
    if (paperKey.empty()) {
        PS::Mark("set", "none");
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the paper could not be spawned with a key");
        UE_LOGI("chipper-feed: DONE on host");
        return;
    }
    ::Sleep(3000);  // the paper comes to rest and the chipper's state reaches the client
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
        std::sscanf(box.c_str(), "%f,%f,%f", &clientBox.X, &clientBox.Y, &clientBox.Z) != 3 || Dist(hostBox, clientBox) > 30.f) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "the client's chipper copy does not stand where the host's does (its box %s, the host's "
                               "%.0f,%.0f,%.0f)", box.c_str(), hostBox.X, hostBox.Y, hostBox.Z);
        PS::Mark("host_done");
        UE_LOGI("chipper-feed: DONE on host");
        return;
    }
    std::string fed;
    if (!PS::AwaitText("fed", fed, kStepSec * 2) || fed.rfind("held", 0) != 0) {  // "held touches=N"
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the client did not hold the paper in the box (%s)",
                               fed.c_str());
        PS::Mark("host_done");
        UE_LOGI("chipper-feed: DONE on host");
        return;
    }
    const bool hostGone =
        PS::WaitFor("the host's chipper eats the paper", Manual() ? kManualSec : kEatSec, [&] { return Gone(paperKey); }, false);
    std::string theirs;
    PS::AwaitText("client_result", theirs, kStepSec);
    PS::Mark("host_done");
    coop::woodchipper_feed::Counts c{};
    OnGameThread([&] { c = coop::woodchipper_feed::GetCounts(); });
    int clientGone = 0, asked = 0, whileHeld = 0;
    std::sscanf(theirs.c_str(), "gone=%d asked=%d held=%d", &clientGone, &asked, &whileHeld);
    coop::rig_verdict::Say(kTag, hostGone && clientGone == 1 ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail,
                           "the client carried paper %s into the box (%s): gone on the host=%d, on the client=%d, while "
                           "still carried=%d; the client asked %d feed(s), the host fed %u and refused %u",
                           Narrow(paperKey).c_str(), fed.c_str() + 5, hostGone ? 1 : 0, clientGone, whileHeld, asked, c.fed,
                           c.refused);
    UE_LOGI("chipper-feed: DONE on host");
}

bool g_eatenOnTheWay = false;
  // CLIENT: the paper went while the grab carried it to the box

// The client's walk with the paper in its grab: round the box, looking at it, until the paper sits in it. True once it
// does; false when no side lets it in or the grab lets go.
bool CarryIntoBox(const std::wstring& chipKey, const std::wstring& paperKey) {
    // The way back first: a paper on a slope rolls off before the grab takes it, so the walk to the chipper has no cap
    // but the drill's own (3 minutes of steps).
    for (int i = 0; i < 3600; ++i) {
        bool lost = false, close = false;
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            void* p = ByKey(paperKey);
            void* eat = EatOf(ByKey(chipKey));
            FVector ml{};
            if (me && !p) {
                g_eatenOnTheWay = true;
                return;
            }
            if (!me || !eat || coop::director::Grabbing(me) != p || !E::TryGetActorLocation(me, ml)) {
                lost = true;
                return;
            }
            const FVector cam = E::GetCameraLocation(), e = E::GetComponentLocation(eat);
            E::SetControlRotation(E::GetController(me), coop::director::LookAt(cam, e));
            const float mx = e.X - cam.X, my = e.Y - cam.Y, m = std::sqrt(mx * mx + my * my);
            close = m < 250.f;
            if (!close) E::SetActorLocation(me, FVector{ml.X + mx / m * kStepCm, ml.Y + my / m * kStepCm, ml.Z});
            if (i % 200 == 0) UE_LOGI("chipper-feed: CLIENT the way back: %.0f cm to the chipper", m);
        });
        if (g_eatenOnTheWay) return true;
        if (lost) return coop::woodchipper_feed::GetCounts().asked > 0;  // fed: the hand let go of it
        if (close) break;
        ::Sleep(50);
    }
    for (int side = 0; side < 8; ++side) {
        const float a = 3.14159f + 0.7854f * static_cast<float>(side);  // the paper's side first, then round
        for (int i = 0; i < 80; ++i) {
            bool inside = false, lost = false, there = false;
            OnGameThread([&] {
                void* me = coop::players::Registry::Get().Local();
                void* p = ByKey(paperKey);
                void* eat = EatOf(ByKey(chipKey));
                FVector pl{}, ml{};
                if (me && !p) {
                    g_eatenOnTheWay = true;
                    return;
                }
                if (!me || !eat || !E::TryGetActorLocation(p, pl) || !E::TryGetActorLocation(me, ml)) {
                    lost = true;
                    return;
                }
                if (coop::director::Grabbing(me) != p) {
                    lost = true;
                    return;
                }
                const FVector cam = E::GetCameraLocation(), e = E::GetComponentLocation(eat);
                E::SetControlRotation(E::GetController(me), coop::director::LookAt(cam, e));
                inside = InBox(eat, p);
                // Where to stand: on this side, as far from the box as the grab holds the paper from the camera.
                const float hold = Dist(pl, cam), dz = cam.Z - e.Z;
                const float rh = std::sqrt(std::fmax(hold * hold - dz * dz, 900.f));
                const float tx = e.X + rh * std::cos(a), ty = e.Y + rh * std::sin(a);
                const float mx = tx - cam.X, my = ty - cam.Y, m = std::sqrt(mx * mx + my * my);
                there = m < 5.f;
                if (!inside && !there) {
                    const float k = (m > kStepCm ? kStepCm : m) / m;
                    E::SetActorLocation(me, FVector{ml.X + mx * k, ml.Y + my * k, ml.Z});
                }
                if (i % 20 == 0)
                    UE_LOGI("chipper-feed: CLIENT side %d step %d: the paper %.0f cm off the box, %.0f cm to stand", side,
                            i, Dist(pl, e), m);
            });
            if (g_eatenOnTheWay) return true;
            if (lost) return coop::woodchipper_feed::GetCounts().asked > 0;  // fed: the hand let go of it
            if (inside) return true;
            if (there) break;
            ::Sleep(50);
        }
        // At the stand: a moment for the grab to bring the paper along.
        for (int i = 0; i < 10; ++i) {
            bool inside = false;
            OnGameThread([&] {
                void* me = coop::players::Registry::Get().Local();
                void* p = ByKey(paperKey);
                void* eat = EatOf(ByKey(chipKey));
                FVector pl{};
                if (me && !p) g_eatenOnTheWay = true;
                if (!me || !p || !eat || !E::TryGetActorLocation(p, pl)) return;
                const FVector cam = E::GetCameraLocation(), e = E::GetComponentLocation(eat);
                E::SetControlRotation(E::GetController(me), coop::director::LookAt(cam, e));
                inside = InBox(eat, p);
            });
            if (inside || g_eatenOnTheWay) return true;
            ::Sleep(100);
        }
    }
    return false;
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    ::Sleep(20000);  // past the join's reconcile window
    PS::Mark("client_ready");
    std::string set;
    if (!PS::AwaitText("set", set, kStepSec) || set == "none") return;
    const std::wstring chipKey = Field(set, "chipper="), paperKey = Field(set, "paper=");
    FVector myBox{};
    for (int i = 0; i < 50; ++i) {
        bool ok = false;
        OnGameThread([&] {
            if (void* eat = EatOf(ByKey(chipKey))) {
                myBox = E::GetComponentLocation(eat);
                ok = true;
            }
        });
        if (ok) break;
        ::Sleep(100);
    }
    char box[96];
    std::snprintf(box, sizeof(box), "%.1f,%.1f,%.1f", myBox.X, myBox.Y, myBox.Z);
    UE_LOGI("chipper-feed: CLIENT its chipper copy's box at %s", box);
    PS::Mark("client_box", box);
    // The grab, as the use key takes it: from 70 cm off the paper, a step round it each try (a flat paper can escape
    // the player's trace from one side).
    std::shared_ptr<coop::director::AimedGrab> grab;
    coop::director::GrabState st = coop::director::GrabState::Failed;
    for (int attempt = 0; attempt < 6 && st != coop::director::GrabState::Grabbed; ++attempt) {
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            void* p = ByKey(paperKey);
            FVector pl{};
            if (!me || !p || !E::TryGetActorLocation(p, pl)) return;
            const float a = 3.14159f + 0.6f * static_cast<float>(attempt);
            E::TeleportTo(me, FVector{pl.X + 70.f * std::cos(a), pl.Y + 70.f * std::sin(a), pl.Z + 100.f},
                          ue_wrap::FRotator{});
            grab = std::make_shared<coop::director::AimedGrab>(me, p);
        });
        ::Sleep(700);
        st = coop::director::GrabState::Failed;
        for (int i = 0; grab && i < 1200; ++i) {
            OnGameThread([&] { st = grab->Tick(); });
            if (st != coop::director::GrabState::Working) break;
            ::Sleep(16);
        }
        if (st != coop::director::GrabState::Grabbed)
            UE_LOGI("chipper-feed: CLIENT grab try %d failed (%s)", attempt + 1, grab ? grab->Why() : "no paper or player");
    }
    if (st != coop::director::GrabState::Grabbed) {
        PS::Mark("fed", "no grab");
        return;
    }
    if (Manual()) {
        UE_LOGI("chipper-feed: CLIENT HAND-OVER -- this player holds the paper; put it in the chipper's cone (%d s)",
                kManualSec);
        PS::Mark("fed", "held touches=by hand");
        const bool gone = PS::WaitFor("the paper goes into the chipper by hand", kManualSec, [&] { return Gone(paperKey); },
                                      false);
        coop::woodchipper_feed::Counts c{};
        OnGameThread([&] { c = coop::woodchipper_feed::GetCounts(); });
        UE_LOGI("chipper-feed: CLIENT the paper gone=%d by hand, asked %u", gone ? 1 : 0, c.asked);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "gone=%d asked=%u held=0", gone ? 1 : 0, c.asked);
        PS::Mark("client_result", buf);
        PS::Await("host_done", kStepSec);
        UE_LOGI("chipper-feed: DONE on client");
        return;
    }
    uint64_t touches = 0;
    OnGameThread([&] { touches = coop::woodchipper_gate::GetCounts().eatRefused; });
    const bool carried = CarryIntoBox(chipKey, paperKey);
    OnGameThread([&] { touches = coop::woodchipper_gate::GetCounts().eatRefused - touches; });
    UE_LOGI("chipper-feed: CLIENT the carry: %llu entries into this copy's eat box", static_cast<unsigned long long>(touches));
    if (!carried && touches == 0) {
        UE_LOGW("chipper-feed: CLIENT no side of the chipper let the held paper into the box");
        PS::Mark("fed", "no side");
        return;
    }
    const bool fedOnTheWay = !g_eatenOnTheWay && coop::woodchipper_feed::GetCounts().asked > 0;
    UE_LOGI("chipper-feed: CLIENT %s", g_eatenOnTheWay ? "the held paper went as it reached the box"
                                       : fedOnTheWay  ? "the held paper reached the box: fed, and the hand let go"
                                                      : "the held paper sits in the eat box");
    PS::Mark("fed", "held touches=" + std::to_string(touches));
    // Held there 2 s, the camera on the box; then the use key lets go.
    bool whileHeld = g_eatenOnTheWay;
    for (int i = 0; i < 20 && !whileHeld && !fedOnTheWay; ++i) {
        whileHeld = Gone(paperKey);
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            void* eat = EatOf(ByKey(chipKey));
            if (me && eat) E::SetControlRotation(E::GetController(me), coop::director::LookAt(E::GetCameraLocation(),
                                                                                              E::GetComponentLocation(eat)));
        });
        ::Sleep(100);
    }
    if (!whileHeld && !fedOnTheWay) {
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            void* p = ByKey(paperKey);
            const bool let = me && p && E::ReleaseMainPlayerGrabIfHolding(me, p);
            UE_LOGI("chipper-feed: CLIENT let the paper go (released=%d)", let ? 1 : 0);
        });
    } else {
        UE_LOGI("chipper-feed: CLIENT the paper went %s", fedOnTheWay ? "on the feed" : "while carried");
    }
    const bool gone = whileHeld || PS::WaitFor("the paper leaves the client's world", kEatSec, [&] { return Gone(paperKey); }, false);
    coop::woodchipper_feed::Counts c{};
    OnGameThread([&] { c = coop::woodchipper_feed::GetCounts(); });
    UE_LOGI("chipper-feed: CLIENT the paper gone=%d (while held %d), asked %u", gone ? 1 : 0, whileHeld ? 1 : 0, c.asked);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "gone=%d asked=%u held=%d", gone ? 1 : 0, c.asked, whileHeld ? 1 : 0);
    PS::Mark("client_result", buf);
    PS::Await("host_done", kStepSec);
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
