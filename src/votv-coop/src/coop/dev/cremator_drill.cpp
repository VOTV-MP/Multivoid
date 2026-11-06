// coop/dev/cremator_drill.cpp -- see coop/dev/cremator_drill.h.

#include "coop/dev/cremator_drill.h"

#include "cremator_dev_support.h"  // co-located private header (src tree, not include/)
#include "cremator_drill_g2.h"     // the G2 arms' file, and what it shares with this one

#include "harness/autotest.h"  // IsClientRole
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/commands/command_sync.h"  // the /cremate arms ask through the real dispatcher
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/cremator_client_guard.h"  // G5: the refused collections
#include "coop/interactables/cremator_occupancy.h"  // the burn volume's begin-overlap: the landing
#include "coop/interactables/interactable_sync.h"  // the cremator lane's key and its resolve
#include "coop/net/end_reason.h"  // Describe: the id the client's notice carries
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/player_damage.h"  // g3low: the refusals of the burn loop's achievement
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // the player number a command line names a seat by
#include "coop/session/join_progress.h"  // the client's end-reason notice
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/session/teleport_client.h"  // OrderToPose: the order that sends the client to the firebox

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace coop::dev::cremator_drill {
// What cremator_drill_g2.cpp shares (declared in cremator_drill_g2.h). A handle's travel is 1 to 3 s a leg, so a copy
// shows a run within about twice that plus the round trip; the header's bounds are generous.
namespace detail {

namespace C  = ue_wrap::cremator;
namespace E  = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

namespace {
int g_fails = 0;
}  // namespace

int Fails() { return g_fails; }

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (const wchar_t c : w) s.push_back(static_cast<char>(c));  // a lane key is ASCII
    return s;
}

void* Resolve(const std::wstring& key) { return coop::interactable_sync::ResolveCremator(key); }

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

// The drill could not measure: the ABORT line, and the rig's own verdict line, so a peer that would otherwise be waited
// on stops the run at once.
void Abort(const char* fmt, ...) {
    char why[300];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(why, sizeof(why), fmt, args);
    va_end(args);
    UE_LOGW("cremator-drill: VERDICT ABORT -- %s", why);
    coop::rig_verdict::Say("cremator-drill", coop::rig_verdict::Outcome::Invalid, "%s", why);
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

bool CopyResolves(const std::wstring& key) {
    return PS::WaitFor("the cremator the host named", 120, [&] {
        void* c = nullptr;
        OnGameThread([&] { c = Resolve(key); });
        return c != nullptr;
    });
}

// The cremator nearest the host's player; both peers name it by the lane's key.
std::wstring HostFindKey() {
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
        Abort("the host's cremator lane indexes no cremator");
        return key;
    }
    UE_LOGI("cremator-drill: HOST cremator key='%ls'", key.c_str());
    return key;
}

void ReportCopy(const std::wstring& key, int active, const char* signal) {
    PS::WaitFor("the copy at the host's state", kCopySec, [&] { return CopyAt(key, active); }, false);
    std::string text;
    OnGameThread([&] { text = Text(ReadCopy(key)); });
    UE_LOGI("cremator-drill: CLIENT copy %s", text.c_str());
    Mark(signal, text);
}

}  // namespace detail

namespace {

using namespace detail;
namespace sg = ue_wrap::script_gate;

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
    const std::wstring key = HostFindKey();
    if (key.empty()) return;
    if (!PS::WaitFor("the host's handle at rest, run off", kHostRunSec, [&] { return CopyAt(key, 0); })) return;
    Mark("key", Narrow(key));

