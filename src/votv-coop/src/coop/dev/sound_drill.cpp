// coop/dev/sound_drill.cpp -- see coop/dev/sound_drill.h.

#include "coop/dev/sound_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_hand.h"
#include "coop/dev/event_trigger.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/sound_catalog.h"
#include "coop/world/sound_relay.h"
#include "harness/autotest/peer_signal.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::sound_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace RV = coop::rig_verdict;
namespace PS = harness::autotest::peer_signal;
namespace SC = coop::sound_catalog;
namespace SR = coop::sound_relay;
namespace N  = coop::net;

constexpr const char* kHead = "[SOUND-DRILL] ";
constexpr uint64_t kBoundMs = 120000;   // INVALID bound of a phase: an event that did not come
constexpr uint64_t kTryEveryMs = 500;   // the pace of a swing, or of a look for the knocker
constexpr int kMaxTries = 6;
constexpr float kFarCm = 8000.f;        // the far arm's place, from the sender's body: far past any swing's reach
constexpr int kPeers = 3;

enum class Arm : uint8_t { Off, Hit, Knock, Far, HostWorld, Index };

Arm TheArm() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::sound_drill);
        return v == "hit" ? Arm::Hit : v == "knock" ? Arm::Knock : v == "far" ? Arm::Far
             : v == "hostworld" ? Arm::HostWorld : v == "index" ? Arm::Index : Arm::Off;
    }();
    return a;
}

const char* Tag() {
    switch (TheArm()) {
    case Arm::Hit: return "sound-hit";
    case Arm::Knock: return "sound-knock";
    case Arm::Far: return "sound-far";
    case Arm::HostWorld: return "sound-hostworld";
    case Arm::Index: return "sound-index";
    default: return "sound-off";
    }
}

bool g_begun = false;
bool g_ended = false;
bool g_go = false;
const char* g_phase = "the arrival of the other peers";
uint64_t g_phaseAt = 0;
int g_stage = 0;
int g_tries = 0;
uint64_t g_nextTryAt = 0;
bool g_heardSaid = false;

uint64_t Now() { return ::GetTickCount64(); }
int MySlot() { return static_cast<int>(coop::players::Registry::Get().LocalPeerId()); }
bool IsHost() { return coop::roster::LocalIsHost(); }
const char* Who() { return IsHost() ? "the host" : (MySlot() == 1 ? "client 1" : "client 2"); }

void Enter(const char* phase) {
    g_phase = phase;
    g_phaseAt = Now();
}

std::string Named(const char* name, int slot) { return std::string(name) + "_" + std::to_string(slot); }
bool PeekOf(const char* name, int slot, std::string& text) { return PS::Peek(Named(name, slot).c_str(), text); }

// Ends this peer's drill with a verdict, and tells the others, who end without one.
void Decide(RV::Outcome o, const char* fmt, ...) {
    if (g_ended) return;
    char text[352] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    g_ended = true;
    const char kind = o == RV::Outcome::Pass ? 'P' : o == RV::Outcome::Fail ? 'F' : 'I';
    PS::Mark("end", std::string(1, kind) + " " + text);
    RV::Say(Tag(), o, "%s", text);
}

bool Over() {
    if (g_ended) return true;
    std::string text;
    if (PS::Peek("end", text) && !text.empty()) {
        g_ended = true;
        UE_LOGI("%sthe run was ended by another peer: %s", kHead, text.c_str());
    }
    return g_ended;
}

bool Overdue() {
    if (Now() - g_phaseAt <= kBoundMs) return false;
    Decide(RV::Outcome::Invalid, "%s: no event within %llu s on %s", g_phase, static_cast<unsigned long long>(kBoundMs / 1000),
           Who());
    return true;
}

