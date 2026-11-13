// harness/autotest/autotest_world_sound.cpp -- TEST-ONLY: the earth teleport's sounds, a faucet's click and the alien
// noises across two peers.
//
// Armed by the row world_sound_drill on both peers (the peers' folder is VOTVCOOP_CAN_DIR). What a peer hears cannot be
// read back, so each term is the OTHER peer's receipt and its play call, from the lane's own counts:
//   1     A faucet each peer spawns under one key is pressed by the client, then by the host: the other peer's copy must
//         flip and its click count rise. The host runs the sky's tp: the client must receive both cues and play both.
//         The host fires the alien sounds: each peer must have three noisers around it and see them make a noise.
//         The host says `rig: VERDICT world-sound PASS|FAIL|INVALID`. Reds: world_sound_earth_off on the host,
//         world_sound_faucet_off on the receiving peer.
//   join  With the rig's --rejoin. The host fires the alien sounds and, once its noisers are out, the client leaves; the
//         rejoined client (VOTVCOOP_WORLD_SOUND_DRILL_REJOIN=1) must have three noisers of its own. The host says `rig:
//         VERDICT world-sound-join PASS|FAIL|INVALID`; INVALID when the host's noisers are gone at the ready edge. Red:
//         alien_join_off on the host.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/event_fire_sync.h"
#include "coop/world/world_sound_sync.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_set>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace A = ue_wrap::appliance;
namespace PS = harness::autotest::peer_signal;
namespace RV = coop::rig_verdict;
namespace WS = coop::world_sound_sync;
using disc_drill::OnGameThread;

constexpr const char* kTag = "world-sound";
constexpr const char* kTagJoin = "world-sound-join";
constexpr uint8_t kActionFaucet = 5;   // the faucet's use action, which its click follows
constexpr size_t kNoisers = 3;         // the alien sounds' count
constexpr int kStepSec = 120;
constexpr int kCrossSec = 15;          // a faucet's state, and its click, reaching the other peer
constexpr int kEarthSec = 150;         // the sky's tp plays its second sound over a minute after its first
constexpr int kAlienSec = 60;          // three noisers out, and one of them making a noise

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::world_sound_drill); }

// The live noisers here, the most seen at once, and each alien noise their timers started.
struct Noisers {
    size_t most = 0;
    std::unordered_set<void*> noises;
};
void Sample(Noisers& out) {
    OnGameThread([&] {
        size_t live = 0;
        for (void* n : R::FindObjectsByClass(L"noiser_C"))
            if (R::IsLive(n)) ++live;
        out.most = (std::max)(out.most, live);
        void* const audio = ue_wrap::object_index::ClassByName(L"AudioComponent");
        const int32_t soundOff = audio ? R::FindPropertyOffset(audio, L"Sound") : -1;
        if (soundOff < 0) return;
        struct Ctx {
            int32_t off;
            std::unordered_set<void*>* seen;
        } ctx{soundOff, &out.noises};
        ue_wrap::object_index::ForEachInstance(
            audio,
            [](void* c, void* comp, int32_t) {
                auto* x = static_cast<Ctx*>(c);
                if (!R::IsLive(comp) || R::ClassNameOf(R::OuterOf(comp)) != L"noiser_C") return;
                void* sound = *reinterpret_cast<void**>(static_cast<uint8_t*>(comp) + x->off);
                if (sound && _wcsicmp(R::ToString(R::NameOf(sound)).c_str(), L"outsideAlien_Cue") == 0) x->seen->insert(comp);
            },
            &ctx);
    });
}

size_t NoisersNow() {
    size_t live = 0;
    OnGameThread([&] {
        for (void* n : R::FindObjectsByClass(L"noiser_C"))
            if (R::IsLive(n)) ++live;
    });
    return live;
}

