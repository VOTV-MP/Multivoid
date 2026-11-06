// coop/dev/cremator_drill.cpp -- see coop/dev/cremator_drill.h.

#include "coop/dev/cremator_drill.h"

#include "cremator_dev_support.h"  // co-located private header (src tree, not include/)

#include "harness/autotest.h"  // IsClientRole
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/cremator_client_guard.h"  // G5: the refused collections
#include "coop/interactables/cremator_occupancy.h"  // the burn volume's begin-overlap: the landing
#include "coop/interactables/interactable_sync.h"  // the cremator lane's key and its resolve
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/player_damage.h"  // g3low: the refusals of the burn loop's achievement
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::dev::cremator_drill {
namespace {

namespace C  = ue_wrap::cremator;
namespace E  = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
namespace sg = ue_wrap::script_gate;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

// Bounds on waits for a game state. A handle's travel is 1 to 3 s a leg, so a copy shows a run within about twice
// that plus the round trip; the bounds are generous and a timeout is the answer FAIL, never a clock to reach a state.
constexpr int kJoinSec    = 480;
constexpr int kSignalSec  = 240;
constexpr int kHostRunSec = 90;
constexpr int kCopySec    = 40;

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

// The cremator this peer's lane indexes under `key`; game thread.
void* Resolve(const std::wstring& key) { return coop::interactable_sync::ResolveCremator(key); }

// One reading of a cremator's copy, as signalled: -1 for a flag that did not read. Game thread.
struct Reading { int active = -1, closed = -1, anim = -1; };

Reading ReadCopy(const std::wstring& key) {
    Reading r;
    void* c = Resolve(key);
    bool v = false;
    if (c && C::ReadActive(c, v)) r.active = v ? 1 : 0;
    if (c && C::ReadClosed(c, v)) r.closed = v ? 1 : 0;
    if (c && C::ReadAnim(c, v)) r.anim = v ? 1 : 0;
    return r;
}

std::string Text(const Reading& r) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "active=%d closed=%d anim=%d", r.active, r.closed, r.anim);
    return buf;
}

// The value after `name` in a signalled reading; -1 when the field is absent.
int FieldOf(const std::string& text, const char* name) {
    const size_t at = text.find(name);
    return at == std::string::npos ? -1 : static_cast<int>(std::strtol(text.c_str() + at + std::strlen(name), nullptr, 10));
}

Reading Parse(const std::string& text) {
    Reading r;
    r.active = FieldOf(text, "active=");
    r.closed = FieldOf(text, "closed=");
    r.anim = FieldOf(text, "anim=");
    return r;
}

// A copy at rest in the wanted state: the flag, and the handle not moving.
bool CopyAt(const std::wstring& key, int active) {
    bool ok = false;
    OnGameThread([&] {
        const Reading r = ReadCopy(key);
        ok = r.active == active && r.anim == 0;
    });
    return ok;
}

void Verdict(bool ok, const char* arm, const std::string& what) {
    if (!ok) ++g_fails;
    UE_LOGI("cremator-drill: VERDICT %hs %hs -- %hs", ok ? "PASS" : "FAIL", arm, what.c_str());
}

// The world is up and the host's puppet exists here.
bool HostPuppetUp() {
    if (!PS::WaitFor("the announced world", kJoinSec, [] {
            bool ready = false;
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            return ready;
        }))
        return false;
    return PS::WaitFor("the host's puppet", 60, [] {
        bool have = false;
        OnGameThread([&] {
            coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
            have = p && p->valid();
        });
        return have;
    });
}

// Waits for this peer's lane to resolve the cremator `key` names.
bool CopyResolves(const std::wstring& key) {
    return PS::WaitFor("the cremator the host named", 120, [&] {
        void* c = nullptr;
        OnGameThread([&] { c = Resolve(key); });
        return c != nullptr;
    });
}

// ---- HOST ------------------------------------------------------------------------------------------------

// Shuts the door and pulls the lever on the host's copy, the machine's own verb; game thread.
bool Pull(const std::wstring& key) {
    bool ran = false, wasOpened = false;
    OnGameThread([&] {
        void* c = Resolve(key);
        ran = c && C::ShutDoor(c, wasOpened) && C::CallUseLever(c);
    });
    UE_LOGI("cremator-drill: HOST pulled the lever ran=%d doorWasOpened=%d", ran ? 1 : 0, wasOpened ? 1 : 0);
    return ran;
}