    // G1.
    if (!Pull(key)) { Abort("the host's lever did not run"); return; }
    if (!PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); })) return;
    Mark("g1_on");
    std::string on;
    if (!AwaitText("g1_on_seen", on, kSignalSec)) return;
    const Reading ron = Parse(on);
    Verdict(ron.active == 1 && ron.closed == 1, "g1-on", "the client's copy after the host's pull: " + on);

    if (!Pull(key)) { Abort("the host's lever did not run"); return; }
    if (!PS::WaitFor("the host's run off", kHostRunSec, [&] { return CopyAt(key, 0); })) return;
    Mark("g1_off");
    std::string off;
    if (!AwaitText("g1_off_seen", off, kSignalSec)) return;
    const Reading roff = Parse(off);
    Verdict(roff.active == 0, "g1-off", "the client's copy after the host's pull back: " + off);
    UE_LOGI("cremator-drill: G1 DONE fails=%d", Fails());

    // G4. The run stays on; the client leaves, and comes back into it.
    if (!Pull(key)) { Abort("the host's lever did not run"); return; }
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
    UE_LOGI("cremator-drill: DONE on host fails=%d", Fails());
}

// ---- CLIENT ----------------------------------------------------------------------------------------------

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
constexpr float kLowHealth = 5.f;  // under the burn loop's 10-health test, the host's (g3low) and the client's (cremate-low)

std::atomic<void*> g_g3Cremator{nullptr};
std::atomic<void*> g_g3Puppet{nullptr};
std::atomic<int>   g_g3Landed{0};
float              g_hostHealthWas = -1.f;  // the host's health before BurnRun set it; the drill thread only

// The cremator's begin-overlap: the client's puppet entering the cremator the burn was sent to.
void OnLanding(void* cremator, void* other) {
    if (other && cremator == g_g3Cremator.load(std::memory_order_acquire) &&
        other == g_g3Puppet.load(std::memory_order_acquire)) {
        if (g_g3Landed.exchange(1, std::memory_order_acq_rel) == 0) UE_LOGI("cremator-drill: HOST LANDED");
    }
}

// The cremator nearest the client's puppet is the one its landing is watched on (the choice the /cremate verb makes
// too): remembered with the puppet, and the begin-overlap listener added. Game thread; false on a step that failed (said).
bool WatchLanding(C::FireboxMove& move) {
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(kClientSlot);
    ue_wrap::FVector from{};
    if (!puppet || !puppet->TryGetLocation(from)) { UE_LOGW("cremator-drill: g3 the client's puppet has no location"); return false; }
    const C::MoveResult found = C::FindFireboxMove(from, move);
    if (found == C::MoveResult::NoCremator) { UE_LOGW("cremator-drill: g3 no cremator with a firebox is loaded"); return false; }
    if (found == C::MoveResult::RotationUnread) { UE_LOGW("cremator-drill: g3 the cremator's rotation is unreadable"); return false; }
    g_g3Cremator.store(move.actor, std::memory_order_release);
    g_g3Puppet.store(puppet->GetActor(), std::memory_order_release);
    g_g3Landed.store(0, std::memory_order_release);
    if (!coop::cremator_occupancy::AddListener(&OnLanding)) {
        UE_LOGW("cremator-drill: g3 the begin-overlap watch did not register");
        return false;
    }
    return true;
}

// Slot 1's join has settled. Waits for a game state; false on the abort line.
bool ClientSettled() {
    return PS::WaitFor("slot 1's join settled", kSignalSec, [] {
        bool ok = false;
        OnGameThread([&] { ok = CS::SlotSettled(harness::session_runtime::Session(), kClientSlot); });
        return ok;
    });
}

// The nearest cremator to the client's puppet gets its door opened, the overlap watch and the teleport order. Game
// thread. The cremator's lane key on success, "" on a step that failed (said).
std::wstring SendClientToFirebox(void*& cremator) {
    C::FireboxMove move;
    if (!WatchLanding(move)) return {};
    cremator = move.actor;
    const std::wstring key = coop::interactable_sync::CrematorKey(cremator);
    if (key.empty()) { UE_LOGW("cremator-drill: g3 the cremator lane does not index the nearest cremator"); return {}; }
    bool doorOpened = false;
    if (!C::OpenDoor(cremator, doorOpened)) { UE_LOGW("cremator-drill: g3 the door's open() did not run"); return {}; }
    if (!coop::teleport_client::OrderToPose(harness::session_runtime::Session(), kClientSlot, move.at, move.rot)) {
        UE_LOGW("cremator-drill: g3 the teleport order was not sent");
        return {};
    }
    UE_LOGI("cremator-drill: HOST g3 door opened=%d, teleport order to=(%.0f,%.0f,%.0f) key='%ls'", doorOpened ? 1 : 0,
            move.at.X, move.at.Y, move.at.Z, key.c_str());
    return key;
}

