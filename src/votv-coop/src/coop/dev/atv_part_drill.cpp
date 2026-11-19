// coop/dev/atv_part_drill.cpp -- see coop/dev/atv_part_drill.h.

#include "coop/dev/atv_part_drill.h"

#include "atv_spawn_near.h"  // SpawnAtNav, KeyOf (co-located private header)

#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/interactables/atv_part_intent.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

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
#include <cwchar>
#include <string>

namespace coop::dev::atv_part_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace AC = ue_wrap::atv_condition;
namespace AP = ue_wrap::atv_parts;
namespace DC = coop::dev::drill_chat;
namespace SN = coop::dev::atv_spawn_near;
namespace RV = coop::rig_verdict;
using ue_wrap::FVector;

enum class Arm { Off, Wheel };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::atv_part_drill);
        return v == "wheel" ? Arm::Wheel : Arm::Off;
    }();
    return a;
}

bool Host() { return coop::roster::LocalIsHost(); }
char Who() { return Host() ? 'H' : 'C'; }

constexpr const char* kTag = "atv-part";  // the rig's verdict tag
constexpr uint8_t  kNeedBits    = 0x07;    // the wheels the legs touch (0, 1, 2) are on at the start
constexpr int      kSlot        = 1;       // the rig's client; the only peer that acts
constexpr float    kAheadCm     = 220.f;   // the ATV is born this far ahead of the host's player
// A bound on a wait for an event: a run that never reaches it is INVALID, never a verdict.
constexpr uint64_t kWaitBoundMs  = 30000;
constexpr uint64_t kStartBoundMs = 90000;
const char* const  kSayKey   = "atvpart key ";
const char* const  kSayState = "atvpart state ";
const char* const  kSayHand  = "atvpart hand ";
const char* const  kSayCount = "atvpart cnt ";
const char* const  kSayEnd   = "atvpart end ";

constexpr const wchar_t* kPartClass = L"prop_atvWheel_C";
constexpr const wchar_t* kPartItem = L"atvwheel";
constexpr const wchar_t* kWrenchClass = L"prop_wrenchtool_C";
constexpr const wchar_t* kPowerToolClass = L"prop_powertool_C";

// What a peer's copy holds: the tyre mask and the wheel's world props (a hand item and its mirrors are
// no world props).
struct State {
    int mask = -1, props = -1;
    bool operator==(const State& o) const { return mask == o.mask && props == o.props; }
};

// What the host's hand lane says the client's hand holds.
enum class Hand { None, Wrench, Wheel, Keep };

const char* HandWord(Hand h) {
    switch (h) {
    case Hand::None:   return "none";
    case Hand::Wrench: return "wrench";
    case Hand::Wheel:  return "wheel";
    default:           return "other";
    }
}

enum class Act { Unscrew, Fit, ForgedFit };

// One leg: the hand it needs, the act, the wheel, and what the host must do with it. The wheels differ from one
// leg to the next, so the client's own repeat guard never swallows an ask.
struct Leg {
    const char* name;
    Hand hand;
    Act act;
    int wheel;
    bool hostRuns;
};
constexpr Leg kLegs[] = {
    {"unbolt with an empty hand", Hand::None, Act::Unscrew, 2, false},
    {"unbolt wheel 0 with the wrench", Hand::Wrench, Act::Unscrew, 0, true},
    {"unbolt wheel 1 with the wrench", Hand::Wrench, Act::Unscrew, 1, true},
    {"fit the held wheel into socket 0", Hand::Wheel, Act::Fit, 0, true},
    {"a second fit on the hold the first spent", Hand::Keep, Act::ForgedFit, 1, false},
};
constexpr int kLegCount = static_cast<int>(sizeof(kLegs) / sizeof(kLegs[0]));

constexpr unsigned long long ExpectedRan() {
    unsigned long long n = 0;
    for (const Leg& l : kLegs) n += l.hostRuns ? 1 : 0;
    return n;
}
constexpr unsigned long long ExpectedRefused() { return kLegCount - ExpectedRan(); }

ue_wrap::CachedObjRef g_atv;
std::wstring g_key;

int PartProps() {
    int n = 0;
    for (void* a : R::FindObjectsByClass(kPartClass))
        if (a && R::IsLive(a) && !R::NameStartsWith(R::NameOf(a), L"Default__") &&
            !coop::hand_item::IsHandAxisActor(a))
            ++n;
    return n;
}

