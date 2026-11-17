// coop/dev/atv_part_drill.cpp -- see coop/dev/atv_part_drill.h.

#include "coop/dev/atv_part_drill.h"

#include "atv_spawn_near.h"  // SpawnAtNav, KeyOf (co-located private header)

#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/interactables/atv_part_intent.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
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
#include "ue_wrap/devices/atv_condition.h"
#include "ue_wrap/devices/atv_parts.h"
#include "ue_wrap/engine/engine.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::atv_part_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace AC = ue_wrap::atv_condition;
namespace AP = ue_wrap::atv_parts;
namespace DC = coop::dev::drill_chat;
namespace SN = coop::dev::atv_spawn_near;
using ue_wrap::FVector;

enum class Arm { Off, Wheel, Battery };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::atv_part_drill);
        return v == "wheel" ? Arm::Wheel : v == "battery" ? Arm::Battery : Arm::Off;
    }();
    return a;
}

bool Host() { return coop::roster::LocalIsHost(); }
char Who() { return Host() ? 'H' : 'C'; }

constexpr int      kWheel       = 0;
constexpr uint8_t  kBit         = 1u << kWheel;
constexpr float    kAheadCm     = 220.f;   // the ATV is born this far ahead of the host's player
constexpr float    kFixtureCharge = 60.f;
constexpr uint64_t kDwellMs     = 2500;    // a step's state must still stand this long after it
constexpr uint64_t kStepBoundMs = 15000;
constexpr uint64_t kHandMs      = 2500;    // the part in the client's hand reaches the host's record of it
constexpr uint64_t kStartBoundMs = 90000;
const char* const  kSayKey   = "atvpart key ";
const char* const  kSayState = "atvpart state ";
const char* const  kSayDone  = "atvpart done";

const wchar_t* PartClass() { return ArmOf() == Arm::Wheel ? L"prop_atvWheel_C" : L"prop_atvcarbattery_C"; }
const wchar_t* PartItem() { return ArmOf() == Arm::Wheel ? L"atvwheel" : L"atvcarbattery"; }

// What a peer's copy holds: the tyre mask, whether a battery is fitted, and the part's world props (a
// hand item and its mirrors are no world props).
struct State {
    int mask = -1, battery = -1, props = -1;
    bool operator==(const State& o) const { return mask == o.mask && battery == o.battery && props == o.props; }
};

ue_wrap::CachedObjRef g_atv;
std::wstring g_key;

int PartProps() {
    int n = 0;
    for (void* a : R::FindObjectsByClass(PartClass()))
        if (a && R::IsLive(a) && !R::NameStartsWith(R::NameOf(a), L"Default__") &&
            !coop::hand_item::IsHandAxisActor(a))
            ++n;
    return n;
}

bool ReadState(State& out) {
    void* atv = g_atv.Get();
    AC::Snapshot snap;
    if (!atv || (!AC::Resolved() && !AC::Resolve()) || !AC::Read(atv, snap)) return false;
    out = State{snap.mask, snap.battery >= 0.f ? 1 : 0, PartProps()};
    return true;
}

bool TheirState(State& out) {
    std::string rest;
    const std::string prefix = std::string(kSayState) + (Host() ? "C " : "H ");
    if (!DC::SeenRestOf(prefix, &rest)) return false;
    return std::sscanf(rest.c_str(), "%d %d %d", &out.mask, &out.battery, &out.props) == 3;
}

