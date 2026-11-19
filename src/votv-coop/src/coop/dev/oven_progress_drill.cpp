// coop/dev/oven_progress_drill.cpp -- see coop/dev/oven_progress_drill.h.

#include "coop/dev/oven_progress_drill.h"

#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/oven_judge_drill.h"  // the judge arms, and the client's walk to the box
#include "coop/interactables/interactable_sync.h"  // LiveOvens
#include "coop/interactables/oven_progress_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/oven_screen.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::oven_progress_drill {
namespace {

namespace OS = ue_wrap::oven_screen;
namespace DC = coop::dev::drill_chat;

enum class Arm { Off, Host, Client, Join };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::oven_progress_drill);
        return v == "host" ? Arm::Host : v == "client" ? Arm::Client : v == "join" ? Arm::Join : Arm::Off;
    }();
    return a;
}

bool Host() { return coop::roster::LocalIsHost(); }
char Who() { return Host() ? 'H' : 'C'; }
bool LocalActs() { return (ArmOf() == Arm::Client) != Host(); }

// The steps the acting peer raises on its screen, one at a time: a screw, the other three, the lid.
constexpr uint8_t  kSteps[]       = {OS::kScrewTL, OS::kScrewTR | OS::kScrewBL | OS::kScrewBR, OS::kLid};
constexpr int      kStepCount     = 3;
// A wait ends on its event; this only says INVALID when the event never comes. It never decides a verdict.
constexpr uint64_t kInvalidBoundMs = 30000;
const char* const  kSayMask       = "oven mask ";
const char* const  kSayFail       = "oven drill FAIL";

ue_wrap::CachedObjRef g_oven;
int      g_step = 0;           // the next step to raise
uint64_t g_waitMs = 0;         // when the current wait began; 0 = none
bool     g_raisedStep = false;
bool     g_started = false;    // the actor's fixture check has passed
bool     g_walked = false;     // the client is at the oven's switch box (the host's judge measures its reach)
bool     g_done = false;
int      g_saidMask = -2;      // the watching peer's last said mask; -1 for an oven with no screen
int      g_lastTheirs = -2;    // the acting peer's last read of the watching peer's mask
bool     g_baseTaken = false;
uint64_t g_baseArrived = 0;    // MasksArrived when the drill's first tick ran

void* Oven() {
    if (void* o = g_oven.Get()) return o;
    std::vector<std::pair<std::wstring, void*>> ovens;
    coop::interactable_sync::LiveOvens(ovens);
    if (ovens.empty()) return nullptr;
    g_oven.Set(ovens.front().second);
    return ovens.front().second;
}

void Verdict(bool pass, const char* fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[OVEN-DRILL] [%c] PASS %s", Who(), msg);
    else      UE_LOGW("[OVEN-DRILL] [%c] FAIL %s", Who(), msg);
    g_done = true;
}

void Invalid(const char* fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    UE_LOGW("[OVEN-DRILL] [%c] INVALID %s", Who(), msg);
    g_done = true;
}

// One wait on an event. Each call without the event counts toward the bound, which only says INVALID.
void Wait(const char* what) {
    const uint64_t now = coop::net::NowMs();
    if (!g_waitMs) { g_waitMs = now; return; }
    if (now - g_waitMs > kInvalidBoundMs)
        Invalid("%s did not come in %llu ms", what, static_cast<unsigned long long>(kInvalidBoundMs));
}
void Progress() { g_waitMs = 0; }