// This peer's copy as the last poll read it (the walk of the wheel props is dev-only and runs at the poll's pace).
State g_mine{};
bool g_haveMine = false;
State g_said{};
Hand g_saidHand = Hand::None;
bool g_saidHandAny = false;
bool g_saidCount = false;
coop::atv_part_intent::Stats g_saidStats{};
coop::atv_part_intent::Stats g_statsAtStart{};
bool g_named = false;  // HOST: the ATV is named and the counters' baseline taken

bool ReadState(State& out) {
    void* atv = g_atv.Get();
    AC::Snapshot snap;
    if (!atv || (!AC::Resolved() && !AC::Resolve()) || !AC::Read(atv, snap)) return false;
    out = State{snap.mask, PartProps()};
    return true;
}

bool TheirState(State& out) {
    std::string rest;
    const std::string prefix = std::string(kSayState) + (Host() ? "C " : "H ");
    if (!DC::SeenRestOf(prefix, &rest)) return false;
    return sscanf_s(rest.c_str(), "%d %d", &out.mask, &out.props) == 2;
}

struct Counts {
    unsigned long long ran = 0, refused = 0, refunded = 0;
};

// CLIENT: the host's counters, as it last said them.
bool HostCounts(Counts& out) {
    std::string rest;
    if (!DC::SeenRestOf(kSayCount, &rest)) return false;
    return sscanf_s(rest.c_str(), "%llu %llu %llu", &out.ran, &out.refused, &out.refunded) == 3;
}

// HOST: what the client's hand holds, by the lane that judges it.
Hand HostHandKind() {
    if (!coop::hand_item::HoldsItem(kSlot)) return Hand::None;
    if (coop::hand_item::HeldClassIs(kSlot, kWrenchClass) || coop::hand_item::HeldClassIs(kSlot, kPowerToolClass))
        return Hand::Wrench;
    return coop::hand_item::HeldClassIs(kSlot, kPartClass) ? Hand::Wheel : Hand::Keep;
}

// CLIENT: the hand the host last said.
bool HostSaysHand(Hand want) {
    std::string rest;
    return DC::SeenRestOf(kSayHand, &rest) && rest == HandWord(want);
}