// The burn's host half, shared by the arms that put the client in the firebox: sends the client there, judges the
// landing by the cremator's own begin-overlap, shuts the door and pulls the lever. `keyMark`, when set, leaves the
// cremator's key for the client as soon as the order is out; `leverMark` is left once the lever ran. A `hostHealth`
// of 0 or more is written to the host's own health once the client has landed, before the lever. False on an abort
// (said).
bool BurnRun(const char* arm, const char* keyMark, const char* leverMark, float hostHealth = -1.f) {
    if (!Await("client_ready", kJoinSec)) return false;
    if (!ClientSettled()) return false;
    void* cremator = nullptr;
    std::wstring key;
    OnGameThread([&] { key = SendClientToFirebox(cremator); });
    if (key.empty()) { Abort("the host could not send the client to the firebox"); return false; }
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
        if (!wrote) { Abort("the host's health write did not run"); return false; }
        g_hostHealthWas = was;
    }
    bool ran = false, wasOpened = false;
    OnGameThread([&] { ran = C::ShutDoor(cremator, wasOpened) && C::CallUseLever(cremator); });
    UE_LOGI("cremator-drill: HOST %hs pulled the lever ran=%d doorWasOpened=%d", arm, ran ? 1 : 0, wasOpened ? 1 : 0);
    if (!ran) { Abort("the host's lever did not run"); return false; }
    Mark(leverMark);
    return true;
}