bool WorldUp(N::Session& s) {
    if (!coop::players::Registry::Get().Local()) return false;
    if (IsHost()) return s.IsSlotWorldReady(1) && s.IsSlotWorldReady(2);
    return coop::net_pump::HasAnnouncedWorldReady() && coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool HostHasBodyOfClient1() {
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(1);
    return rp && rp->valid() && rp->HasPose();
}

// True once every peer is in and the host has said go. The clients mark themselves in on every tick until the host's
// go is read: a peer whose drill began later reads only a mark written after its own start.
bool Arrive(N::Session& s) {
    if (!g_begun) {
        if (!WorldUp(s)) return false;
        if (!PS::Begin(Tag())) {   // says its own INVALID line
            g_ended = true;
            return false;
        }
        g_begun = true;
        Enter("the arrival of the other peers");
        UE_LOGI("%sarmed on %s (slot %d), arm %s", kHead, Who(), MySlot(), Tag());
    }
    if (Over() || Overdue()) return false;
    if (g_go) return true;
    if (IsHost()) {
        std::string text;
        bool all = true;
        for (int slot = 1; slot < kPeers; ++slot)
            if (!PS::Peek(Named("up", slot).c_str(), text)) all = false;
        if (!all || !HostHasBodyOfClient1()) return false;
        PS::Mark("go");
        g_go = true;
    } else {
        PS::Mark(Named("up", MySlot()).c_str(), "up");
        std::string text;
        if (!PS::Peek("go", text)) return false;
        g_go = true;
    }
    Enter("the arm's own event");
    UE_LOGI("%sall three peers are in: go", kHead);
    return g_go;
}

// A listener's half: the first sound a peer played here is said to the host, once.
void SayHeard() {
    if (g_heardSaid) return;
    const SR::Counts c = SR::GetCounts();
    if (c.played == 0) return;
    g_heardSaid = true;
    PS::Mark(Named("heard", MySlot()).c_str(), "index " + std::to_string(c.lastPlayed));
    UE_LOGI("%s%s played index %u from the relay", kHead, Who(), static_cast<unsigned>(c.lastPlayed));
}

// A refusal the arm did not ask for, read at the host: the first one decides.
const char* RefusalOtherThan(SR::Refusal want, SR::Refusal& got) {
    const SR::Counts c = SR::GetCounts();
    for (int i = 0; i < static_cast<int>(SR::Refusal::kCount); ++i) {
        if (c.refused[i] == 0 || i == static_cast<int>(want)) continue;
        got = static_cast<SR::Refusal>(i);
        return SR::RefusalName(got);
    }
    return nullptr;
}

// ---- hit -------------------------------------------------------------------------------------

// Client 1: a crowbar in the hand, a look at the floor, then swings through the player's own attack until one plays a
// catalogued sound here.
void HitActor() {
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    if (g_stage == 0) {
        const bool held = coop::dev::drill_hand::TakeIntoHandNow(L"crowbar", L"prop_crowbar_C");
        void* const pc = E::GetController(me);
        ue_wrap::FRotator r = pc ? E::GetControlRotation(pc) : ue_wrap::FRotator{};
        r.Pitch = -70.f;
        if (pc) E::SetControlRotation(pc, r);
        UE_LOGI("%sclient 1: a crowbar in the hand (%d), looking at the floor", kHead, held ? 1 : 0);
        g_stage = 1;
        g_nextTryAt = Now() + kTryEveryMs;
        return;
    }
    if (g_stage != 1 || Now() < g_nextTryAt) return;
    g_nextTryAt = Now() + kTryEveryMs;
    ++g_tries;
    const SR::Counts before = SR::GetCounts();
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(me), L"attack"));
    const bool ran = f.valid() && ue_wrap::Call(me, f);
    const bool hit = ran && f.Get<bool>(L"hit");
    const SR::Counts after = SR::GetCounts();
    UE_LOGI("%sclient 1: swing %d (ran=%d, hit=%d)", kHead, g_tries, ran ? 1 : 0, hit ? 1 : 0);
    // The sound plays inside the swing, so what the hook did with it is decided when the swing returns.
    if (after.notReadyHere > before.notReadyHere) {
        Decide(RV::Outcome::Invalid, "client 1's sound table was not built when its hit played: nothing could be sent");
        return;
    }
    if (after.notCatalogued > before.notCatalogued) {
        Decide(RV::Outcome::Fail, "client 1's hit played a sound the table does not hold for a melee hit");
        return;
    }
    if (after.playedHere > before.playedHere) {
        if (after.sent == before.sent) {
            Decide(RV::Outcome::Fail, "client 1's hit played index %u here and sent nothing for it",
                   static_cast<unsigned>(after.lastPlayedHere));
            return;
        }
        PS::Mark("swung", "index " + std::to_string(after.lastPlayedHere));
        g_stage = 2;
        Enter("the other peers playing the hit");
        return;
    }
    if (g_tries >= kMaxTries)
        Decide(RV::Outcome::Invalid, "client 1 swung %d times and the game played no catalogued sound for it", g_tries);
}