// Each peer reads its copy and says what changed, at the poll's pace. The host also says its hand lane's view of
// the client's hand and its counters: the events the client's legs wait on.
void Poll() {
    State now{};
    if (ReadState(now)) {
        g_mine = now;
        g_haveMine = true;
        if (!(now == g_said)) {
            g_said = now;
            char line[64];
            std::snprintf(line, sizeof(line), "%s%c %d %d", kSayState, Who(), now.mask, now.props);
            DC::Say(line);
            UE_LOGI("[ATV-PART-DRILL] [%c] state mask=0x%X wheel props=%d", Who(), now.mask, now.props);
        }
    }
    if (!Host() || !g_named) return;
    const Hand hand = HostHandKind();
    if (!g_saidHandAny || hand != g_saidHand) {
        g_saidHandAny = true;
        g_saidHand = hand;
        DC::Say(std::string(kSayHand) + HandWord(hand));
        UE_LOGI("[ATV-PART-DRILL] [H] the client's hand holds: %s", HandWord(hand));
    }
    const auto st = coop::atv_part_intent::GetStats();
    if (!g_saidCount || st.ran != g_saidStats.ran || st.refused != g_saidStats.refused ||
        st.refunded != g_saidStats.refunded) {
        g_saidCount = true;
        g_saidStats = st;
        char line[96];
        std::snprintf(line, sizeof(line), "%s%llu %llu %llu", kSayCount,
                      static_cast<unsigned long long>(st.ran - g_statsAtStart.ran),
                      static_cast<unsigned long long>(st.refused - g_statsAtStart.refused),
                      static_cast<unsigned long long>(st.refunded - g_statsAtStart.refunded));
        DC::Say(line);
    }
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

enum class Step { Start, Named, Hand, Act, Await, Done };
Step g_step = Step::Start;
uint64_t g_stepMs = 0;
bool g_acted = false;
int g_leg = 0;
State g_legStart{};
Counts g_legCounts{};
coop::atv_part_intent::Stats g_legAsked{};
bool g_ended = false;
Hand g_equipped = Hand::Keep;  // CLIENT: the hand this drill last took (Keep: none yet)

void Go(Step s) {
    g_step = s;
    g_stepMs = coop::net::NowMs();
    g_acted = false;
}
uint64_t InStepMs() { return coop::net::NowMs() - g_stepMs; }

// This peer's verdict, said once, and told to the other peer, which says its own: the host's log carries the run's
// verdict whichever peer decided it. The client's PASS is not said here: the host's PASS rests on its own counters.
void End(RV::Outcome o, const char* fmt, ...) {
    if (g_ended) return;
    g_ended = true;
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    RV::Say(kTag, o, "%s", msg);
    char line[160];
    std::snprintf(line, sizeof(line), "%s%c %c %.100s", kSayEnd, Who(), o == RV::Outcome::Fail ? 'F' : 'I', msg);
    DC::Say(line);
    Go(Step::Done);
}

// The other peer's end: its FAIL or INVALID is this peer's verdict too; the client's `P` is judged by the host.
bool AdoptOtherEnd() {
    std::string rest;
    const std::string prefix = std::string(kSayEnd) + (Host() ? "C " : "H ");
    if (!DC::SeenRestOf(prefix, &rest) || rest.empty() || g_ended) return false;
    const char kind = rest[0];
    const std::string text = rest.size() > 2 ? rest.substr(2) : std::string();
    g_ended = true;
    Go(Step::Done);
    if (kind == 'P') {
        const auto st = coop::atv_part_intent::GetStats();
        const unsigned long long ran = st.ran - g_statsAtStart.ran, refused = st.refused - g_statsAtStart.refused,
                                 refunded = st.refunded - g_statsAtStart.refunded;
        if (ran == ExpectedRan() && refused == ExpectedRefused() && refunded == 0)
            RV::Say(kTag, RV::Outcome::Pass, "the host ran %llu asks, refused %llu and gave nothing back; every leg "
                    "stood on both peers", ran, refused);
        else
            RV::Say(kTag, RV::Outcome::Fail, "the host ran %llu and refused %llu and refunded %llu (%llu, %llu and 0 "
                    "wanted)", ran, refused, refunded, ExpectedRan(), ExpectedRefused());
        return true;
    }
    RV::Say(kTag, kind == 'F' ? RV::Outcome::Fail : RV::Outcome::Invalid, "told by the %s: %s",
            Host() ? "client" : "host", text.c_str());
    return true;
}

// ---- The host: the fixture, then the events it says ----------------------------------------------

void TickHost(coop::net::Session& s) {
    void* me = coop::players::Registry::Get().Local();
    if (g_step != Step::Start) return;
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
        if (!sp.actor)
            return End(RV::Outcome::Invalid, "fixture: no ATV could be spawned beside the host (%s, got %s)",
                       sp.what ? sp.what : "?", sp.got);
        g_atv.Set(sp.actor);
        return;
    }
    const char* why = nullptr;
    const SN::Key k = SN::KeyOf(g_atv.Get(), &g_key, &why);
    if (k == SN::Key::Bad)
        return End(RV::Outcome::Invalid, "fixture: the spawned ATV's key cannot ride a chat line (%s)", why);
    if (k != SN::Key::Ok) {
        if (InStepMs() > kStartBoundMs)
            End(RV::Outcome::Invalid, "fixture: the lane's table never held the spawned ATV");
        return;
    }
    // The wheels the legs work on are on the ATV to start with.
    AC::Snapshot snap;
    void* atv = g_atv.Get();
    if ((!AC::Resolved() && !AC::Resolve()) || !AC::Read(atv, snap))
        return End(RV::Outcome::Invalid, "fixture: the spawned ATV's condition does not read");
    if ((snap.mask & kNeedBits) != kNeedBits) {
        AC::WritePresence(atv, static_cast<uint8_t>(snap.mask | kNeedBits), snap.hasSpare);
        AC::CallVerb(atv, AC::Verb::UpdTires);
    }
    g_statsAtStart = coop::atv_part_intent::GetStats();
    g_named = true;
    DC::Say(std::string(kSayKey) + std::string(g_key.begin(), g_key.end()));
    UE_LOGI("[ATV-PART-DRILL] [H] spawned the ATV '%ls' (mask=0x%X) and named it", g_key.c_str(), snap.mask);
    Go(Step::Named);
}