// g3: the client dies by the machine. g3low: the same, with the host's health under 10, and the host must also have
// refused the burn loop's achievement call for the puppet (player_damage's count of refusals went up).
void RunHostG3(bool low) {
    const char* arm = low ? "g3low" : "g3";
    const uint32_t refused0 = coop::player_damage::RefusedAchievements();
    if (!BurnRun(arm, nullptr, "g3_lever", low ? kLowHealth : -1.f)) return;
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
    UE_LOGI("cremator-drill: %hs DONE fails=%d", low ? "G3LOW" : "G3", Fails());
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
    UE_LOGI("cremator-drill: G5 DONE fails=%d", Fails());
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

// ---- /cremate: the verb, asked through the command dispatcher ------------------------------------------------------
// The host asks as the operator and judges the line the verb answers with, the burn volume's landing and, where the
// arm ends in a kick, the end reason the client reads. The refusal arms ask from client 1, a player the host's own /mv
// granted the node (cleared before, revoked at the end), and the observer is the one who holds the exempt node or only stands.

namespace CMD = coop::command_sync;

std::mutex               g_lineMu;
std::vector<std::string> g_lines;  // every reply line this peer's command transport delivered, in order

void ObserveLine(std::string_view line) {
    std::lock_guard<std::mutex> lk(g_lineMu);
    g_lines.emplace_back(line);
}

size_t LineCount() {
    std::lock_guard<std::mutex> lk(g_lineMu);
    return g_lines.size();
}

// The first delivered line containing `needle` ("" matches the first line); empty when there is none.
std::string LineWith(const char* needle) {
    std::lock_guard<std::mutex> lk(g_lineMu);
    for (const std::string& l : g_lines)
        if (l.find(needle) != std::string::npos) return l;
    return {};
}

// How the verb words its ends; each ends a trip with exactly one.
constexpr const char* kCremated = " was cremated.";
constexpr const char* kSurvived = " survived the crematorium.";
constexpr const char* kDied     = " died before landing.";
constexpr const char* kNoLand   = " did not land in the cremator.";
constexpr const char* kEnds[]   = {kCremated, kSurvived, " survived the crematorium (still burning).", " left before the end.", kDied,
                                    " died in the cremator, but not by its fire.", kNoLand, "would not start for",
                                    "The cremator is gone"};

std::string EndLine() {
    for (const char* e : kEnds) {
        const std::string l = LineWith(e);
        if (!l.empty()) return l;
    }
    return {};
}

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The player number a command line names the seat by. Game thread.
std::string PlayerNo(int slot) { return "#" + std::to_string(coop::roster_ledger::Get(slot).playerNo); }

void Done(const std::string& arm) { UE_LOGI("cremator-drill: %hs DONE fails=%d", Upper(arm).c_str(), Fails()); }

bool LowArm(const std::string& arm) { return arm == "cremate-low" || arm == "cremate-running-low"; }
bool RunningArm(const std::string& arm) { return arm == "cremate-running" || arm == "cremate-running-low"; }
bool EndsInKick(const std::string& arm) { return arm == "cremate" || RunningArm(arm) || LowArm(arm); }

// The arms that send the client to the crematorium. cremate-running starts the host's cremator first; cremate-survive
// pulls its lever back once the run is on. The red arm is cremate with the host's cremate_landing_off.
// cremate-low: the client starts under 10 health, so the machine's first burn tick ignites and kills it at once (the
// one pose carries both); it must still end cremated and kicked. cremate-running-low: the same into a cremator the host
// already runs, so the client may burn and die on the pose that lands it.
void RunHostCremate(const std::string& arm) {
    const bool kick = EndsInKick(arm);
    const char* want = kick ? kCremated : arm == "cremate-survive" ? kSurvived : arm == "cremate-transit" ? kDied : kNoLand;
    // The landing the arm wants: 1 judged landed, 0 judged not, -1 only reported (a player who dies on the order's
    // receipt does not move, but its landing is not judged here).
    const int wantLanded = kick || arm == "cremate-survive" ? 1 : arm == "cremate-dropped" ? 0 : -1;
    if (!Await("client_ready", kJoinSec) || !ClientSettled()) return;
    C::FireboxMove move;
    bool watching = false;
    std::wstring key;
    std::string no;
    OnGameThread([&] {
        CMD::SetReplyObserver(&ObserveLine);
        watching = WatchLanding(move);
        if (watching) key = coop::interactable_sync::CrematorKey(move.actor);
        no = PlayerNo(kClientSlot);
    });
    if (!watching || key.empty()) { Abort("the landing cannot be watched"); return; }
    // The verb refuses a trip whose landing cannot be seen, so the order waits for the watch to be live (the red arm's
    // watch is this drill's own listener, as the verb registers none).
    if (!PS::WaitFor("the burn volume's watch live", kSignalSec, [] { return coop::cremator_occupancy::WatchLive(); }))
        return;
    if (RunningArm(arm) && (!Pull(key) || !PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); })))
        return;
    CMD::Submit("cremate " + no);
    UE_LOGI("cremator-drill: HOST %hs dispatched 'cremate %hs' as the operator", arm.c_str(), no.c_str());
    if (arm == "cremate-survive") {
        if (!PS::WaitFor("the landing and the run", kLandSec + kHostRunSec,
                         [&] { return g_g3Landed.load(std::memory_order_acquire) == 1 && CopyAt(key, 1); }))
            return;
        bool ran = false;
        OnGameThread([&] { ran = C::CallUseLever(move.actor); });
        UE_LOGI("cremator-drill: HOST pulled the lever back ran=%d", ran ? 1 : 0);
        if (!ran) { Abort("the host's lever did not run"); return; }
    }
    if (!PS::WaitFor("the verb's end line", kDeathSec + 60, [] { return !EndLine().empty(); })) return;
    const std::string end = EndLine();
    Mark("cremate_over");
    std::string report;
    if (!AwaitText("cremate_end", report, kSignalSec)) return;
    const int landed = g_g3Landed.load(std::memory_order_acquire);
    const bool kicked = report.find("id=MV-H35") != std::string::npos;
    char what[400];
    std::snprintf(what, sizeof(what), "the end line '%s' (wanted '%s'), landed=%d (wanted %d, -1 = not judged), the client's report '%s' (MV-H35 %s)",
                  end.c_str(), want, landed, wantLanded, report.c_str(), kick ? "wanted" : "not wanted");
    Verdict(end.find(want) != std::string::npos && (wantLanded < 0 || landed == wantLanded) && kicked == kick, arm.c_str(), what);
    Done(arm);
}