void RunHost() {
    if (!Await("client_ready", kJoinSec)) return;
    // The cremator nearest the host's player; both peers name it by the lane's key.
    std::wstring key;
    PS::WaitFor("the host's lane indexing a cremator", 60, [&] {
        OnGameThread([&] {
            void* local = coop::players::Registry::Get().Local();
            ue_wrap::FVector at{};
            if (!local || !E::TryGetActorLocation(local, at)) return;
            const C::Nearest n = C::FindNearestFirebox(at);
            key = n.actor ? coop::interactable_sync::CrematorKey(n.actor) : std::wstring();
        });
        return !key.empty();
    }, false);
    if (key.empty()) {
        UE_LOGW("cremator-drill: VERDICT ABORT -- the host's cremator lane indexes no cremator");
        return;
    }
    UE_LOGI("cremator-drill: HOST cremator key='%ls'", key.c_str());
    if (!PS::WaitFor("the host's handle at rest, run off", kHostRunSec, [&] { return CopyAt(key, 0); })) return;
    Mark("key", Narrow(key));

    // G1.
    if (!Pull(key)) { UE_LOGW("cremator-drill: VERDICT ABORT -- the host's lever did not run"); return; }
    if (!PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); })) return;
    Mark("g1_on");
    std::string on;
    if (!AwaitText("g1_on_seen", on, kSignalSec)) return;
    const Reading ron = Parse(on);
    Verdict(ron.active == 1 && ron.closed == 1, "g1-on", "the client's copy after the host's pull: " + on);

    if (!Pull(key)) { UE_LOGW("cremator-drill: VERDICT ABORT -- the host's lever did not run"); return; }
    if (!PS::WaitFor("the host's run off", kHostRunSec, [&] { return CopyAt(key, 0); })) return;
    Mark("g1_off");
    std::string off;
    if (!AwaitText("g1_off_seen", off, kSignalSec)) return;
    const Reading roff = Parse(off);
    Verdict(roff.active == 0, "g1-off", "the client's copy after the host's pull back: " + off);
    UE_LOGI("cremator-drill: G1 DONE fails=%d", g_fails);

    // G4. The run stays on; the client leaves, and comes back into it.
    if (!Pull(key)) { UE_LOGW("cremator-drill: VERDICT ABORT -- the host's lever did not run"); return; }
    if (!PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); })) return;
    // Life 1's occupancy of slot 1: the wait is for it to change, never for the slot to read closed -- a fast rejoin
    // closes and refills the slot between two reads.
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    Mark("g4_on");
    std::string seen;
    if (!AwaitText("client_done", seen, kSignalSec)) return;
    UE_LOGI("cremator-drill: HOST the client's first life saw the run: %s", seen.c_str());
    Mark("host_ack");  // now watching slot 1: life 1 may end
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    // The second life leaves rejoin_ready once its world is up, and only then is the key written, so it reads it as
    // written since its own start.
    if (!Await("rejoin_ready", 400)) return;
    Mark("e_key", Narrow(key));
    std::string again;
    if (!AwaitText("rejoin_done", again, 400)) return;
    const Reading rj = Parse(again);
    Verdict(rj.active == 1 && rj.closed == 1, "g4", "the rejoined client's copy after its world-ready: " + again);
    UE_LOGI("cremator-drill: DONE on host fails=%d", g_fails);
}

// ---- CLIENT ----------------------------------------------------------------------------------------------

// Waits for the copy at `active` and signals what it reads; a copy that never gets there is reported as it is, and
// the host judges it.
void ReportCopy(const std::wstring& key, int active, const char* signal) {
    PS::WaitFor("the copy at the host's state", kCopySec, [&] { return CopyAt(key, active); }, false);
    std::string text;
    OnGameThread([&] { text = Text(ReadCopy(key)); });
    UE_LOGI("cremator-drill: CLIENT copy %s", text.c_str());
    Mark(signal, text);
}

void RunClient() {
    if (!HostPuppetUp()) return;
    Mark("client_ready");
    std::string keyText;
    if (!AwaitText("key", keyText, kSignalSec)) return;
    const std::wstring key = Widen(keyText);
    if (!CopyResolves(key)) return;

    if (!Await("g1_on", kSignalSec)) return;
    ReportCopy(key, 1, "g1_on_seen");
    if (!Await("g1_off", kSignalSec)) return;
    ReportCopy(key, 0, "g1_off_seen");
    UE_LOGI("cremator-drill: G1 DONE on client");

    if (!Await("g4_on", kSignalSec)) return;
    ReportCopy(key, 1, "client_done");
    // The DONE line is the rig's cue to kill this life: printed only once the host watches slot 1.
    Await("host_ack", 60);
    // A killed process only stops sending, and the host learns of it when the next life connects: life 1 leaves as a
    // player's quit does (network first), so the host sees slot 1 empty at once.
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "cremator drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
    UE_LOGI("cremator-drill: DONE on client");
}

