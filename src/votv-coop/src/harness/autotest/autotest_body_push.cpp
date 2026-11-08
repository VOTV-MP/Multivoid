// harness/autotest/autotest_body_push.cpp -- TEST-ONLY: a prop a client walks into stands the same on both peers.
//
// The host finds level open ground (harness/autotest/level_ground.h, or VOTVCOOP_BODY_PUSH_AT) and drops a woodchipper
// upright just above it, so its fall is the host's own birth stream and both copies come to rest together; each peer
// reads its copy's pose. Then the client walks into the chipper: from 120 cm off one side it steps 8 cm a time through
// where the chipper stands, two seconds, as a player walking into it. Eight seconds after, each peer reads its copy's
// pose again. The host says `rig: VERDICT body-push PASS|FAIL|INVALID`: PASS when the walk moved the host's copy (more
// than 5 cm or 3 degrees) and both copies then agree within 10 cm and 3 degrees; INVALID when the chipper does not
// land upright, the copies disagree before the walk, or the walk moved neither copy. VOTVCOOP_BODY_PUSH_ON_CLIENT=1
// instead bears the chipper where the client stands and judges how alike the copies rest. Gated by VOTVCOOP_RUN_BODY_PUSH;
// the peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "level_ground.h"  // co-located private header: level open ground round the host
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using disc_drill::OnGameThread;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr const char* kTag = "body-push";
constexpr const wchar_t* kChipperPath = L"/Game/objects/woodchipper.woodchipper_C";
constexpr int kStepSec = 120;

struct Pose {
    FVector loc{};
    FRotator rot{};
    bool ok = false;
};

Pose PoseOf(const std::wstring& key) {
    Pose p;
    OnGameThread([&] {
        void* a = nullptr;
        if (coop::prop_key_index::Resolve(key, &a) != coop::prop_key_index::Read::Hit || !a) return;
        p.ok = E::TryGetActorLocation(a, p.loc) && E::TryGetActorRotation(a, p.rot);
    });
    return p;
}

std::string Text(const Pose& p) {
    char b[128];
    std::snprintf(b, sizeof(b), "%.1f,%.1f,%.1f,%.2f,%.2f,%.2f", p.loc.X, p.loc.Y, p.loc.Z, p.rot.Pitch, p.rot.Yaw, p.rot.Roll);
    return b;
}

// VOTVCOOP_BODY_PUSH_ON_CLIENT=1: the chipper is born where the client stands, as a spawn into a player does, and the
// copies are compared once it rests; no walk.
bool OnClient() { return coop::config::ReadEnv("VOTVCOOP_BODY_PUSH_ON_CLIENT") == "1"; }

bool Parse(const std::string& s, Pose& p) {
    p.ok = std::sscanf(s.c_str(), "%f,%f,%f,%f,%f,%f", &p.loc.X, &p.loc.Y, &p.loc.Z, &p.rot.Pitch, &p.rot.Yaw,
                       &p.rot.Roll) == 6;
    return p.ok;
}

float Cm(const Pose& a, const Pose& b) {
    const float x = a.loc.X - b.loc.X, y = a.loc.Y - b.loc.Y, z = a.loc.Z - b.loc.Z;
    return std::sqrt(x * x + y * y + z * z);
}