void RunClientCremate(const std::string& arm) {
    if (!HostPuppetUp()) return;
    if (LowArm(arm)) {
        bool wrote = false;
        OnGameThread([&] { wrote = ue_wrap::vitals::Write(ue_wrap::vitals::Field::Health, kLowHealth); });
        UE_LOGI("cremator-drill: CLIENT %hs set its own health to %.0f ok=%d", arm.c_str(), kLowHealth, wrote ? 1 : 0);
        if (!wrote) { Abort("%s: the client's health did not write", arm.c_str()); return; }
    }
    Mark("client_ready");
    if (!Await("cremate_over", kDeathSec + 120)) return;
    // A kick lands moments after the verb's end line, and its notice stays until the dialog is acknowledged.
    coop::join_progress::Notice notice;
    if (EndsInKick(arm))
        PS::WaitFor("the host's end-reason notice", 30, [&] { return coop::join_progress::PeekNotice(notice); }, false);
    const char* id = coop::join_progress::PeekNotice(notice) ? coop::net::Describe(notice.code).id : "";
    const std::string text = std::string("id=") + id;
    UE_LOGI("cremator-drill: CLIENT %hs end reason %hs", arm.c_str(), text.c_str());
    Mark("cremate_end", text);
    UE_LOGI("cremator-drill: DONE on client");
}

// Removes the nodes the host granted, on every way out of the arm. Game thread posts through Submit.
struct Revoke {
    std::vector<std::string> lines;
    ~Revoke() {
        for (const std::string& l : lines) CMD::Submit(l);
    }
};

constexpr int kStillCm = 150;  // an observer nobody ordered stands where it did, give or take its own shuffling

// cremate-exempt / cremate-self / cremate-host: client 1 asks for the observer, for itself, or for the host.
void RunHostRefusal(const std::string& arm) {
    const bool exempt = arm == "cremate-exempt";
    if (!Await("a_ready", kJoinSec) || (exempt && !Await("b_ready", kJoinSec))) return;
    std::string a, b;
    OnGameThread([&] {
        CMD::SetReplyObserver(&ObserveLine);
        a = PlayerNo(1);
        b = PlayerNo(2);
    });
    std::vector<std::string> grants{"mv user " + a + " permission set multivoid.cremate true"};
    Revoke revoke;
    revoke.lines.push_back("mv user " + a + " permission unset multivoid.cremate");
    if (exempt) {
        grants.push_back("mv user " + b + " permission set multivoid.cremate.exempt true");
        revoke.lines.push_back("mv user " + b + " permission unset multivoid.cremate.exempt");
    }
    // A run that died before its own unset left its nodes in the host's permission files; both nodes of every
    // player the arm names are cleared first, through the dispatcher the grants use, so the arm starts from none.
    std::vector<std::string> clears;
    for (const std::string& who : exempt ? std::vector<std::string>{a, b} : std::vector<std::string>{a}) {
        clears.push_back("mv user " + who + " permission unset multivoid.cremate");
        clears.push_back("mv user " + who + " permission unset multivoid.cremate.exempt");
    }
    for (const std::string& c : clears) CMD::Submit(c);
    PS::WaitFor("the clears' replies", kLandSec, [&] { return LineCount() >= clears.size(); });
    for (const std::string& c : clears) UE_LOGI("cremator-drill: HOST cleared with '%hs'", c.c_str());
    const size_t cleared = LineCount();
    for (const std::string& g : grants) CMD::Submit(g);
    PS::WaitFor("the grants' replies", kLandSec, [&] { return LineCount() >= cleared + grants.size(); });
    for (const std::string& g : grants) UE_LOGI("cremator-drill: HOST granted with '%hs'", g.c_str());
    Mark("perm_ready");
    std::string asked, moved;
    if (!AwaitText("a_line", asked, kSignalSec)) return;
    Mark("cremate_over");
    if (exempt && !AwaitText("b_end", moved, kSignalSec)) return;
    const char* want = exempt ? " cannot be cremated." : arm == "cremate-self" ? "You cannot cremate yourself."
                                                                             : "That is the host -- it cannot be cremated.";
    const int movedCm = exempt ? FieldOf(moved, "moved=") : 0;
    char what[300];
    std::snprintf(what, sizeof(what), "client 1 was answered '%s' (wanted '%s'); the observer moved %d cm (%d or less wanted)",
                  asked.c_str(), want, movedCm, kStillCm);
    Verdict(asked.find(want) != std::string::npos && movedCm >= 0 && movedCm <= kStillCm, arm.c_str(), what);
    Done(arm);
}

