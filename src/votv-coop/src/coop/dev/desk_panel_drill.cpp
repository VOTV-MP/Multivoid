// coop/dev/desk_panel_drill.cpp -- see coop/dev/desk_panel_drill.h.

#include "coop/dev/desk_panel_drill.h"

#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/interactables/desk_panel_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_panels.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::desk_panel_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DP = ue_wrap::desk_panels;
namespace CD = ue_wrap::console_desk;
namespace DC = coop::dev::drill_chat;
using ue_wrap::FVector;

enum class Arm { Off, Host, Client, Race };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::desk_panel_drill);
        return v == "host" ? Arm::Host : v == "client" ? Arm::Client : v == "race" ? Arm::Race : Arm::Off;
    }();
    return a;
}

bool Host() { return coop::roster::LocalIsHost(); }
char Who() { return Host() ? 'H' : 'C'; }
// The peer that runs the steps: the client in the client arm, the host otherwise (the race's unbolt).
bool LocalActs() { return (ArmOf() == Arm::Client) != Host(); }

// The drill's panel, picked from the desk as the save left it: the first panel still on, a bolted one
// before an unbolted one, never panel 2, which the desk never lets go.
int      g_panel  = 0;
uint8_t  g_bit    = 1;
bool     g_picked = false;
constexpr uint64_t kDwellMs     = 2500;  // a step's state must still stand this long after it
constexpr uint64_t kStepBoundMs = 12000;
constexpr uint64_t kRaceLeadMs  = 600;   // from the go line to the press
constexpr uint64_t kRaceEarlyMs = 25;    // the client presses this much sooner: the line reached it later
constexpr uint64_t kRaceJudgeMs = 6000;
constexpr int      kPutTries    = 5;
const char* const  kSayState    = "panel state ";
const char* const  kSayGo       = "panel race go";

// What a peer's desk and world hold: both masks and the live panel props of the drill's place.
struct State {
    int on = -1, unscrewed = -1, props = -1;
    bool operator==(const State& o) const { return on == o.on && unscrewed == o.unscrewed && props == o.props; }
};

void PickPanel(const DP::Masks& m) {
    for (int pass = 0; pass < 2 && !g_picked; ++pass)
        for (int i = 0; i < DP::kPanels && !g_picked; ++i) {
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            if (i == 2 || !(m.on & bit) || (pass == 0 && (m.unscrewed & bit))) continue;
            g_panel = i;
            g_bit = bit;
            g_picked = true;
        }
    if (g_picked)
        UE_LOGI("[DESK-PANEL-DRILL] [%c] the drill's panel is %d (desk on=0x%X unscrewed=0x%X)",
                coop::roster::LocalIsHost() ? 'H' : 'C', g_panel, m.on, m.unscrewed);
}

// The live panel props of the drill's place. A save can hold some already.
std::vector<void*> PanelPropsNow() {
    std::vector<void*> out;
    void* cls = DP::ClassForPanel(g_panel);
    if (!cls) return out;
    for (void* a : R::FindObjectsByClass(R::ToString(R::NameOf(cls)).c_str()))
        if (a && R::IsLive(a) && R::ClassOf(a) == cls && !R::NameStartsWith(R::NameOf(a), L"Default__"))
            out.push_back(a);
    return out;
}

int PanelProps() { return DP::ClassForPanel(g_panel) ? static_cast<int>(PanelPropsNow().size()) : -1; }

// The panel prop the last press spawned: the one that was not there before it.
std::vector<void*> g_beforePress;
void* PressedPanelProp() {
    for (void* a : PanelPropsNow())
        if (std::find(g_beforePress.begin(), g_beforePress.end(), a) == g_beforePress.end()) return a;
    return nullptr;
}

bool ReadState(State& out) {
    DP::Masks m{};
    if (!DP::EnsureResolved() || !CD::Instance() || !DP::Read(m)) return false;
    if (!g_picked) PickPanel(m);
    out = State{m.on, m.unscrewed, PanelProps()};
    return true;
}

// The other peer's last said state.
bool TheirState(State& out) {
    std::string rest;
    const std::string prefix = std::string(kSayState) + (Host() ? "C " : "H ");
    if (!DC::SeenRestOf(prefix, &rest)) return false;
    return std::sscanf(rest.c_str(), "%d %d %d", &out.on, &out.unscrewed, &out.props) == 3;
}