// The noisers around this peer: three out, and (when `noise`) one of them heard making a noise.
Noisers WatchNoisers(bool noise) {
    Noisers n;
    PS::WaitFor("three noisers around this player", kAlienSec, [&] {
        Sample(n);
        return n.most >= kNoisers && (!noise || !n.noises.empty());
    }, false);
    return n;
}

std::string Text(const Noisers& n) { return "most=" + std::to_string(n.most) + " noises=" + std::to_string(n.noises.size()); }

// The one keyed faucet both peers' sorted keys agree on.
void* FirstFaucet() {
    struct Best {
        void* actor = nullptr;
        std::wstring key;
    } best;
    A::EnsureResolved();
    void* const cls = ue_wrap::object_index::ClassByName(L"faucet_C");
    if (!cls) return nullptr;
    ue_wrap::object_index::ForEachInstance(
        cls,
        [](void* ctx, void* obj, int32_t) {
            auto* b = static_cast<Best*>(ctx);
            const std::wstring key = A::GetKeyString(obj);
            if (R::IsLive(obj) && !key.empty() && key != L"None" && (!b->actor || key < b->key)) {
                b->actor = obj;
                b->key = key;
            }
        },
        &best);
    return best.actor;
}

bool FaucetOn(bool& on) {
    bool read = false;
    OnGameThread([&] { read = A::TryReadState(FirstFaucet(), on); });
    return read;
}

uint32_t Clicks() {
    uint32_t n = 0;
    OnGameThread([&] { n = WS::GetCounts().clicks; });
    return n;
}

void PressFaucet() {
    OnGameThread([&] {
        void* faucet = FirstFaucet();
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!faucet || !local || !E::TryGetActorLocation(faucet, at)) {
            UE_LOGW("world-sound: no faucet to press");
            return;
        }
        void* mesh = ue_wrap::prop::GetStaticMesh(faucet);
        ue_wrap::prop_use::CallActionOptionIndex(faucet, local, mesh, at, kActionFaucet, mesh);
    });
}

// This peer's copy takes the other peer's press: the state flips to `want`, and the click is played for it. "1" when both.
std::string Crossed(bool want, uint32_t clicksBefore) {
    const bool flipped = PS::WaitFor("the faucet at the other peer's state", kCrossSec, [want] {
        bool on = !want;
        return FaucetOn(on) && on == want;
    }, false);
    const bool clicked = flipped && PS::WaitFor("the faucet's click played here", kCrossSec,
                                                [clicksBefore] { return Clicks() > clicksBefore; }, false);
    return std::to_string(flipped ? 1 : 0) + " " + std::to_string(clicked ? 1 : 0);
}

// The save holds no faucet and a spawned one does not cross, so each peer spawns one under the same Key, as a saved faucet
// would have it. Returns the Key.
std::string SpawnFaucet(const std::string& wantKey) {
    std::string out;
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/faucet.faucet_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        void* made = cls && local && E::TryGetActorLocation(local, me) ? E::SpawnActor(cls, {me.X + 100.f, me.Y, me.Z}) : nullptr;
        if (!made) return;
        if (!wantKey.empty()) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(made), L"Key");
            const std::wstring wide(wantKey.begin(), wantKey.end());
            const R::FName name = ue_wrap::fname_utils::StringToFName(wide.c_str());
            if (off >= 0) std::memcpy(static_cast<uint8_t*>(made) + off, &name, sizeof(name));
        }
        const std::wstring key = A::GetKeyString(made);
        out.assign(key.begin(), key.end());
    });
    return out;
}