void RunClientRefusal(const std::string& arm) {
    if (!HostPuppetUp()) return;
    if (coop::players::Registry::Get().LocalPeerId() == 1) {
        Mark("a_ready");
        if (!Await("perm_ready", kSignalSec)) return;
        std::string no;
        OnGameThread([&] {
            CMD::SetReplyObserver(&ObserveLine);
            no = PlayerNo(arm == "cremate-host" ? 0 : arm == "cremate-self" ? 1 : 2);
        });
        CMD::Submit("cremate " + no);
        if (!PS::WaitFor("the host's answer", kLandSec, [] { return LineCount() > 0; })) return;
        const std::string line = LineWith("");
        UE_LOGI("cremator-drill: CLIENT %hs asked 'cremate %hs' and was answered '%hs'", arm.c_str(), no.c_str(), line.c_str());
        Mark("a_line", line);
    } else {
        Mark("b_ready");
        ue_wrap::FVector was{}, now{};
        OnGameThread([&] { E::TryGetActorLocation(coop::players::Registry::Get().Local(), was); });
        if (!Await("cremate_over", kSignalSec)) return;
        OnGameThread([&] { E::TryGetActorLocation(coop::players::Registry::Get().Local(), now); });
        const float dx = now.X - was.X, dy = now.Y - was.Y, dz = now.Z - was.Z;
        char text[32];
        std::snprintf(text, sizeof(text), "moved=%.0f", std::sqrt(dx * dx + dy * dy + dz * dz));
        UE_LOGI("cremator-drill: OBSERVER %hs %hs", arm.c_str(), text);
        Mark("b_end", text);
    }
    UE_LOGI("cremator-drill: DONE on client");
}

}  // namespace

DWORD WINAPI Thread(LPVOID /*arg*/) {
    if (!PS::Begin("cremator-drill")) return 0;
    // `g3`, `g3low` and `g5` run the burn arms alone; `g2*` a client's handle press; `1` runs the lever arms.
    const std::string arm = coop::config::ResolveEnum(coop::config_registry::rows::cremator_drill);
    if (arm.rfind("cremate", 0) == 0) {
        const bool refusal = arm == "cremate-exempt" || arm == "cremate-self" || arm == "cremate-host";
        const bool client = harness::autotest::IsClientRole();
        if (refusal) { if (client) RunClientRefusal(arm); else RunHostRefusal(arm); }
        else if (client) RunClientCremate(arm);
        else RunHostCremate(arm);
        return 0;
    }
    if (arm == "g3" || arm == "g3low") {
        if (harness::autotest::IsClientRole()) RunClientG3(); else RunHostG3(arm == "g3low");
        return 0;
    }
    if (arm == "g5") {
        if (harness::autotest::IsClientRole()) RunClientG5(); else RunHostG5();
        return 0;
    }
    if (arm == "g2" || arm == "g2-double" || arm == "g2-far") {
        if (harness::autotest::IsClientRole()) RunClientG2(arm); else RunHostG2(arm);
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