State g_said{};
void SayStateOnChange() {
    State now{};
    if (!DC::PollDue() || !ReadState(now) || now == g_said) return;
    g_said = now;
    char line[64];
    std::snprintf(line, sizeof(line), "%s%c %d %d %d", kSayState, Who(), now.on, now.unscrewed, now.props);
    DC::Say(line);
    UE_LOGI("[DESK-PANEL-DRILL] [%c] state on=0x%X unscrewed=0x%X panel props=%d", Who(), now.on, now.unscrewed,
            now.props);
}

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

// ---- The desk's own verbs ------------------------------------------------------------------------

bool SetLookingAt(void* desk, int panel) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(desk), L"lookingAtPanel");
    if (off < 0) return false;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(desk) + off) = panel;
    return true;
}

int TakeOffLatch(void* desk) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(desk), L"panelTakeOff");
    return off < 0 ? -1 : (*(reinterpret_cast<uint8_t*>(desk) + off) ? 1 : 0);
}

// The wrench's last step: the desk toggles the bolts of the panel it is looking at.
bool Unscrew() {
    void* desk = CD::Instance();
    void* fn = desk ? R::FindDispatchFunctionCached(R::ClassOf(desk), L"unscrewPanel") : nullptr;
    if (!fn || !SetLookingAt(desk, g_panel)) return false;
    ue_wrap::ParamFrame f(fn);
    const bool ok = f.valid() && ue_wrap::Call(desk, f);
    SetLookingAt(desk, -1);
    return ok;
}

// The E press on the panel the desk is looking at: an unbolted panel comes off into the hand.
bool Press() {
    void* desk = CD::Instance();
    void* player = coop::players::Registry::Get().Local();
    void* fn = desk ? R::FindDispatchFunctionCached(R::ClassOf(desk), L"actionOptionIndex") : nullptr;
    if (!fn || !player || !SetLookingAt(desk, g_panel)) return false;
    g_beforePress = PanelPropsNow();
    ue_wrap::ParamFrame f(fn);
    const bool ok = f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(desk, f);
    SetLookingAt(desk, -1);
    return ok;
}

// The drill's player stands wherever its world put it, far from the desk, and a hand that far would
// drag the panel through the desk's own lid again and again. So the panel is put down beside the
// player at once, as one who carried it away would leave it.
void CarryAway(void* prop) {
    void* player = coop::players::Registry::Get().Local();
    FVector at{};
    if (!prop || !player || !E::TryGetActorLocation(player, at)) return;
    E::ReleaseMainPlayerGrabIfHolding(player, prop);
    E::SetActorLocation(prop, FVector{at.X, at.Y, at.Z + 60.f});
}

// The player's grab names the panel prop, as it does for one who picked it up; null lets go.
bool Grab(void* prop) {
    void* player = coop::players::Registry::Get().Local();
    return player && E::WriteMainPlayerGrabbingPair(player, prop, nullptr) &&
           (!prop || E::IsMainPlayerGrabbing(player, prop));
}

// ---- The steps -----------------------------------------------------------------------------------

enum class Step { WaitJoin, Unbolt, Take, PutBack, Rebolt, RaceWait, RaceJudge, Done };
Step g_step = Step::WaitJoin;
uint64_t g_stepMs = 0;
bool g_acted = false;
ue_wrap::CachedObjRef g_prop;
FVector g_lidAt{};
int g_putTries = 0;
uint64_t g_nextPutMs = 0;
uint64_t g_goSeenMs = 0;
bool g_pressed = false;

const char* Name(Step s) {
    switch (s) {
    case Step::Unbolt:  return "unbolt";
    case Step::Take:    return "take-off";
    case Step::PutBack: return "put-back";
    case Step::Rebolt:  return "rebolt";
    default:            return "?";
    }
}

void Go(Step s) {
    g_step = s;
    g_stepMs = coop::net::NowMs();
    g_acted = false;
}
uint64_t InStepMs() { return coop::net::NowMs() - g_stepMs; }

void Verdict(bool pass, const char* fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[DESK-PANEL-DRILL] [%c] PASS %s", Who(), msg);
    else      UE_LOGW("[DESK-PANEL-DRILL] [%c] FAIL %s", Who(), msg);
    Go(Step::Done);
}