bool Parse2(const std::string& text, int& a, int& b) { return sscanf_s(text.c_str(), "%d %d", &a, &b) == 2; }

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    const std::string key = SpawnFaucet("");
    UE_LOGI("world-sound: host faucet key='%hs'", key.c_str());
    PS::Mark("faucet_key", key);
    if (!PS::Await("client_faucet", kStepSec)) return;
    bool start = false;
    if (!PS::WaitFor("the host's faucet readable", 30, [&] { return FaucetOn(start); })) return;

    // The client presses; this peer's copy must flip and click.
    const uint32_t clicks0 = Clicks();
    PS::Mark("f1_go", start ? "1" : "0");
    if (!PS::Await("f1_done", kStepSec)) return;
    int f1Flip = 0, f1Click = 0;
    Parse2(Crossed(!start, clicks0), f1Flip, f1Click);
    UE_LOGI("world-sound: HOST the client's press: flipped=%d clicked=%d", f1Flip, f1Click);

    // The host presses; the client's copy must flip and click.
    PS::Mark("f2_go", start ? "1" : "0");
    PressFaucet();
    PS::Mark("f2_pressed");
    std::string f2;
    if (!PS::AwaitText("f2_result", f2, kStepSec)) return;
    int f2Flip = 0, f2Click = 0;
    Parse2(f2, f2Flip, f2Click);

    // The sky's tp: two cues to the client.
    PS::Mark("earth_go");
    OnGameThread([&] {
        void* sky = R::FindObjectByClass(L"newsky_C");
        const bool ran = sky && ue_wrap::component_calls::CallParamlessNamed(sky, L"tp");
        UE_LOGI("world-sound: the sky's tp ran=%d", ran ? 1 : 0);
    });
    std::string earth;
    if (!PS::AwaitText("earth_result", earth, kEarthSec + 60)) return;
    int received = 0, played = 0;
    Parse2(earth, received, played);
    uint32_t sent = 0;
    OnGameThread([&] { sent = WS::GetCounts().sent; });

    // The alien sounds: each peer's own noisers.
    const bool fired = coop::event_fire_sync::HostFire(coop::event_fire_sync::FireKind::SpecialEvent, L"alienSounds", L"None");
    UE_LOGI("world-sound: the alien sounds fired=%d", fired ? 1 : 0);
    PS::Mark("alien_go");
    const Noisers mine = WatchNoisers(true);
    std::string theirs;
    if (!PS::AwaitText("alien_result", theirs, kAlienSec + 60)) return;
    size_t cMost = 0, cNoises = 0;
    sscanf_s(theirs.c_str(), "most=%zu noises=%zu", &cMost, &cNoises);
    PS::Mark("host_done");

    if (!fired) {
        RV::Say(kTag, RV::Outcome::Invalid, "the alien sounds did not fire");
        return;
    }
    if (!f1Flip || !f2Flip) {
        RV::Say(kTag, RV::Outcome::Invalid, "a faucet's state did not cross (client's press=%d, host's press=%d): its lane had not listed it", f1Flip, f2Flip);
        return;
    }
    const bool pass = f1Flip && f1Click && f2Flip && f2Click && sent == 2 && received == 2 && played == 2 &&
                      mine.most >= kNoisers && !mine.noises.empty() && cMost >= kNoisers && cNoises > 0;
    RV::Say(kTag, pass ? RV::Outcome::Pass : RV::Outcome::Fail,
            "faucet: the client's press flipped the host's copy=%d and clicked there=%d, the host's press flipped the client's=%d "
            "and clicked there=%d; earth: the host sent %u cues, the client received %d and played %d; alien: the host %s, the "
            "client most=%zu noises=%zu",
            f1Flip, f1Click, f2Flip, f2Click, sent, received, played, Text(mine).c_str(), cMost, cNoises);
    UE_LOGI("world-sound: DONE on host");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("client_ready");
    std::string key;
    if (!PS::AwaitText("faucet_key", key, kStepSec)) return;
    UE_LOGI("world-sound: client faucet key='%hs'", SpawnFaucet(key).c_str());
    PS::Mark("client_faucet");
    std::string start;
    if (!PS::AwaitText("f1_go", start, kStepSec)) return;
    const bool on = start == "1";
    PressFaucet();
    PS::Mark("f1_done");

    // The host presses; this peer's copy must flip and click.
    if (!PS::AwaitText("f2_go", start, kStepSec)) return;
    const uint32_t clicks0 = Clicks();
    if (!PS::Await("f2_pressed", kStepSec)) return;
    PS::Mark("f2_result", Crossed(on, clicks0));

    // The sky's tp: both cues received and played here.
    if (!PS::Await("earth_go", kStepSec)) return;
    PS::WaitFor("both of the sky's sounds played here", kEarthSec, [] {
        uint32_t played = 0;
        OnGameThread([&] { played = WS::GetCounts().played; });
        return played >= 2;
    }, false);
    WS::Counts c;
    OnGameThread([&] { c = WS::GetCounts(); });
    UE_LOGI("world-sound: CLIENT the sky's sounds: received=%u played=%u", c.received, c.played);
    PS::Mark("earth_result", std::to_string(c.received) + " " + std::to_string(c.played));

    if (!PS::Await("alien_go", kStepSec)) return;
    PS::Mark("alien_result", Text(WatchNoisers(true)));
    PS::Await("host_done", kStepSec);
    UE_LOGI("world-sound: DONE on client");
}