bool JoinOver(coop::net::Session& s) {
    if (!Host())
        return coop::net_pump::HasAnnouncedWorldReady() &&
               coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

constexpr int kNoOven = -3;

// The screen's mask now; -1 for an oven with no screen, kNoOven before the lane's index holds an oven.
int ReadMask() {
    uint8_t m = 0;
    void* oven = Oven();
    if (!oven) return kNoOven;
    return OS::TryReadProgress(oven, m) ? m : -1;
}

// The watching peer says its screen's mask whenever it changes. A mask that reached it with a bit its screen
// lacked must have changed the screen in the same drain: when the screen reads as before, the verdict is FAIL
// at that arrival, and the acting peer is told so it ends.
void TickWatcher() {
    const uint64_t arrived = coop::oven_progress_sync::MasksArrived();
    const bool arrival = arrived != g_baseArrived;
    g_baseArrived = arrived;
    const int now = ReadMask();
    if (now == kNoOven) return Wait("the oven in the lane's index");
    Progress();
    if (arrival && now == g_saidMask) {
        DC::Say(kSayFail);
        return Verdict(false, "a mask reached this peer with a bit its screen lacked, and the screen still reads %d",
                       now);
    }
    if (now == g_saidMask) return;
    g_saidMask = now;
    char line[40];
    std::snprintf(line, sizeof(line), "%s%d", kSayMask, now);
    DC::Say(line);
    UE_LOGI("[OVEN-DRILL] [%c] screen mask %d", Who(), now);
}

bool TheirMask(int& out) {
    std::string rest;
    if (!DC::SeenRestOf(kSayMask, &rest)) return false;
    char* end = nullptr;
    const long v = std::strtol(rest.c_str(), &end, 10);
    if (end == rest.c_str()) return false;
    out = static_cast<int>(v);
    return true;
}

// host and client: a step is raised on the acting peer's screen as a screw's turn leaves it, and must
// stand on the watching peer's before the next. The judge is the watching peer's next mask: equal to this
// screen's, the step stands; any other value is a contradiction.
void TickActor() {
    if (DC::SeenHas(kSayFail)) {
        g_done = true;
        UE_LOGW("[OVEN-DRILL] [%c] the watching peer judged FAIL; this arm ends", Who());
        return;
    }
    void* oven = Oven();
    if (!oven) return Wait("the oven in the lane's index");
    uint8_t mine = 0;
    if (!OS::TryReadProgress(oven, mine))
        return Invalid("the acting peer's oven has no repair screen (repaired in this save, or not found)");
    // A client's steps are judged by the host's reach of the oven's switch box: the director walks it there first.
    if (!Host() && !g_walked) {
        switch (coop::dev::oven_judge_drill::WalkToBox(oven)) {
        case coop::dev::oven_judge_drill::Walk::Going: return;
        case coop::dev::oven_judge_drill::Walk::Failed:
            return Invalid("the director's walk to the oven's switch box failed");
        case coop::dev::oven_judge_drill::Walk::Arrived:
            g_walked = true;
            Progress();
            UE_LOGI("[OVEN-DRILL] [C] walked to the oven's switch box");
            break;
        }
    }
    int theirs = -2;
    if (!TheirMask(theirs)) return Wait("the watching peer's first mask");
    if (!g_started) {
        if (mine != 0 || theirs != 0)
            return Invalid("fixture: both screens must start with every screw in; here 0x%02X, there %d", mine, theirs);
        g_started = true;
        g_lastTheirs = theirs;
        Progress();
    }
    if (!g_raisedStep) {
        g_raisedStep = true;
        Progress();
        const bool ok = OS::RaiseProgress(oven, kSteps[g_step]);
        UE_LOGI("[OVEN-DRILL] [%c] step %d: raised 0x%02X on this screen (%d)", Who(), g_step, kSteps[g_step],
                ok ? 1 : 0);
        return;
    }
    if (theirs != g_lastTheirs) {  // the watching peer's mask arrived
        g_lastTheirs = theirs;
        if (theirs == mine) {
            ++g_step;
            g_raisedStep = false;
            Progress();
            if (g_step >= kStepCount)
                Verdict(true, "three steps each stood on both screens; both read 0x%02X", mine);
            return;
        }
        return Verdict(false, "step %d: this screen reads 0x%02X, the other peer's mask settled at %d", g_step, mine,
                       theirs);
    }
    Wait("the watching peer's mask");
}

// join: the host's screen takes every step before anyone joins; the joiner's must hold them when the host's
// mask reaches it (the connect replay), which is the event that judges.
void TickJoin(coop::net::Session* session) {
    if (Host()) {
        if (g_raisedStep) return;
        void* oven = Oven();
        uint8_t m = 0;
        if (!oven || !OS::TryReadProgress(oven, m)) return Wait("the host's oven and screen");
        g_raisedStep = true;
        Progress();
        UE_LOGI("[OVEN-DRILL] [H] raised 0x%02X on the host's screen ahead of the join (%d)", OS::kAll,
                OS::RaiseProgress(oven, OS::kAll) ? 1 : 0);
        return;
    }
    if (!session->connected() || !JoinOver(*session)) return;
    if (coop::oven_progress_sync::MasksArrived() == g_baseArrived) return Wait("the host's replay of the oven mask");
    const int m = ReadMask();
    Verdict(m == OS::kAll, "the joiner's screen reads %d (0x%02X wanted) when the host's mask reached it, %llu bit(s) "
            "raised from the wire", m, OS::kAll, static_cast<unsigned long long>(coop::oven_progress_sync::BitsRaised()));
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off || coop::dev::oven_judge_drill::IsJudgeArm(); }

bool PeersJoined(coop::net::Session& session) { return JoinOver(session); }

void Tick(coop::net::Session* session) {
    if (coop::dev::oven_judge_drill::IsJudgeArm()) return coop::dev::oven_judge_drill::Tick(session);
    if (!IsEnabled() || !session || g_done) return;
    if (!g_baseTaken) {
        g_baseTaken = true;
        g_baseArrived = coop::oven_progress_sync::MasksArrived();
    }
    if (ArmOf() == Arm::Join) return TickJoin(session);
    if (!session->connected()) return;
    DC::SeenHas(kSayMask);  // the chat history records from its first read on
    if (!JoinOver(*session)) return;
    if (LocalActs()) TickActor();
    else TickWatcher();
}

void OnDisconnect() {
    coop::dev::oven_judge_drill::OnDisconnect();
    g_walked = false;
    g_oven.Reset();
    g_step = 0;
    g_waitMs = 0;
    g_raisedStep = false;
    g_started = false;
    g_done = false;
    g_saidMask = -2;
    g_lastTheirs = -2;
    g_baseTaken = false;
}

}  // namespace coop::dev::oven_progress_drill