// Has the step's state stood on both peers for the dwell? Fails the run at the bound.
bool Stands(const State& want, Step next) {
    State mine{}, theirs{};
    const bool haveMine = ReadState(mine), haveTheirs = TheirState(theirs);
    if (haveMine && haveTheirs && mine == want && theirs == want && InStepMs() >= kDwellMs) {
        UE_LOGI("[DESK-PANEL-DRILL] [%c] %s stands on both peers: on=0x%X unscrewed=0x%X panel props=%d", Who(),
                Name(g_step), want.on, want.unscrewed, want.props);
        Go(next);
        return true;
    }
    if (InStepMs() > kStepBoundMs)
        Verdict(false, "%s: wanted on=0x%X unscrewed=0x%X props=%d; here on=0x%X unscrewed=0x%X props=%d, there "
                "on=0x%X unscrewed=0x%X props=%d", Name(g_step), want.on, want.unscrewed, want.props, mine.on,
                mine.unscrewed, mine.props, theirs.on, theirs.unscrewed, theirs.props);
    return false;
}

State g_base{};        // both peers' state before the first step: the drill's panel on
bool  g_wasBolted = true;  // a panel the save holds unbolted is not bolted or unbolted by the drill
int   g_raceBaseProps = -1;

void TickActor(coop::net::Session& s) {
    switch (g_step) {
    case Step::WaitJoin: {
        State theirs{};
        if (!JoinOver(s) || !ReadState(g_base) || !TheirState(theirs)) return;
        if (!g_picked || !(g_base.on & g_bit) || !(theirs == g_base))
            return Verdict(false, "fixture: a panel must be on and both peers must start alike; here on=0x%X "
                           "unscrewed=0x%X props=%d, there on=0x%X unscrewed=0x%X props=%d", g_base.on,
                           g_base.unscrewed, g_base.props, theirs.on, theirs.unscrewed, theirs.props);
        g_wasBolted = !(g_base.unscrewed & g_bit);
        return Go(Step::Unbolt);
    }
    case Step::Unbolt: {
        if (!g_acted) {
            g_acted = true;
            if (g_wasBolted && !Unscrew()) return Verdict(false, "unbolt: the desk's unscrewPanel did not dispatch");
        }
        const State want{g_base.on, g_base.unscrewed | g_bit, g_base.props};
        if (ArmOf() != Arm::Race) { Stands(want, Step::Take); return; }
        if (Stands(want, Step::RaceWait)) DC::Say(kSayGo);
        return;
    }
    case Step::Take: {
        if (!g_acted) {
            g_acted = true;
            if (!Press()) return Verdict(false, "take-off: the desk's actionOptionIndex did not dispatch");
            void* prop = PressedPanelProp();
            if (!prop || !E::TryGetActorLocation(prop, g_lidAt))
                return Verdict(false, "take-off: the press left no panel prop on the acting peer");
            g_prop.Set(prop);
            CarryAway(prop);
            UE_LOGI("[DESK-PANEL-DRILL] [%c] took the panel off: prop born at (%.1f, %.1f, %.1f), put down beside "
                    "the player", Who(), g_lidAt.X, g_lidAt.Y, g_lidAt.Z);
        }
        Stands(State{g_base.on & ~g_bit, g_base.unscrewed | g_bit, g_base.props + 1}, Step::PutBack);
        return;
    }
    case Step::PutBack: {
        // The acting player's grab names the panel and it is set back where the press spawned it, inside
        // its lid: the carrier's own desk puts it on. The desk hears an actor's first touch only, so a
        // prop that lies against any of its volumes is set well clear first. Repeated a few times.
        State mine{};
        const uint64_t now = coop::net::NowMs();
        if (ReadState(mine) && !(mine.on & g_bit) && g_putTries < kPutTries && now >= g_nextPutMs) {
            void* prop = g_prop.Get();
            if (!prop) return Verdict(false, "put-back: the panel prop is gone while the panel is still off");
            ++g_putTries;
            g_nextPutMs = now + 1000;
            const bool held = Grab(prop);
            E::SetActorLocation(prop, FVector{g_lidAt.X, g_lidAt.Y, g_lidAt.Z + 1000.f});
            const bool ok = E::SetActorLocation(prop, g_lidAt);
            Grab(nullptr);
            UE_LOGI("[DESK-PANEL-DRILL] [%c] put-back try %d: held=%d snap=%d, the desk's take-off latch reads %d",
                    Who(), g_putTries, held ? 1 : 0, ok ? 1 : 0, TakeOffLatch(CD::Instance()));
            g_stepMs = now;  // the dwell and the bound run from the last snap
        }
        if (Stands(State{g_base.on, g_base.unscrewed | g_bit, g_base.props}, g_wasBolted ? Step::Rebolt : Step::Done) &&
            !g_wasBolted)
            UE_LOGI("[DESK-PANEL-DRILL] [%c] PASS take-off and put-back each stood on both peers; the save held the "
                    "panel unbolted (put-back snaps %d)", Who(), g_putTries);
        return;
    }
    case Step::Rebolt: {
        if (!g_acted) {
            g_acted = true;
            if (!Unscrew()) return Verdict(false, "rebolt: the desk's unscrewPanel did not dispatch");
        }
        State mine{};
        if (Stands(g_base, Step::Done) && ReadState(mine))
            UE_LOGI("[DESK-PANEL-DRILL] [%c] PASS unbolt, take-off, put-back and rebolt each stood on both peers "
                    "(put-back snaps %d, canonicals written here %llu)", Who(), g_putTries,
                    static_cast<unsigned long long>(coop::desk_panel_sync::CanonicalsAdopted()));
        return;
    }
    default:
        return;
    }
}