void RunRejoin() {
    if (!HostPuppetUp()) return;
    Mark("rejoin_ready");
    std::string keyText;
    if (!AwaitText("e_key", keyText, 120)) return;
    const std::wstring key = Widen(keyText);
    if (!CopyResolves(key)) return;
    ReportCopy(key, 1, "rejoin_done");
    UE_LOGI("cremator-drill: REJOIN DONE");
}

// ---- G3: a client in the host's running cremator dies by the machine ----------------------------------------------
// The host moves the client with the admin verb's own order (TeleportClient, the `/cremate` move under test), judges the
// landing by the cremator's own begin-overlap for the client's puppet, shuts the door and pulls the lever. The client
// reports its own death, and the host judges it. g3low sets the host's own health under 10 before the lever: the burn
// loop's achievement call then runs on the host with a puppet as its actor, and the host must refuse it.

namespace CS = coop::dev::cremator_support;

constexpr uint8_t kClientSlot = 1;
constexpr int kLandSec  = 60;
constexpr int kDeathSec = 240;
constexpr float kLowHostHealth = 5.f;  // under the burn loop's 10-health test

std::atomic<void*> g_g3Cremator{nullptr};
std::atomic<void*> g_g3Puppet{nullptr};
std::atomic<int>   g_g3Landed{0};
float              g_hostHealthWas = -1.f;  // the host's health before BurnRun set it; the drill thread only

// The cremator's begin-overlap: the client's puppet entering the cremator the burn was sent to.
void OnLanding(void* cremator, void* other) {
    if (other && cremator == g_g3Cremator.load(std::memory_order_acquire) &&
        other == g_g3Puppet.load(std::memory_order_acquire))
        g_g3Landed.store(1, std::memory_order_release);
}

// The nearest cremator to the client's puppet gets its door opened, the overlap watch and the teleport order. Game
// thread. The cremator's lane key on success, "" on a step that failed (said).
std::wstring SendClientToFirebox(void*& cremator) {
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(kClientSlot);
    ue_wrap::FVector from{};
    if (!puppet || !puppet->TryGetLocation(from)) { UE_LOGW("cremator-drill: g3 the client's puppet has no location"); return {}; }
    const C::Nearest n = C::FindNearestFirebox(from);
    if (!n.actor) { UE_LOGW("cremator-drill: g3 no cremator with a firebox is loaded"); return {}; }
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorRotation(n.actor, rot)) { UE_LOGW("cremator-drill: g3 the cremator's rotation is unreadable"); return {}; }
    cremator = n.actor;
    const std::wstring key = coop::interactable_sync::CrematorKey(cremator);
    if (key.empty()) { UE_LOGW("cremator-drill: g3 the cremator lane does not index the nearest cremator"); return {}; }
    g_g3Cremator.store(cremator, std::memory_order_release);
    g_g3Puppet.store(puppet->GetActor(), std::memory_order_release);
    g_g3Landed.store(0, std::memory_order_release);
    if (!coop::cremator_occupancy::AddListener(&OnLanding)) {
        UE_LOGW("cremator-drill: g3 the begin-overlap watch did not register");
        return {};
    }
    bool doorOpened = false;
    if (!C::OpenDoor(cremator, doorOpened)) { UE_LOGW("cremator-drill: g3 the door's open() did not run"); return {}; }
    coop::net::TeleportClientPayload pose{};
    pose.locX = n.at.X; pose.locY = n.at.Y; pose.locZ = n.at.Z;
    pose.rotPitch = rot.Pitch; pose.rotYaw = rot.Yaw; pose.rotRoll = rot.Roll;
    if (!harness::session_runtime::Session().SendReliableToSlot(kClientSlot, coop::net::ReliableKind::TeleportClient, &pose,
                                                                sizeof(pose))) {
        UE_LOGW("cremator-drill: g3 the teleport order was not sent");
        return {};
    }
    UE_LOGI("cremator-drill: HOST g3 door opened=%d, teleport order to=(%.0f,%.0f,%.0f) key='%ls'", doorOpened ? 1 : 0,
            n.at.X, n.at.Y, n.at.Z, key.c_str());
    return key;
}

