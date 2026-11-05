// coop/dev/cremator_drill.cpp -- see coop/dev/cremator_drill.h.

#include "coop/dev/cremator_drill.h"

#include "harness/autotest.h"  // IsClientRole
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/interactables/interactable_sync.h"  // the cremator lane's key and its resolve
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
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

}  // namespace

DWORD WINAPI Thread(LPVOID /*arg*/) {
    if (!PS::Begin("cremator-drill")) return 0;
    // The rejoined life carries the env the rig sets for it alone (--rejoin-env). The first lives cannot know whether
    // the rig rejoins, so both go on to G4 after G1; a run without --rejoin ends at the host's 'G1 DONE' line.
    if (!harness::autotest::IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_CREMATOR_DRILL_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace coop::dev::cremator_drill