// ---- join: a client that arrives while the host's noisers are out ----

void RunJoinHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    const bool fired = coop::event_fire_sync::HostFire(coop::event_fire_sync::FireKind::SpecialEvent, L"alienSounds", L"None");
    UE_LOGI("world-sound: the alien sounds fired=%d", fired ? 1 : 0);
    if (!fired || !WatchNoisers(false).most) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host's noisers did not come out");
        return;
    }
    PS::Mark("alien_out");
    if (!PS::Await("client_done", kStepSec)) return;
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    PS::Mark("host_ack");
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    size_t atEdge = 0;
    if (!PS::WaitFor("the rejoined client's world ready", 400, [life1Gen, &atEdge] {
            bool ready = false;
            OnGameThread([&] {
                auto& s = harness::session_runtime::Session();
                ready = s.IsSlotWorldReady(1) && s.peerGenerationForSlot(1) != life1Gen;
            });
            if (ready) atEdge = NoisersNow();
            return ready;
        }))
        return;
    UE_LOGI("world-sound: HOST the rejoined client's world is up; the host has %zu noisers out", atEdge);
    std::string theirs;
    if (!PS::AwaitText("rejoin_done", theirs, kAlienSec + 60)) return;
    size_t most = 0;
    sscanf_s(theirs.c_str(), "most=%zu", &most);
    if (atEdge < kNoisers) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host had %zu noisers out at the client's ready edge", atEdge);
        return;
    }
    RV::Say(kTagJoin, most >= kNoisers ? RV::Outcome::Pass : RV::Outcome::Fail,
            "the host had %zu noisers out at the client's ready edge; the rejoined client had %zu around it", atEdge, most);
    UE_LOGI("world-sound: JOIN DONE on host");
}

void RunJoinClient() {
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("client_ready");
    if (!PS::Await("alien_out", kStepSec)) return;
    PS::Mark("client_done");
    PS::Await("host_ack", 60);
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "world sound drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
    UE_LOGI("world-sound: DONE on client");
}

void RunRejoin() {
    if (!disc_drill::HostPuppetUp()) return;
    const Noisers n = WatchNoisers(false);
    UE_LOGI("world-sound: REJOIN the noisers around this player: %zu", n.most);
    PS::Mark("rejoin_done", "most=" + std::to_string(n.most));
    UE_LOGI("world-sound: REJOIN DONE");
}

}  // namespace

DWORD WINAPI WorldSoundThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    const bool join = Arm() == "join";
    if (!IsClientRole()) {
        if (join) RunJoinHost();
        else RunHost();
    } else if (!join) {
        RunClient();
    } else if (coop::config::ReadEnv("VOTVCOOP_WORLD_SOUND_DRILL_REJOIN") == "1") {
        RunRejoin();
    } else {
        RunJoinClient();
    }
    return 0;
}

}  // namespace harness::autotest