State g_said{};
void SayStateOnChange() {
    State now{};
    if (!DC::PollDue() || !ReadState(now) || now == g_said) return;
    g_said = now;
    char line[64];
    std::snprintf(line, sizeof(line), "%s%c %d %d %d", kSayState, Who(), now.mask, now.battery, now.props);
    DC::Say(line);
    UE_LOGI("[ATV-PART-DRILL] [%c] state mask=0x%X battery fitted=%d part props=%d", Who(), now.mask, now.battery,
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

enum class Step { Start, Named, Remove, Hand, Fit, Done };
Step g_step = Step::Start;
uint64_t g_stepMs = 0;
bool g_acted = false;
State g_base{};
coop::atv_part_intent::Stats g_statsAtStart{};

const char* Name(Step s) {
    switch (s) {
    case Step::Remove: return "unbolt";
    case Step::Fit:    return "fit";
    default:           return "?";
    }
}

void Go(Step s) {
    g_step = s;
    g_stepMs = coop::net::NowMs();
    g_acted = false;
}
uint64_t InStepMs() { return coop::net::NowMs() - g_stepMs; }

void Verdict(bool pass, const char* fmt, ...) {
    char msg[360];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[ATV-PART-DRILL] [%c] PASS %s", Who(), msg);
    else      UE_LOGW("[ATV-PART-DRILL] [%c] FAIL %s", Who(), msg);
    Go(Step::Done);
}

// Has the step's state stood on both peers for the dwell? Fails the run at the bound.
bool Stands(const State& want) {
    State mine{}, theirs{};
    const bool haveMine = ReadState(mine), haveTheirs = TheirState(theirs);
    if (haveMine && haveTheirs && mine == want && theirs == want && InStepMs() >= kDwellMs) {
        UE_LOGI("[ATV-PART-DRILL] [%c] %s stands on both peers: mask=0x%X battery fitted=%d part props=%d", Who(),
                Name(g_step), want.mask, want.battery, want.props);
        return true;
    }
    if (InStepMs() > kStepBoundMs) {
        const auto st = coop::atv_part_intent::GetStats();
        Verdict(false, "%s: wanted mask=0x%X battery=%d props=%d; here mask=0x%X battery=%d props=%d, there "
                "mask=0x%X battery=%d props=%d (asked here %llu)", Name(g_step), want.mask, want.battery, want.props,
                mine.mask, mine.battery, mine.props, theirs.mask, theirs.battery, theirs.props,
                static_cast<unsigned long long>(st.asked - g_statsAtStart.asked));
    }
    return false;
}

// ---- The host: the fixture, then its own counters ------------------------------------------------

void TickHost(coop::net::Session& s) {
    void* me = coop::players::Registry::Get().Local();
    switch (g_step) {
    case Step::Start: {
        FVector at{};
        if (!JoinOver(s) || !me || !E::TryGetActorLocation(me, at)) return;
        if (!g_stepMs) g_stepMs = coop::net::NowMs();
        if (!g_atv.Get()) {
            const FVector fwd = E::GetActorForwardVector(me);
            const float side[4][2] = {{fwd.X, fwd.Y}, {-fwd.Y, fwd.X}, {fwd.Y, -fwd.X}, {-fwd.X, -fwd.Y}};
            SN::Spawned sp;
            for (const auto& d : side) {
                sp = SN::Spawned{};
                if (SN::SpawnAtNav(me, FVector{at.X + d[0] * kAheadCm, at.Y + d[1] * kAheadCm, at.Z + SN::kLiftCm}, sp))
                    break;
            }
            if (!sp.actor) return Verdict(false, "fixture: no ATV could be spawned beside the host (%s, got %s)",
                                          sp.what ? sp.what : "?", sp.got);
            g_atv.Set(sp.actor);
            return;
        }
        const char* why = nullptr;
        const SN::Key k = SN::KeyOf(g_atv.Get(), &g_key, &why);
        if (k == SN::Key::Bad) return Verdict(false, "fixture: the spawned ATV's key cannot ride a chat line (%s)", why);
        if (k != SN::Key::Ok) {
            if (InStepMs() > kStartBoundMs) Verdict(false, "fixture: the lane's table never held the spawned ATV");
            return;
        }
        // The part the arm works on is on the ATV to start with.
        AC::Snapshot snap;
        void* atv = g_atv.Get();
        if ((!AC::Resolved() && !AC::Resolve()) || !AC::Read(atv, snap))
            return Verdict(false, "fixture: the spawned ATV's condition does not read");
        if (ArmOf() == Arm::Wheel && !(snap.mask & kBit)) {
            AC::WritePresence(atv, static_cast<uint8_t>(snap.mask | kBit), snap.hasSpare);
            AC::CallVerb(atv, AC::Verb::UpdTires);
        }
        if (ArmOf() == Arm::Battery && snap.battery < 0.f) {
            AC::WriteBattery(atv, kFixtureCharge);
            AC::CallVerb(atv, AC::Verb::UpdBattery);
        }
        g_statsAtStart = coop::atv_part_intent::GetStats();
        DC::Say(std::string(kSayKey) + std::string(g_key.begin(), g_key.end()));
        UE_LOGI("[ATV-PART-DRILL] [H] spawned the ATV '%ls' (mask=0x%X battery=%.1f) and named it", g_key.c_str(),
                snap.mask, snap.battery);
        return Go(Step::Named);
    }
    case Step::Named: {
        if (!DC::SeenHas(kSayDone)) return;
        const auto st = coop::atv_part_intent::GetStats();
        const unsigned long long ran = st.ran - g_statsAtStart.ran, refused = st.refused - g_statsAtStart.refused;
        State mine{};
        ReadState(mine);
        if (ran != 2 || refused != 0)
            return Verdict(false, "the host ran %llu asked changes and refused %llu (2 and 0 wanted)", ran, refused);
        return Verdict(true, "the host ran both asked changes and refused none; mask=0x%X battery fitted=%d part "
                       "props=%d", mine.mask, mine.battery, mine.props);
    }
    default:
        return;
    }
}

// ---- The client: the wrench's last step, then the part put back ----------------------------------

bool CallFit(void* atv, void* held) {
    void* player = coop::players::Registry::Get().Local();
    if (ArmOf() == Arm::Wheel) {
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(atv), L"putTire");
        if (!fn) return false;
        ue_wrap::ParamFrame f(fn);
        return f.valid() && f.Set<int32_t>(L"index", kWheel) && f.Set<void*>(L"wheelObject", held) &&
               ue_wrap::Call(atv, f);
    }
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(atv), L"playerUsedOn");
    if (!fn || !player) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(atv, f);
}