void HitHost() {
    const SR::Counts c = SR::GetCounts();
    for (int i = 0; i < static_cast<int>(SR::Refusal::kCount); ++i)
        if (c.refused[i]) {
            const SR::Refusal why = static_cast<SR::Refusal>(i);
            if (why == SR::Refusal::NotReady || why == SR::Refusal::NoBody)
                Decide(RV::Outcome::Invalid, "the host could not judge the hit: '%s'", SR::RefusalName(why));
            else
                Decide(RV::Outcome::Fail, "the host refused an honest melee hit as '%s'", SR::RefusalName(why));
            return;
        }
    std::string heard;
    if (c.played >= 1 && PeekOf("heard", 2, heard))
        Decide(RV::Outcome::Pass, "client 1's melee hit played on the host (index %u) and on client 2 (%s)",
               static_cast<unsigned>(c.lastPlayed), heard.c_str());
}

// ---- knock -----------------------------------------------------------------------------------

// The host: the vent knocker event, then the knocker's own knock, as its box's overlap runs it.
void KnockHost() {
    if (g_stage == 0) {
        bool ok = false;
        for (const auto& ev : coop::dev::event_trigger::Events())
            if (std::strcmp(ev.name, "ventKnocker") == 0) ok = coop::dev::event_trigger::Trigger(ev);
        UE_LOGI("%sthe host: the ventKnocker event fired (ok=%d)", kHead, ok ? 1 : 0);
        g_stage = 1;
        g_nextTryAt = Now() + kTryEveryMs;
        return;
    }
    if (g_stage == 1) {
        if (Now() < g_nextTryAt) return;
        g_nextTryAt = Now() + kTryEveryMs;
        void* kocker = nullptr;
        for (void* o : R::FindObjectsByClass(L"kocker_C"))
            if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) kocker = o;
        if (!kocker) return;
        ++g_tries;
        const SR::Counts before = SR::GetCounts();
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(kocker), L"knock"));
        const bool ran = f.valid() && ue_wrap::Call(kocker, f);
        const SR::Counts after = SR::GetCounts();
        UE_LOGI("%sthe host: the knocker's knock %d (ran=%d)", kHead, g_tries, ran ? 1 : 0);
        if (after.notReadyHere > before.notReadyHere) {
            Decide(RV::Outcome::Invalid, "the host's sound table was not built when the knock played: nothing could be sent");
            return;
        }
        if (after.notCatalogued > before.notCatalogued) {
            Decide(RV::Outcome::Fail, "the knock played a sound the table does not hold for the knocker");
            return;
        }
        if (after.playedHere > before.playedHere) {
            if (after.sent == before.sent) {
                Decide(RV::Outcome::Fail, "the knock played index %u here and sent nothing for it",
                       static_cast<unsigned>(after.lastPlayedHere));
                return;
            }
            g_stage = 2;
            Enter("both clients playing the knock");
            return;
        }
        if (g_tries >= kMaxTries) Decide(RV::Outcome::Invalid, "the knocker's knock ran %d times and played no sound here", g_tries);
        return;
    }
    std::string a, b;
    if (PeekOf("heard", 1, a) && PeekOf("heard", 2, b))
        Decide(RV::Outcome::Pass, "the vent knock played on client 1 (%s) and on client 2 (%s)", a.c_str(), b.c_str());
}