float Deg(const Pose& a, const Pose& b) {
    const float p = std::fabs(ue_wrap::NormalizeAxis(a.rot.Pitch - b.rot.Pitch));
    const float y = std::fabs(ue_wrap::NormalizeAxis(a.rot.Yaw - b.rot.Yaw));
    const float r = std::fabs(ue_wrap::NormalizeAxis(a.rot.Roll - b.rot.Roll));
    return std::fmax(p, std::fmax(y, r));
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

void Say(coop::rig_verdict::Outcome o, const char* what) {
    coop::rig_verdict::Say(kTag, o, "%s", what);
    PS::Mark("host_done");
    UE_LOGI("body-push: DONE on host");
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    FVector ground{};
    if (!level_ground::Find(ground, "VOTVCOOP_BODY_PUSH_AT")) {
        PS::Mark("set", "none");
        return Say(coop::rig_verdict::Outcome::Invalid, "no level open ground round the host");
    }
    void* chipper = nullptr;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        if (me) E::TeleportTo(me, ground, FRotator{});
    });
    FVector spawnAt{ground.X + 400.f, ground.Y, ground.Z - 60.f};  // a hand's breadth up: it lands
    if (OnClient()) {
        char at[96];
        std::snprintf(at, sizeof(at), "%.1f,%.1f,%.1f", spawnAt.X, spawnAt.Y, spawnAt.Z + 40.f);
        PS::Mark("come", at);  // the client stands where the chipper will be born
        PS::Await("came", kStepSec);
        ::Sleep(2000);
        OnGameThread([&] {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
            void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
            FVector by{};
            if (body && E::TryGetActorLocation(body, by)) spawnAt = FVector{by.X, by.Y, by.Z - 40.f};
        });
    }
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(kChipperPath);
        if (cls) chipper = E::SpawnActor(cls, spawnAt);
    });
    std::wstring key;
    for (int i = 0; i < 100 && key.empty() && chipper; ++i) {
        OnGameThread([&] {
            if (!R::IsLive(chipper)) return;
            std::wstring k = ue_wrap::prop::GetKeyString(chipper);
            if (!k.empty() && k != L"None") key = k;
        });
        ::Sleep(100);
    }
    ::Sleep(6000);  // it lands, its birth stream ends, and the end reaches the client
    const Pose before = PoseOf(key);
    UE_LOGI("body-push: HOST chipper '%ls' rests at %s", key.c_str(), Text(before).c_str());
    if (key.empty() || !before.ok || (!OnClient() && (std::fabs(before.rot.Pitch) > 5.f || std::fabs(before.rot.Roll) > 5.f))) {
        PS::Mark("set", "none");
        return Say(coop::rig_verdict::Outcome::Invalid, "the chipper did not land upright");
    }
    PS::Mark("set", "chipper=" + Narrow(key));
    std::string theirs;
    Pose cBefore;
    if (!PS::AwaitText("client_before", theirs, kStepSec) || !Parse(theirs, cBefore))
        return Say(coop::rig_verdict::Outcome::Invalid, "the client never read its copy");
    if (OnClient()) {
        // The arm's whole question: born into the client, do the copies rest alike?
        const bool alike = Cm(before, cBefore) <= 10.f && Deg(before, cBefore) <= 3.f;
        char b[300];
        std::snprintf(b, sizeof(b), "the chipper was born where the client stood: the copies rest %.1f cm and %.1f deg apart "
                      "(host %s, client %s)", Cm(before, cBefore), Deg(before, cBefore), Text(before).c_str(), theirs.c_str());
        return Say(alike ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail, b);
    }
    if (Cm(before, cBefore) > 10.f || Deg(before, cBefore) > 3.f) {
        char b[256];
        std::snprintf(b, sizeof(b), "the copies disagree before the walk: host %s, client %s", Text(before).c_str(),
                      theirs.c_str());
        return Say(coop::rig_verdict::Outcome::Invalid, b);
    }
    PS::Mark("go");
    if (!PS::Await("walked", kStepSec)) return;
    ::Sleep(8000);
    const Pose after = PoseOf(key);
    PS::Mark("read_after");
    Pose cAfter;
    if (!PS::AwaitText("client_after", theirs, kStepSec) || !Parse(theirs, cAfter))
        return Say(coop::rig_verdict::Outcome::Invalid, "the client never read its copy after the walk");
    const float hostMoved = Cm(before, after), hostTurned = Deg(before, after);
    const float clientMoved = Cm(cBefore, cAfter), clientTurned = Deg(cBefore, cAfter);
    const float apart = Cm(after, cAfter), apartDeg = Deg(after, cAfter);
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the client walked into the chipper: the host's copy moved %.1f cm and turned %.1f deg, the client's "
                  "moved %.1f cm and turned %.1f deg; after, the copies stand %.1f cm and %.1f deg apart (host %s, client %s)",
                  hostMoved, hostTurned, clientMoved, clientTurned, apart, apartDeg, Text(after).c_str(), theirs.c_str());
    if ((hostMoved <= 5.f && hostTurned <= 3.f) && (clientMoved <= 5.f && clientTurned <= 3.f))
        return Say(coop::rig_verdict::Outcome::Invalid, b);
    const bool pass = (hostMoved > 5.f || hostTurned > 3.f) && apart <= 10.f && apartDeg <= 3.f;
    Say(pass ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail, b);
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    ::Sleep(20000);  // past the join's reconcile window
    PS::Mark("client_ready");
    if (OnClient()) {
        std::string at;
        FVector to{};
        if (!PS::AwaitText("come", at, kStepSec) || std::sscanf(at.c_str(), "%f,%f,%f", &to.X, &to.Y, &to.Z) != 3) return;
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            if (me) E::TeleportTo(me, to, FRotator{});
        });
        UE_LOGI("body-push: CLIENT stands where the chipper will be born");
        PS::Mark("came");
    }
    std::string set;
    if (!PS::AwaitText("set", set, kStepSec) || set == "none") return;
    const std::wstring key = Wide(set.substr(set.find("chipper=") + 8));
    Pose before;
    for (int i = 0; i < 50 && !before.ok; ++i) {
        before = PoseOf(key);
        if (!before.ok) ::Sleep(100);
    }
    UE_LOGI("body-push: CLIENT its copy rests at %s", Text(before).c_str());
    PS::Mark("client_before", Text(before));
    if (OnClient()) {
        PS::Await("host_done", kStepSec);
        UE_LOGI("body-push: DONE on client");
        return;
    }
    if (!PS::Await("go", kStepSec)) return;
    // The walk: from 120 cm off the chipper's -Y side, 8 cm a step through where it stands.
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        if (me) E::TeleportTo(me, FVector{before.loc.X, before.loc.Y - 120.f, before.loc.Z + 60.f}, FRotator{0.f, 90.f, 0.f});
    });
    ::Sleep(1000);
    for (int i = 0; i < 30; ++i) {
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            FVector ml{};
            if (me && E::TryGetActorLocation(me, ml)) E::SetActorLocation(me, FVector{ml.X, ml.Y + 8.f, ml.Z});
        });
        ::Sleep(66);
    }
    UE_LOGI("body-push: CLIENT walked into the chipper");
    PS::Mark("walked");
    if (!PS::Await("read_after", kStepSec)) return;
    const Pose after = PoseOf(key);
    UE_LOGI("body-push: CLIENT its copy now at %s", Text(after).c_str());
    PS::Mark("client_after", Text(after));
    PS::Await("host_done", kStepSec);
    UE_LOGI("body-push: DONE on client");
}

}  // namespace

DWORD WINAPI BodyPushThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