// ---- The client: the legs --------------------------------------------------------------------------

bool CallFit(void* atv, void* held, int wheel) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(atv), L"putTire");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<int32_t>(L"index", wheel) && f.Set<void*>(L"wheelObject", held) && ue_wrap::Call(atv, f);
}

State WantAfter(const Leg& l, const State& from) {
    const int bit = 1 << l.wheel;
    if (!l.hostRuns) return from;
    return l.act == Act::Unscrew ? State{from.mask & ~bit, from.props + 1} : State{from.mask | bit, from.props};
}

// The hand a leg needs: taken here, then the host's hand lane must say it holds it.
bool TakeHand(Hand h) {
    switch (h) {
    case Hand::None:   return coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
    case Hand::Wrench: return coop::dev::drill_hand::TakeIntoHandNow(L"tool_w", kWrenchClass);
    case Hand::Wheel:  return coop::dev::drill_hand::TakeIntoHandNow(kPartItem, kPartClass);
    default:           return true;
    }
}

// The act of a leg. False when the leg is decided (a verdict said) and the sequence stops.
bool DoAct(coop::net::Session& s, const Leg& l) {
    const auto before = coop::atv_part_intent::GetStats();
    if (l.act == Act::ForgedFit) {
        // A fit asked with no wheel in this client's hand: what a client that lies sends.
        coop::net::AtvPartIntentPayload p{};
        coop::net::WireKeyFromString(g_key, p.key);
        p.op = coop::net::atv_part::kFitWheel;
        p.index = static_cast<uint8_t>(l.wheel);
        p.durability = 50.f;
        if (!s.SendReliable(coop::net::ReliableKind::AtvPartIntent, &p, sizeof(p))) {
            End(RV::Outcome::Invalid, "%s: the session refused the forged ask", l.name);
            return false;
        }
        UE_LOGI("[ATV-PART-DRILL] [C] sent a second fit of socket %d with no wheel held", l.wheel);
        return true;
    }
    bool dispatched = false;
    if (l.act == Act::Unscrew) {
        dispatched = AP::RunUnscrew(g_atv.Get(), l.wheel);
    } else {
        void* held = coop::hand_item::LocalHandActor();
        // A wheel fresh from its class can carry no durability, which a fit refuses as broken.
        const int32_t durOff = held ? R::FindPropertyOffset(R::ClassOf(held), L"durability") : -1;
        if (durOff >= 0) {
            float& durability = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(held) + durOff);
            if (durability <= 0.f) durability = 50.f;
        }
        dispatched = held && CallFit(g_atv.Get(), held, l.wheel);
    }
    const auto st = coop::atv_part_intent::GetStats();
    UE_LOGI("[ATV-PART-DRILL] [C] leg '%s': dispatched=%d, asked of the host %llu", l.name, dispatched ? 1 : 0,
            static_cast<unsigned long long>(st.asked - before.asked));
    if (st.asked == before.asked) {
        End(RV::Outcome::Fail, "%s: the client's body was not cancelled and asked of the host (it ran on this copy)",
            l.name);
        return false;
    }
    if (l.act == Act::Fit) {
        void* after = coop::hand_item::LocalHandActor();
        if (after && R::IsLive(after)) {
            End(RV::Outcome::Fail, "%s: the part stayed in the client's hand", l.name);
            return false;
        }
    }
    return true;
}

