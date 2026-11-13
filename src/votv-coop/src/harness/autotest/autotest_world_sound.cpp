// harness/autotest/autotest_world_sound.cpp -- TEST-ONLY: the earth teleport's sounds, a faucet's click and the alien
// noises across two peers. A verdict is decided by an event; a bound passing with none is INVALID. A client acts and
// leaves only once its own join is over, and the host's first step waits for that client's slot to be world-ready.
// Armed by the row world_sound_drill on both peers (VOTVCOOP_CAN_DIR). What a peer hears cannot be read back, so each
// term is the OTHER peer's receipt and its play call, from the lane's own counts:
//   1     A faucet each peer spawns under one key is pressed by the client, then by the host: the other peer's copy must
//         flip and its click count rise. The host fires the alien sounds: each peer must have three noisers around it
//         and see them make a noise. Last, the host runs the sky's tp: the client must receive both cues and play both.
//         `rig: VERDICT world-sound PASS|FAIL|INVALID`. Reds: world_sound_earth_off (host), world_sound_faucet_off.
//   join  With the rig's --rejoin. The host fires the alien sounds and, once its noisers are out, the client leaves; at
//         the rejoined client's ready edge the host counts its noisers out, and the rejoined client
//         (VOTVCOOP_WORLD_SOUND_DRILL_REJOIN=1) must have at least that many and at most the event's three (the replay
//         re-runs the event: a noiser the host already ended starts afresh on the joiner; measured host 2, joiner 3).
//         `rig: VERDICT world-sound-join ...`; INVALID when the host has none out at the edge. Red: alien_join_off.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/event_active_sync.h"
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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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
constexpr size_t kNoisers = 3;         // the alien sounds the host fires
constexpr int kStepSec = 120;
constexpr int kCrossSec = 15;          // INVALID bound: a faucet's state reaching the other peer
constexpr int kFirstSoundSec = 30;     // INVALID bound: the sky playing its first sound after tp
constexpr int kEarthSec = 150;         // INVALID bound: the sky playing its second sound, over a minute after its first
constexpr int kCueSec = 15;            // INVALID bound: a cue the host sent, received here
constexpr int kAlienSec = 60;          // INVALID bound: three noisers out, and one of them making a noise

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

// One of the noisers heard making a noise: they sound every 5 to 10 s, so a bound passing without one is INVALID. A peer that
// is waiting for another's verdict names the signal that ends its wait (the verdict said), and the wait is left at once.
bool WaitNoise(const char* stopSignal = nullptr) {
    Noisers n;
    return PS::WaitFor("a noiser's noise", kAlienSec, [&] {
        Sample(n);
        std::string unused;
        return !n.noises.empty() || (stopSignal && PS::Peek(stopSignal, unused));
    }, false) && !n.noises.empty();
}

// The host's alien event dispatched on this peer: the replay count rose. The noisers spawn inside that call, so their
// count is final once it has, and is read at once.
unsigned Replays() {
    unsigned n = 0;
    OnGameThread([&] { n = coop::event_fire_sync::ReplayCount(); });
    return n;
}


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

// The faucet this peer spawned, listed by the appliance lane: a press before that sends nothing. Waits for the lane's own
// index to hold it; a bound passing without it is INVALID, named.
bool WaitListed(const char* role) {
    const ULONGLONG t0 = GetTickCount64();
    const bool listed = PS::WaitFor("the appliance lane lists the spawned faucet", 30, [] {
        std::wstring key;
        OnGameThread([&] { key = coop::interactable_sync::ApplianceKey(FirstFaucet()); });
        return !key.empty();
    });
    if (listed)
        UE_LOGI("world-sound: %s faucet listed by the appliance lane after %llu ms", role,
                static_cast<unsigned long long>(GetTickCount64() - t0));
    return listed;
}