// ---- far, hostworld, index -------------------------------------------------------------------

// Client 1 sends the host the arm's sound once: a raw SoundAt as an honest client would not.
void RawSender(N::Session& s) {
    if (g_stage != 0) return;
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!me || !E::TryGetActorLocation(me, at) || !SC::Ready()) return;   // the table is tried again on the next tick
    N::SoundAtPayload p{};
    const Arm arm = TheArm();
    p.sound = arm == Arm::HostWorld ? SC::FirstFor(SC::kBell) : SC::FirstFor(SC::kAttack);
    if (arm == Arm::Index) p.sound = static_cast<uint16_t>(p.sound + SC::Size());
    p.loc[0] = at.X + (arm == Arm::Far ? kFarCm : 0.f);
    p.loc[1] = at.Y;
    p.loc[2] = at.Z;
    p.volume = 1.f;
    p.pitch = 1.f;
    p.catalogue = SC::Digest();
    if (!p.sound) return;
    if (!s.SendReliable(N::ReliableKind::SoundAt, &p, sizeof(p))) {
        Decide(RV::Outcome::Invalid, "the session refused client 1's raw sound");
        return;
    }
    UE_LOGI("%sclient 1: sent index %u at (%.0f,%.0f,%.0f) as the %s arm", kHead, static_cast<unsigned>(p.sound), p.loc[0],
            p.loc[1], p.loc[2], Tag());
    g_stage = 1;
    Enter("the host's refusal");
}

SR::Refusal WantedRefusal() {
    switch (TheArm()) {
    case Arm::HostWorld: return SR::Refusal::Caller;
    case Arm::Index: return SR::Refusal::Index;
    default: return SR::Refusal::Reach;
    }
}

// A raw sound the host should refuse: its refusal for the arm's reason is the verdict, and nothing is sent after it. Any
// play of it, on the host or on a client, is the failure.
void RawHost() {
    const SR::Counts c = SR::GetCounts();
    if (c.played > 0) {
        Decide(RV::Outcome::Fail, "the host played client 1's raw sound (index %u) instead of refusing it",
               static_cast<unsigned>(c.lastPlayed));
        return;
    }
    const SR::Refusal want = WantedRefusal();
    if (c.refused[static_cast<int>(want)] > 0) {
        Decide(RV::Outcome::Pass, "the host refused client 1's raw sound as '%s' and played and sent nothing", SR::RefusalName(want));
        return;
    }
    SR::Refusal got{};
    if (RefusalOtherThan(want, got) == nullptr) return;
    if (got == SR::Refusal::NoBody || got == SR::Refusal::NotReady)
        Decide(RV::Outcome::Invalid, "the host could not judge it: '%s' (wanted '%s')", SR::RefusalName(got), SR::RefusalName(want));
    else
        Decide(RV::Outcome::Fail, "the host refused client 1's raw sound as '%s', not as '%s'", SR::RefusalName(got),
               SR::RefusalName(want));
}

void RawListener() {
    const SR::Counts c = SR::GetCounts();
    if (c.played > 0)
        Decide(RV::Outcome::Fail, "client 2 played client 1's raw sound (index %u) that the host should have refused",
               static_cast<unsigned>(c.lastPlayed));
}

}  // namespace

void Tick(coop::net::Session* session) {
    const Arm arm = TheArm();
    if (arm == Arm::Off || g_ended || !session || !session->running() || !session->connected()) return;
    if (!Arrive(*session)) return;
    const bool host = IsHost();
    const int slot = MySlot();
    switch (arm) {
    case Arm::Hit:
        if (host) HitHost();
        else if (slot == 1) HitActor();
        else SayHeard();
        break;
    case Arm::Knock:
        if (host) KnockHost();
        else SayHeard();
        break;
    default:
        if (host) RawHost();
        else if (slot == 1) RawSender(*session);
        else RawListener();
        break;
    }
}

}  // namespace coop::dev::sound_drill