void TickClient(coop::net::Session& s) {
    switch (g_step) {
    case Step::Start: {
        if (!JoinOver(s)) return;
        if (!g_stepMs) g_stepMs = coop::net::NowMs();
        std::string rest;
        coop::atv_sync::TableView view{};
        State theirs{};
        Counts counts{};
        const bool named = DC::SeenRestOf(kSayKey, &rest);
        if (named && !g_atv.Get()) {
            g_key = std::wstring(rest.begin(), rest.end());
            if (coop::atv_sync::ReadTable(g_key, view) && view.actor) g_atv.Set(view.actor);
        }
        const bool ready = g_atv.Get() && AP::Resolve() && g_haveMine && TheirState(theirs) && theirs == g_mine &&
                           HostCounts(counts) && coop::atv_sync::ReadTable(g_key, view) && view.syncerSlot == 0 &&
                           (g_mine.mask & kNeedBits) == kNeedBits;
        if (!ready) {
            if (InStepMs() > kStartBoundMs)
                End(RV::Outcome::Invalid, "fixture: the named ATV with its wheels on never stood on both peers under "
                    "the host's sync (named=%d resolved=%d here mask=0x%X props=%d, there mask=0x%X props=%d)",
                    named ? 1 : 0, g_atv.Get() ? 1 : 0, g_mine.mask, g_mine.props, theirs.mask, theirs.props);
            return;
        }
        g_leg = 0;
        return Go(Step::Hand);
    }
    case Step::Hand: {
        const Leg& l = kLegs[g_leg];
        if (l.hand != Hand::Keep) {
            if (!g_acted) {
                g_acted = true;
                // A hand already holding the leg's item is left alone: taking the same tool up again destroys
                // and respawns its hold, which a player's next unbolt never does.
                if (l.hand != g_equipped) {
                    if (!TakeHand(l.hand))
                        return End(RV::Outcome::Invalid, "%s: the client's hand did not take the %s", l.name,
                                   HandWord(l.hand));
                    g_equipped = l.hand;
                }
            }
            // Readiness is the host's own word on the hand, never a wait of so many seconds.
            if (!HostSaysHand(l.hand)) {
                if (InStepMs() > kWaitBoundMs)
                    End(RV::Outcome::Invalid, "%s: the host's hand lane never said the hand held the %s", l.name,
                        HandWord(l.hand));
                return;
            }
        }
        return Go(Step::Act);
    }
    case Step::Act: {
        const Leg& l = kLegs[g_leg];
        if (!g_haveMine || !HostCounts(g_legCounts)) return;
        g_legStart = g_mine;
        if (!DoAct(s, l)) return;
        return Go(Step::Await);
    }
    case Step::Await: {
        const Leg& l = kLegs[g_leg];
        Counts c{};
        State theirs{};
        // The event: the host judged the ask (its ran or refused counter moved).
        if (!HostCounts(c) || (c.ran == g_legCounts.ran && c.refused == g_legCounts.refused)) {
            if (InStepMs() > kWaitBoundMs)
                End(RV::Outcome::Invalid, "%s: the host's counters never moved for the ask", l.name);
            return;
        }
        if (l.hostRuns && c.refused != g_legCounts.refused)
            return End(RV::Outcome::Fail, "%s: the host refused an ask it had to run (its DENY line says why)", l.name);
        if (!l.hostRuns && c.ran != g_legCounts.ran)
            return End(RV::Outcome::Fail, "%s: the host ran an ask it had to refuse", l.name);
        if (c.refunded != g_legCounts.refunded)
            return End(RV::Outcome::Fail, "%s: the host gave a part back that no part was owed for", l.name);
        const State want = WantAfter(l, g_legStart);
        if (!(g_haveMine && TheirState(theirs) && g_mine == want && theirs == want)) {
            if (InStepMs() > kWaitBoundMs)
                End(RV::Outcome::Invalid, "%s: wanted mask=0x%X props=%d on both peers; here mask=0x%X props=%d, "
                    "there mask=0x%X props=%d", l.name, want.mask, want.props, g_mine.mask, g_mine.props,
                    theirs.mask, theirs.props);
            return;
        }
        UE_LOGI("[ATV-PART-DRILL] [C] leg '%s' stands on both peers: mask=0x%X wheel props=%d", l.name, want.mask,
                want.props);
        if (++g_leg < kLegCount) return Go(Step::Hand);
        DC::Say(std::string(kSayEnd) + "C P");
        UE_LOGI("[ATV-PART-DRILL] [C] every leg held; the host judges its own counters");
        g_ended = true;
        return Go(Step::Done);
    }
    default:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected()) return;
    if (DC::PollDue()) Poll();
    if (g_step == Step::Done) return;
    if (AdoptOtherEnd()) return;
    if (Host()) TickHost(*session);
    else TickClient(*session);
}

void OnDisconnect() {
    g_step = Step::Start;
    g_stepMs = 0;
    g_acted = false;
    g_leg = 0;
    g_ended = false;
    g_equipped = Hand::Keep;
    g_named = false;
    g_atv.Reset();
    g_key.clear();
    g_said = State{};
    g_mine = State{};
    g_haveMine = false;
    g_saidHand = Hand::None;
    g_saidHandAny = false;
    g_saidCount = false;
}

}  // namespace coop::dev::atv_part_drill