void TickClient(coop::net::Session& s) {
    const bool wheel = ArmOf() == Arm::Wheel;
    switch (g_step) {
    case Step::Start: {
        if (!JoinOver(s)) return;
        if (!g_stepMs) g_stepMs = coop::net::NowMs();
        std::string rest;
        coop::atv_sync::TableView view{};
        State theirs{};
        const bool named = DC::SeenRestOf(kSayKey, &rest);
        if (named && !g_atv.Get()) {
            g_key = std::wstring(rest.begin(), rest.end());
            if (coop::atv_sync::ReadTable(g_key, view) && view.actor) g_atv.Set(view.actor);
        }
        const bool ready = g_atv.Get() && AP::Resolve() && ReadState(g_base) && TheirState(theirs) &&
                           theirs == g_base && coop::atv_sync::ReadTable(g_key, view) && view.syncerSlot == 0 &&
                           (wheel ? (g_base.mask & kBit) != 0 : g_base.battery == 1);
        if (!ready) {
            if (InStepMs() > kStartBoundMs)
                Verdict(false, "fixture: the named ATV with its part on never stood on both peers under the host's "
                        "sync (named=%d resolved=%d here mask=0x%X battery=%d props=%d, there mask=0x%X battery=%d "
                        "props=%d)", named ? 1 : 0, g_atv.Get() ? 1 : 0, g_base.mask, g_base.battery, g_base.props,
                        theirs.mask, theirs.battery, theirs.props);
            return;
        }
        g_statsAtStart = coop::atv_part_intent::GetStats();
        // The host judges an unbolt against the tool in the sender's hand.
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"tool_w", L"prop_wrenchtool_C"))
            return Verdict(false, "unbolt: the client's hand did not take a wrench");
        return Go(Step::Remove);
    }
    case Step::Remove: {
        if (!g_acted) {
            if (InStepMs() < kHandMs) return;  // the wrench reaches the host's record of the hand
            g_acted = true;
            g_stepMs = coop::net::NowMs();
            const bool ran = AP::RunUnscrew(g_atv.Get(), wheel ? kWheel : -1);
            const auto st = coop::atv_part_intent::GetStats();
            UE_LOGI("[ATV-PART-DRILL] [C] ran the wrench's last step on %s: dispatched=%d, asked of the host %llu",
                    wheel ? "wheel 0" : "the battery", ran ? 1 : 0,
                    static_cast<unsigned long long>(st.asked - g_statsAtStart.asked));
            if (st.asked == g_statsAtStart.asked)
                return Verdict(false, "unbolt: the client's body was not cancelled and asked of the host");
        }
        const State want{wheel ? g_base.mask & ~kBit : g_base.mask, wheel ? g_base.battery : 0, g_base.props + 1};
        if (Stands(want)) Go(Step::Hand);
        return;
    }
    case Step::Hand: {
        if (!g_acted) {
            g_acted = true;
            if (!coop::dev::drill_hand::TakeIntoHandNow(PartItem(), PartClass()))
                return Verdict(false, "fit: the client's hand did not take the part");
        }
        if (InStepMs() >= kHandMs) Go(Step::Fit);
        return;
    }
    case Step::Fit: {
        if (!g_acted) {
            g_acted = true;
            void* held = coop::hand_item::LocalHandActor();
            // A wheel fresh from its class can carry no durability, which a fit refuses as broken.
            const int32_t durOff = (wheel && held) ? R::FindPropertyOffset(R::ClassOf(held), L"durability") : -1;
            if (durOff >= 0) {
                float& durability = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(held) + durOff);
                if (durability <= 0.f) durability = 50.f;
            }
            const auto before = coop::atv_part_intent::GetStats();
            const bool ran = held && CallFit(g_atv.Get(), held);
            const auto st = coop::atv_part_intent::GetStats();
            void* after = coop::hand_item::LocalHandActor();
            UE_LOGI("[ATV-PART-DRILL] [C] ran the part's fit: dispatched=%d, asked of the host %llu, the hand is %s",
                    ran ? 1 : 0, static_cast<unsigned long long>(st.asked - before.asked),
                    (after && R::IsLive(after)) ? "still full" : "empty");
            if (st.asked == before.asked)
                return Verdict(false, "fit: the client's body was not cancelled and asked of the host");
            if (after && R::IsLive(after)) return Verdict(false, "fit: the part stayed in the client's hand");
        }
        const State want{g_base.mask, g_base.battery, g_base.props + 1};
        if (!Stands(want)) return;
        DC::Say(kSayDone);
        return Verdict(true, "the unbolt and the fit each stood on both peers: mask=0x%X battery fitted=%d, one part "
                       "prop more than at the start", want.mask, want.battery);
    }
    default:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected()) return;
    SayStateOnChange();
    if (g_step == Step::Done) return;
    if (Host()) TickHost(*session);
    else TickClient(*session);
}

void OnDisconnect() {
    g_step = Step::Start;
    g_stepMs = 0;
    g_acted = false;
    g_atv.Reset();
    g_key.clear();
    g_said = State{};
    g_base = State{};
}

}  // namespace coop::dev::atv_part_drill