// The burn's host half, shared by the arms that put the client in the firebox: sends the client there, judges the
// landing by the cremator's own begin-overlap, shuts the door and pulls the lever. `keyMark`, when set, leaves the
// cremator's key for the client as soon as the order is out; `leverMark` is left once the lever ran. A `hostHealth`
// of 0 or more is written to the host's own health once the client has landed, before the lever. False on an abort
// (said).
bool BurnRun(const char* arm, const char* keyMark, const char* leverMark, float hostHealth = -1.f) {
    if (!Await("client_ready", kJoinSec)) return false;
    if (!PS::WaitFor("slot 1's join settled", kSignalSec, [] {
            bool ok = false;
            OnGameThread([&] { ok = CS::SlotSettled(harness::session_runtime::Session(), kClientSlot); });
            return ok;
        }))
        return false;
    void* cremator = nullptr;
    std::wstring key;
    OnGameThread([&] { key = SendClientToFirebox(cremator); });
    if (key.empty()) { UE_LOGW("cremator-drill: VERDICT ABORT -- the host could not send the client to the firebox"); return false; }
    if (keyMark) Mark(keyMark, Narrow(key));
    if (!PS::WaitFor("the cremator's overlap for the client's puppet", kLandSec,
                     [] { return g_g3Landed.load(std::memory_order_acquire) == 1; }))
        return false;
    if (hostHealth >= 0.f) {
        bool wrote = false;
        float was = -1.f;
        OnGameThread([&] {
            ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &was);
            wrote = ue_wrap::vitals::Write(ue_wrap::vitals::Field::Health, hostHealth);
        });
        UE_LOGI("cremator-drill: HOST %hs set its own health to %.0f (was %.1f) ok=%d", arm, hostHealth, was, wrote ? 1 : 0);
        if (!wrote) { UE_LOGW("cremator-drill: VERDICT ABORT -- the host's health write did not run"); return false; }
        g_hostHealthWas = was;
    }
    bool ran = false, wasOpened = false;
    OnGameThread([&] { ran = C::ShutDoor(cremator, wasOpened) && C::CallUseLever(cremator); });
    UE_LOGI("cremator-drill: HOST %hs pulled the lever ran=%d doorWasOpened=%d", arm, ran ? 1 : 0, wasOpened ? 1 : 0);
    if (!ran) { UE_LOGW("cremator-drill: VERDICT ABORT -- the host's lever did not run"); return false; }
    Mark(leverMark);
    return true;
}

// g3: the client dies by the machine. g3low: the same, with the host's health under 10, and the host must also have
// refused the burn loop's achievement call for the puppet (player_damage's count of refusals went up).
void RunHostG3(bool low) {
    const char* arm = low ? "g3low" : "g3";
    const uint32_t refused0 = coop::player_damage::RefusedAchievements();
    if (!BurnRun(arm, nullptr, "g3_lever", low ? kLowHostHealth : -1.f)) return;
    std::string text;
    if (!AwaitText("g3_dead", text, kDeathSec + 60)) return;
    const int dead = FieldOf(text, "dead=");
    const int seconds = FieldOf(text, "T=");
    const uint32_t refused = coop::player_damage::RefusedAchievements() - refused0;
    char what[200];
    if (dead == 1) std::snprintf(what, sizeof(what), "the client died by the machine T=%d", seconds);
    else std::snprintf(what, sizeof(what), "the client survived the host's run: %s", text.c_str());
    if (low) {
        const size_t used = std::strlen(what);
        std::snprintf(what + used, sizeof(what) - used, "; the host refused the cremator achievement %u time(s) (1 or more wanted)",
                      refused);
        OnGameThread([] { ue_wrap::vitals::Write(ue_wrap::vitals::Field::Health, g_hostHealthWas); });
    }
    Verdict(dead == 1 && (!low || refused >= 1), arm, what);
    UE_LOGI("cremator-drill: %hs DONE fails=%d", low ? "G3LOW" : "G3", g_fails);
}

// This player is dead or has no health left. Game thread.
bool PlayerDead(float& health) {
    float dead = 0.f;
    const bool readDead = ue_wrap::vitals::Read(ue_wrap::vitals::Field::Dead, &dead);
    const bool readHealth = ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &health);
    return (readDead && dead > 0.5f) || (readHealth && health <= 0.f);
}

void RunClientG3() {
    if (!HostPuppetUp()) return;
    Mark("client_ready");
    if (!Await("g3_lever", kSignalSec)) return;
    const auto since = std::chrono::steady_clock::now();
    bool dead = false;
    float health = -1.f;
    PS::WaitFor("this player's death", kDeathSec, [&] {
        OnGameThread([&] { dead = PlayerDead(health); });
        return dead;
    }, false);
    const int seconds = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - since).count());
    char text[64];
    std::snprintf(text, sizeof(text), "dead=%d health=%.0f T=%d", dead ? 1 : 0, health, seconds);
    UE_LOGI("cremator-drill: CLIENT g3 %s", text);
    Mark("g3_dead", text);
    UE_LOGI("cremator-drill: DONE on client");
}