void PressFaucet(const char* role) {
    UE_LOGI("world-sound: %s presses the faucet", role);
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

// This peer's copy takes the other peer's press: "<flip> <click>". The flip is the event (a bound passing without it is
// -1, which is INVALID); the click is played in the same task that applies the flip, so its count is final when the flip
// is seen and the answer is read at once.
std::string Crossed(bool want, uint32_t clicksBefore) {
    const bool flipped = PS::WaitFor("the faucet at the other peer's state", kCrossSec, [want] {
        bool on = !want;
        return FaucetOn(on) && on == want;
    }, false);
    if (!flipped) return "-1 0";
    return std::string("1 ") + (Clicks() > clicksBefore ? "1" : "0");
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

// A verdict decided before the later steps ends the run there: the client is told each step is skipped, so it finishes at
// once and says nothing of its own.
void EndNow(RV::Outcome outcome, bool alienDone, const char* what, ...) {
    char text[600];
    va_list args;
    va_start(args, what);
    std::vsnprintf(text, sizeof(text), what, args);
    va_end(args);
    if (!alienDone) PS::Mark("alien_go", "skip");
    PS::Mark("earth_go");
    PS::Mark("earth_final", "0");
    PS::Mark("host_done");
    RV::Say(kTag, outcome, "%s", text);
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    if (!PS::WaitFor("the client's slot world-ready", 120, [] {
            bool ready = false;
            OnGameThread([&] { ready = harness::session_runtime::Session().IsSlotWorldReady(1); });
            return ready;
        }))
        return;
    const std::string key = SpawnFaucet("");
    UE_LOGI("world-sound: host faucet key='%hs'", key.c_str());
    if (!WaitListed("HOST")) return;
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
    // A state that crossed with no click played is decided here: the host's press and the later legs are not run.
    if (f1Flip == 1 && !f1Click) {
        PS::Mark("f2_go", "skip");
        EndNow(RV::Outcome::Fail, false, "a faucet's state crossed and no click was played for it here (the client's press: the "
               "host clicked=0)");
        return;
    }

    // The host presses; the client's copy must flip and click. The client takes its click baseline first and says it is
    // armed: a press landing before the baseline would read as no click.
    PS::Mark("f2_go", start ? "1" : "0");
    if (!PS::Await("f2_armed", kStepSec)) return;
    PressFaucet("HOST");
    PS::Mark("f2_pressed");
    std::string f2;
    if (!PS::AwaitText("f2_result", f2, kStepSec)) return;
    int f2Flip = 0, f2Click = 0;
    Parse2(f2, f2Flip, f2Click);
    // A decided FAIL is said before an INVALID of the other press.
    if (f2Flip == 1 && !f2Click) {
        EndNow(RV::Outcome::Fail, false, "a faucet's state crossed and no click was played for it here (the host's press: the "
               "client clicked=0)");
        return;
    }
    if (f1Flip != 1 || f2Flip != 1) {
        EndNow(RV::Outcome::Invalid, false, "a faucet's state did not cross (client's press=%d, host's press=%d): its lane had not "
               "listed it", f1Flip, f2Flip);
        return;
    }

    // The alien sounds: each peer's own noisers. The host's three spawn inside its fire; the client's are counted once the
    // host's event has dispatched there: fewer than three is a failure, decided at that point. A bound passing without the
    // dispatch, or without a noise, is INVALID.
    const bool fired = coop::event_fire_sync::HostFire(coop::event_fire_sync::FireKind::SpecialEvent, L"alienSounds", L"None");
    UE_LOGI("world-sound: the alien sounds fired=%d", fired ? 1 : 0);
    const size_t mineMost = fired ? NoisersNow() : 0;
    // Fewer than three noisers where the event dispatched is decided there: nothing later brings them, and no noise is
    // waited for first.
    if (fired && mineMost < kNoisers) {
        EndNow(RV::Outcome::Fail, false, "alien: the host's event left %zu noisers on the host (three)", mineMost);
        return;
    }
    PS::Mark("alien_go");
    std::string count;
    if (!PS::AwaitText("alien_count", count, kAlienSec + 60)) return;
    size_t cMost = 0;
    int cApplied = 0;
    sscanf_s(count.c_str(), "most=%zu applied=%d", &cMost, &cApplied);
    if (cApplied && cMost < kNoisers) {
        EndNow(RV::Outcome::Fail, true, "alien: the host's event left %zu noisers on the host and %zu on the client (three each)",
               mineMost, cMost);
        return;
    }
    const bool mineNoise = fired && WaitNoise();
    std::string theirs;
    if (!PS::AwaitText("alien_result", theirs, kAlienSec + 60)) return;
    int cNoise = 0;
    sscanf_s(theirs.c_str(), "noise=%d", &cNoise);

    // The sky's tp, last: two cues to the client. The host's hook sees each sound the sky plays (`matched`) and sends a
    // cue for it (`sent`), so a cue not sent is decided when its sound is played, and the client is told at once.
    PS::Mark("earth_go");
    OnGameThread([&] {
        void* sky = R::FindObjectByClass(L"newsky_C");
        const bool ran = sky && ue_wrap::component_calls::CallParamlessNamed(sky, L"tp");
        UE_LOGI("world-sound: the sky's tp ran=%d", ran ? 1 : 0);
    });
    auto counts = [] {
        WS::Counts c;
        OnGameThread([&] { c = WS::GetCounts(); });
        return c;
    };
    const bool first = PS::WaitFor("the sky's first sound", kFirstSoundSec, [&] { return counts().matched >= 1; }, false);
    WS::Counts c = counts();
    bool secondPlayed = true;
    if (first && c.sent >= c.matched)
        secondPlayed = PS::WaitFor("the sky's second sound", kEarthSec, [&] { return counts().matched >= 2; }, false);
    c = counts();
    const bool cueNotSent = first && c.sent < c.matched;   // decided: a sound was played here and no cue was sent for it
    PS::Mark("earth_final", std::to_string(c.sent));
    int received = 0, played = 0, earthTimeout = 0;
    if (!cueNotSent && first && secondPlayed) {
        std::string earth;
        if (!PS::AwaitText("earth_result", earth, kStepSec)) return;
        sscanf_s(earth.c_str(), "%d %d %d", &received, &played, &earthTimeout);
    }
    PS::Mark("host_done");

    // A cue not sent for a sound played here is decided; so is a cue received and not played (the client's play call runs in
    // the task that receives it). What did not come in its bound is INVALID.
    if (cueNotSent) {
        RV::Say(kTag, RV::Outcome::Fail, "earth: the sky played %u sounds here and the host sent %u cues", c.matched, c.sent);
        return;
    }
    const bool invalid = !fired || !cApplied || !mineNoise || !cNoise || !first || !secondPlayed || earthTimeout;
    if (invalid) {
        RV::Say(kTag, RV::Outcome::Invalid,
                "an event did not come inside its bound: alien fired=%d, dispatched on the client=%d, a noise on the host=%d and on "
                "the client=%d; the sky's sounds played here (first=%d, second=%d); the client's cues received in time=%d",
                fired ? 1 : 0, cApplied, mineNoise ? 1 : 0, cNoise, first ? 1 : 0, secondPlayed ? 1 : 0, earthTimeout ? 0 : 1);
        return;
    }
    const bool pass = c.sent == 2 && received == 2 && played == 2;
    RV::Say(kTag, pass ? RV::Outcome::Pass : RV::Outcome::Fail,
            "faucet: both presses crossed and clicked; alien: three noisers on the host (%zu) and on the client (%zu); earth: "
            "the sky played %u sounds here, the host sent %u cues, the client received %d and played %d",
            mineMost, cMost, c.matched, c.sent, received, played);
    UE_LOGI("world-sound: DONE on host");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    PS::Mark("client_ready");
    std::string key;
    if (!PS::AwaitText("faucet_key", key, kStepSec)) return;
    UE_LOGI("world-sound: client faucet key='%hs'", SpawnFaucet(key).c_str());
    if (!WaitListed("CLIENT")) return;
    PS::Mark("client_faucet");
    std::string start;
    if (!PS::AwaitText("f1_go", start, kStepSec)) return;
    const bool on = start == "1";
    PressFaucet("CLIENT");
    PS::Mark("f1_done");

    // The host presses; this peer's copy must flip and click.
    if (!PS::AwaitText("f2_go", start, kStepSec)) return;
    if (start == "skip") {   // the host decided at the first press
        UE_LOGI("world-sound: DONE on client (the host's verdict is said)");
        return;
    }
    const uint32_t clicks0 = Clicks();
    const unsigned replays0 = Replays();
    PS::Mark("f2_armed");
    if (!PS::Await("f2_pressed", kStepSec)) return;
    PS::Mark("f2_result", Crossed(on, clicks0));

    // The alien sounds: the noisers are counted once the host's event has dispatched here, and a noise is waited for. The
    // host skips this step when a verdict is already decided.
    std::string go;
    if (!PS::AwaitText("alien_go", go, kStepSec)) return;
    if (go != "skip") {
        const bool applied = PS::WaitFor("the host's alien sounds dispatched here", kAlienSec, [replays0] { return Replays() > replays0; }, false);
        const size_t most = applied ? NoisersNow() : 0;
        PS::Mark("alien_count", "most=" + std::to_string(most) + " applied=" + (applied ? "1" : "0"));   // the count first: it decides
        const bool noise = applied && WaitNoise("host_done");
        std::string verdictSaid;
        if (!noise && PS::Peek("host_done", verdictSaid)) {   // the host said its verdict at the count: no later leg is run
            UE_LOGI("world-sound: DONE on client (the host's verdict is said)");
            return;
        }
        PS::Mark("alien_result", std::string("noise=") + (noise ? "1" : "0"));
    }

    // The sky's tp: the host says how many cues it sent once its sounds are played (none, in the red, and then nothing can
    // arrive); each cue sent is received here, and a bound passing without it is INVALID (timeout=1). Its play call runs in
    // the task that receives it, so whether it played is final when it is received, and is judged by the host.
    if (!PS::Await("earth_go", kStepSec)) return;
    std::string fin;
    if (!PS::AwaitText("earth_final", fin, kEarthSec + 60)) return;
    const uint32_t sent = static_cast<uint32_t>(std::atoi(fin.c_str()));
    bool arrived = true;
    if (sent > 0)
        arrived = PS::WaitFor("the host's cues received here", kCueSec, [sent] {
            uint32_t received = 0;
            OnGameThread([&] { received = WS::GetCounts().received; });
            return received >= sent;
        }, false);
    WS::Counts c;
    OnGameThread([&] { c = WS::GetCounts(); });
    UE_LOGI("world-sound: CLIENT the sky's sounds: the host sent %u, received=%u played=%u", sent, c.received, c.played);
    PS::Mark("earth_result", std::to_string(c.received) + " " + std::to_string(c.played) + " " + std::to_string(arrived ? 0 : 1));
    PS::Await("host_done", kStepSec);
    UE_LOGI("world-sound: DONE on client");
}

// ---- join: a client that arrives while the host's noisers are out ----

void RunJoinHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    const bool fired = coop::event_fire_sync::HostFire(coop::event_fire_sync::FireKind::SpecialEvent, L"alienSounds", L"None");
    UE_LOGI("world-sound: the alien sounds fired=%d", fired ? 1 : 0);
    if (!fired || NoisersNow() < kNoisers) {
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
    // The host's counter lives as long as its session, across both of the client's lives: the join's own snapshots are
    // the count at the ready edge less the count when life 1 was gone, before the new life's world was up.
    uint32_t snapshots0 = 0;
    OnGameThread([&] { snapshots0 = coop::event_active_sync::JoinSnapshotsSent("alienSounds"); });
    size_t atEdge = 0;
    uint32_t snapshots = 0;
    if (!PS::WaitFor("the rejoined client's world ready", 400, [life1Gen, snapshots0, &atEdge, &snapshots] {
            bool ready = false;
            OnGameThread([&] {
                auto& s = harness::session_runtime::Session();
                ready = s.IsSlotWorldReady(1) && s.peerGenerationForSlot(1) != life1Gen;
                // The connect replay ran in the edge's own task.
                if (ready) snapshots = coop::event_active_sync::JoinSnapshotsSent("alienSounds") - snapshots0;
            });
            if (ready) atEdge = NoisersNow();
            return ready;
        }))
        return;
    UE_LOGI("world-sound: HOST the rejoined client's world is up; the host has %zu noisers out and sent %u snapshot(s)", atEdge,
            snapshots);
    PS::Mark("replay_decision", std::to_string(snapshots));
    if (atEdge == 0) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host had no noisers out at the client's ready edge: nothing to replay");
        return;
    }
    // The host's decision is made at the edge: noisers out and no event told to the joiner cannot change later.
    if (snapshots == 0) {
        RV::Say(kTagJoin, RV::Outcome::Fail, "the host had %zu noisers out at the client's ready edge and sent the joiner no event",
                atEdge);
        return;
    }
    std::string theirs;
    if (!PS::AwaitText("rejoin_done", theirs, kAlienSec + 60)) return;
    size_t most = 0;
    int timedOut = 0;
    if (sscanf_s(theirs.c_str(), "most=%zu timeout=%d", &most, &timedOut) != 2 || timedOut) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host's event was not dispatched on the rejoined client in time ('%s')",
                theirs.c_str());
        return;
    }
    RV::Say(kTagJoin, (most >= atEdge && most <= kNoisers) ? RV::Outcome::Pass : RV::Outcome::Fail,
            "the host had %zu noisers out at the client's ready edge and sent %u snapshot(s); the rejoined client had %zu around it",
            atEdge, snapshots, most);
    UE_LOGI("world-sound: JOIN DONE on host");
}

void RunJoinClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
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
    // The host says at the edge how many events it sent: none, and nothing can arrive, so the noisers are counted at once; some,
    // and its dispatch here is the event, after which the noisers are counted at once (they spawn inside it). A bound passing
    // without the dispatch is INVALID (timeout=1).
    std::string decision;
    if (!PS::AwaitText("replay_decision", decision, kStepSec)) return;
    const bool sent = decision != "0";
    const bool arrived = !sent || PS::WaitFor("the host's alien sounds dispatched here", kAlienSec, [] { return Replays() >= 1; }, false);
    const size_t most = NoisersNow();
    UE_LOGI("world-sound: REJOIN the noisers around this player: %zu (the host sent %s)", most, sent ? "an event" : "none");
    PS::Mark("rejoin_done", "most=" + std::to_string(most) + " timeout=" + std::to_string(arrived ? 0 : 1));
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