// Both peers press on one instant, counted from the tick the go line is live here.
void TickRace() {
    if (g_step == Step::WaitJoin || g_step == Step::Unbolt) {
        if (!DC::ChatHasLive(kSayGo)) return;
        Go(Step::RaceWait);
    }
    if (g_step == Step::RaceWait) {
        if (!g_goSeenMs) {
            if (!DC::ChatHasLive(kSayGo)) return;
            g_goSeenMs = coop::net::NowMs();
            State base{};
            if (ReadState(base)) g_raceBaseProps = base.props;
        }
        const uint64_t at = g_goSeenMs + kRaceLeadMs - (Host() ? 0 : kRaceEarlyMs);
        if (!g_pressed && coop::net::NowMs() >= at) {
            g_pressed = true;
            const bool ok = Press();
            CarryAway(PressedPanelProp());
            UE_LOGI("[DESK-PANEL-DRILL] [%c] race press dispatched=%d, panel props here %d", Who(), ok ? 1 : 0,
                    PanelProps());
            Go(Step::RaceJudge);
        }
        return;
    }
    if (g_step != Step::RaceJudge || InStepMs() < kRaceJudgeMs) return;
    State mine{}, theirs{};
    const bool haveMine = ReadState(mine), haveTheirs = TheirState(theirs);
    const unsigned long long denied = coop::desk_panel_sync::TakeOffsDenied();
    if (!haveMine || !haveTheirs)
        return Verdict(false, "race: a state did not read (here %d, there %d)", haveMine ? 1 : 0, haveTheirs ? 1 : 0);
    if (mine.props != g_raceBaseProps + 1 || (mine.on & g_bit) || !(mine == theirs))
        return Verdict(false, "race: wanted the panel off and one panel prop more than the %d before on both peers; "
                       "here on=0x%X props=%d, there on=0x%X props=%d (take-offs denied here %llu)", g_raceBaseProps,
                       mine.on, mine.props, theirs.on, theirs.props, denied);
    Verdict(true, "race: the panel is off and one panel prop more is left on both peers (take-offs denied here %llu)",
            denied);
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected()) return;
    // The chat history records from its first read on, so it is read from the first pass: a state the
    // other peer says before this one's join is over is still found.
    DC::SeenHas(kSayGo);
    if (!JoinOver(*session)) return;
    SayStateOnChange();
    if (g_step == Step::Done) return;
    const bool racing = ArmOf() == Arm::Race && (g_step == Step::RaceWait || g_step == Step::RaceJudge || !LocalActs());
    if (racing) TickRace();
    else if (LocalActs()) TickActor(*session);
}

void OnDisconnect() {
    g_step = Step::WaitJoin;
    g_stepMs = 0;
    g_acted = false;
    g_prop.Reset();
    g_picked = false;
    g_wasBolted = true;
    g_raceBaseProps = -1;
    g_beforePress.clear();
    g_putTries = 0;
    g_nextPutMs = 0;
    g_goSeenMs = 0;
    g_pressed = false;
    g_said = State{};
}

}  // namespace coop::dev::desk_panel_drill