// ---- G5: a client's copy of the cremator acts on nothing ----------------------------------------------------------
// The burn of G3 with the client's own copy watched: the client dies by the host's machine, its copy's `actors` reads 0
// at every look (the guard refused each collection) and the guard's count of refused collections is at least one. The
// fireballs the client's copy spawns are counted and reported, not judged (M-i).

// A fireball's birth, on the client: the copy's tick spawns one while the copy is active with its door open.
UE_FACT_FN(kRowFireballBegin, "dev.cremator_drill", L"prop_crematorFireball_C", L"ReceiveBeginPlay", Declared);
constexpr int kTagFireball = 0x43443546;  // 'CD5F'

std::atomic<int> g_fireballs{0};

void OnFireballPost(const sg::Call&) { g_fireballs.fetch_add(1, std::memory_order_relaxed); }

void RunHostG5() {
    if (!BurnRun("g5", "g5_key", "g5_lever")) return;
    std::string text;
    if (!AwaitText("g5_dead", text, kDeathSec + 60)) return;
    const int dead = FieldOf(text, "dead=");
    const int actors = FieldOf(text, "actors=");
    const int refused = FieldOf(text, "refused=");
    const int fireballs = FieldOf(text, "fireballs=");
    char what[240];
    std::snprintf(what, sizeof(what),
                  "dead=%d (1 wanted), the client copy's max actors=%d (0 wanted), addObject refused=%d (1 or more wanted), "
                  "fireball births=%d (reported, not judged)", dead, actors, refused, fireballs);
    Verdict(dead == 1 && actors == 0 && refused >= 1, "g5", what);
    UE_LOGI("cremator-drill: G5 DONE fails=%d", g_fails);
}

void RunClientG5() {
    if (!HostPuppetUp()) return;
    OnGameThread([] {
        if (!sg::WatchName(kRowFireballBegin, kTagFireball, nullptr, &OnFireballPost))
            UE_LOGW("cremator-drill: g5 the fireball birth watch did not register");
    });
    Mark("client_ready");
    std::string keyText;
    if (!AwaitText("g5_key", keyText, kSignalSec)) return;
    const std::wstring key = Widen(keyText);
    if (!CopyResolves(key)) return;
    // From the host's order on, every look at the copy's `actors` is kept: a collection that was not refused leaves the
    // sitter in it until the burn ends.
    int maxActors = -1;
    bool dead = false;
    float health = -1.f;
    const auto since = std::chrono::steady_clock::now();
    PS::WaitFor("this player's death", kDeathSec, [&] {
        OnGameThread([&] {
            dead = PlayerDead(health);
            if (void* c = Resolve(key)) {
                const int n = C::ReadMachine(c).actors;
                if (n > maxActors) maxActors = n;
            }
        });
        return dead;
    }, false);
    const int seconds = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - since).count());
    unsigned long long refused = 0;
    OnGameThread([&] { refused = coop::cremator_client_guard::AddObjectRefused(); });
    char text[128];
    std::snprintf(text, sizeof(text), "dead=%d health=%.0f waited=%d actors=%d refused=%llu fireballs=%d", dead ? 1 : 0,
                  health, seconds, maxActors, refused, g_fireballs.load(std::memory_order_relaxed));
    UE_LOGI("cremator-drill: CLIENT g5 %s", text);
    Mark("g5_dead", text);
    UE_LOGI("cremator-drill: DONE on client");
}

}  // namespace

DWORD WINAPI Thread(LPVOID /*arg*/) {
    if (!PS::Begin("cremator-drill")) return 0;
    // `g3`, `g3low` and `g5` run the burn arms alone; `1` runs the lever arms.
    const std::string arm = coop::config::ResolveEnum(coop::config_registry::rows::cremator_drill);
    if (arm == "g3" || arm == "g3low") {
        if (harness::autotest::IsClientRole()) RunClientG3(); else RunHostG3(arm == "g3low");
        return 0;
    }
    if (arm == "g5") {
        if (harness::autotest::IsClientRole()) RunClientG5(); else RunHostG5();
        return 0;
    }
    // The rejoined life carries the env the rig sets for it alone (--rejoin-env). The first lives cannot know whether
    // the rig rejoins, so both go on to G4 after G1; a run without --rejoin ends at the host's 'G1 DONE' line.
    if (!harness::autotest::IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_CREMATOR_DRILL_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace coop::dev::cremator_drill
