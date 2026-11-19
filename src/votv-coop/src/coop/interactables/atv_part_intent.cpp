// coop/interactables/atv_part_intent.cpp -- see coop/interactables/atv_part_intent.h for the design.

#include "coop/interactables/atv_part_intent.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_sync_internal.h"  // the row's expressed baseline, reseeded after the host's change
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"  // the sender's tool and part; this peer's own hand actor
#include "coop/player/hand_spend.h"  // the part a hold spends: one use per hold
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: an ATV of a world still loading is not asked about
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_condition.h"
#include "ue_wrap/devices/atv_parts.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>

namespace coop::atv_part_intent {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace AC = ue_wrap::atv_condition;
namespace AP = ue_wrap::atv_parts;
namespace sg = ue_wrap::script_gate;
namespace op = coop::net::atv_part;

// The four bodies of ATV_C that change which wheels it carries.
UE_FACT_FN(kRowUnscrewPanel, "atv_part_intent", L"ATV_C", L"unscrewPanel", Declared);
UE_FACT_FN(kRowPutTire, "atv_part_intent", L"ATV_C", L"putTire", Declared);
UE_FACT_FN(kRowPlayerUsedOn, "atv_part_intent", L"ATV_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowActionOptionIndex, "atv_part_intent", L"ATV_C", L"actionOptionIndex", Declared);
constexpr int kTagPart = 0x41545650;  // 'ATVP'

constexpr int      kWheels = 4;
constexpr uint8_t  kActionTakeSpare = 7;   // the radial option that hands out the spare
// Twice mainPlayer.armLength, the same reach the ATV's seat and grab asks use (atv_authority.cpp, kReachUU); the
// authority adds the ATV's bounds.
constexpr float    kReachUU = 400.0f;
constexpr float    kBesideCm = 70.0f;      // a part given to a sender is born this far ahead of its puppet
constexpr float    kBesideUpCm = 20.0f;
// The repair countdown's domain, the one the condition read measured (atv_condition.h, Read); a client's claim is
// held to it, since the host has no copy of the wheel a client's hand held.
constexpr int32_t  kFixesMin = -1;
constexpr int32_t  kFixesMax = 3;
constexpr uint64_t kRepeatMs = 700;        // a second ask of one unbolt or spare take inside this is not sent
constexpr coop::net::IntentBudget kBudget{6.0f, 2.0f};

constexpr const wchar_t* kWheelClass   = L"prop_atvWheel_C";

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchAsked = false;
uint32_t g_registered = 0;  // bit i: row i's watch registered (game thread)
bool g_settled = false;
Stats g_stats{};

// HOST, per sender: its budget. The part a fit or stow spends is counted by hand_spend, one use per hold.
coop::net::IntentBucket g_buckets[coop::net::kMaxPeers];

// CLIENT: the last unbolt or spare take asked, so a body the game re-runs every tick asks once.
struct LastAsk {
    std::wstring key;
    uint8_t  what = 0xFF;
    uint8_t  index = 0;
    uint64_t ms = 0;
};
LastAsk g_lastAsk;

const char* OpName(uint8_t o) {
    switch (o) {
    case op::kRemoveWheel:   return "unbolt a wheel";
    case op::kFitWheel:      return "fit a wheel";
    case op::kStowSpare:     return "stow the spare";
    case op::kTakeSpare:     return "take the spare";
    default:                 return "?";
    }
}

bool TakesHeldPart(uint8_t o) { return o == op::kFitWheel || o == op::kStowSpare; }

int8_t ClampI8(int32_t v) { return static_cast<int8_t>(v < -128 ? -128 : v > 127 ? 127 : v); }
float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// The part drill's red arms (config row atv_part_red), read here only, by the rule each turns off.
bool Red(const char* value) { return coop::config::ResolveEnum(::coop::config_registry::rows::atv_part_red) == value; }
bool CancelOff() { static const bool on = Red("cancel_off"); return on; }        // a client's bodies are not cancelled
bool ToolJudgeOff() { static const bool on = Red("tool_judge_off"); return on; }  // the host does not check the tool

// ---- CLIENT: the bodies cancelled and asked of the host ----------------------------------------

coop::net::Session* ClientSession() {
    if (CancelOff()) return nullptr;
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected() && s->role() == coop::net::Role::Client) ? s : nullptr;
}

// An ATV of this client's announced world whose key the wire can carry, with both layouts resolved.
bool Askable(const sg::Call& c, std::wstring& key) {
    if (!c.object || !ue_wrap::atv::IsAtv(c.object) || !AP::Resolve()) return false;
    if (!AC::Resolved() && !AC::Resolve()) return false;
    if (!coop::net_pump::IsInAnnouncedWorld(c.object)) return false;
    key = ue_wrap::atv::GetKeyString(c.object);
    return coop::atv_sync::AnnounceableAtvKey(key);
}

template <class T>
bool Param(const sg::Call& c, const wchar_t* name, T& out) {
    const int32_t off = c.locals ? R::FindParamOffset(c.function, name) : -1;
    if (off < 0) return false;
    out = *reinterpret_cast<const T*>(c.locals + off);
    return true;
}

// One intent to the host. False when the session refused the send, said.
bool Ask(coop::net::Session& s, const std::wstring& key, uint8_t what, uint8_t index, const AP::Wheel* wheel) {
    coop::net::AtvPartIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = what;
    p.index = index;
    if (wheel) {
        p.durability = wheel->durability;
        p.dirt = wheel->dirt;
        p.fixes = ClampI8(wheel->fixes);
    }
    if (!s.SendReliable(coop::net::ReliableKind::AtvPartIntent, &p, sizeof(p))) {
        UE_LOGW("atv_part: the ask to %s on '%ls' was not sent (the session refused it); the ATV stays as the "
                "host has it", OpName(what), key.c_str());
        return false;
    }
    ++g_stats.asked;
    UE_LOGI("atv_part: CLIENT asked the host to %s on '%ls' (index %u)", OpName(what), key.c_str(), index);
    return true;
}

// An unbolt or a spare take, asked once per kRepeatMs.
void AskOnce(coop::net::Session& s, const std::wstring& key, uint8_t what, uint8_t index) {
    const uint64_t now = coop::atv_sync::NowMs();
    if (g_lastAsk.what == what && g_lastAsk.index == index && g_lastAsk.key == key && now - g_lastAsk.ms < kRepeatMs)
        return;
    if (Ask(s, key, what, index, nullptr)) g_lastAsk = LastAsk{key, what, index, now};
}

// The wrench's last step on a looked-at wheel leaves through the host; the body's own resetUnscrew, which a
// cancelled body does not reach, is run here so the progress bar and its timer end. A socket's fit goes on to
// putTire, watched below. With no wheel looked at the step is the battery's, which atv_gate asks (AtvRequest),
// and it runs.
sg::Verdict OnUnscrewPre(const sg::Call& c) {
    auto* s = ClientSession();
    std::wstring key;
    AP::Look look{};
    if (!s || !Askable(c, key) || !AP::ReadLook(c.object, look)) return sg::Verdict::Run;
    if (look.tire < 0 || look.tire >= kWheels) return sg::Verdict::Run;
    AskOnce(*s, key, op::kRemoveWheel, static_cast<uint8_t>(look.tire));
    AP::ResetUnscrew(c.object);
    return sg::Verdict::Cancel;
}

// The held wheel leaves this client's hand as the body would take it, once its ask is on the wire.
sg::Verdict GiveHeldPart(coop::net::Session& s, const std::wstring& key, uint8_t what, uint8_t index, void* held,
                         const AP::Wheel* wheel) {
    if (Ask(s, key, what, index, wheel)) E::DestroyActor(held);
    return sg::Verdict::Cancel;
}

// putTire(index, wheelObject): the wheel in this client's hand into an empty socket. An occupied
// socket and a bad index run, for the body's own hint.
sg::Verdict OnPutTirePre(const sg::Call& c) {
    auto* s = ClientSession();
    std::wstring key;
    int32_t index = -1;
    void* wheel = nullptr;
    AC::Snapshot snap;
    AP::Wheel w{};
    if (!s || !Askable(c, key) || !Param(c, L"index", index) || !Param(c, L"wheelObject", wheel))
        return sg::Verdict::Run;
    if (index < 0 || index >= kWheels || !wheel || !R::IsLive(wheel) || !AP::ReadWheel(wheel, w))
        return sg::Verdict::Run;
    if (!AC::Read(c.object, snap) || (snap.mask & (1u << index)) != 0) return sg::Verdict::Run;
    if (!coop::hand_item::IsLocalHandViewActor(wheel)) {
        UE_LOGW("atv_part: putTire on '%ls' with a wheel that is not this client's hand item -- the body runs here "
                "alone", key.c_str());
        return sg::Verdict::Run;
    }
    return GiveHeldPart(*s, key, op::kFitWheel, static_cast<uint8_t>(index), wheel, &w);
}

// playerUsedOn: of its branches, a held wheel on the spare box of an ATV with no spare. The trunk, an
// upgrade, a battery (atv_gate's), a wheel on a socket (putTire above), the tools (unscrewPanel above) and
// every hint run.
sg::Verdict OnPlayerUsedOnPre(const sg::Call& c) {
    auto* s = ClientSession();
    std::wstring key;
    AP::Look look{};
    void* player = nullptr;
    if (!s || !Askable(c, key) || !AP::ReadLook(c.object, look) || look.container) return sg::Verdict::Run;
    if (!Param(c, L"player", player) || player != coop::players::Registry::Get().Local()) return sg::Verdict::Run;
    void* held = coop::hand_item::LocalHandActor();
    if (!held || !R::IsLive(held)) return sg::Verdict::Run;
    AC::Snapshot snap;
    if (!AC::Read(c.object, snap)) return sg::Verdict::Run;
    AP::Wheel w{};
    if (!AP::ReadWheel(held, w) || !look.spareBox || snap.hasSpare) return sg::Verdict::Run;
    return GiveHeldPart(*s, key, op::kStowSpare, 0, held, &w);
}

// actionOptionIndex: the spare's take. An empty box runs, for the body's own hint.
sg::Verdict OnActionPre(const sg::Call& c) {
    auto* s = ClientSession();
    std::wstring key;
    uint8_t action = 0;
    AC::Snapshot snap;
    if (!s || !Askable(c, key) || !Param(c, L"action", action) || action != kActionTakeSpare)
        return sg::Verdict::Run;
    if (!AC::Read(c.object, snap) || !snap.hasSpare) return sg::Verdict::Run;
    AskOnce(*s, key, op::kTakeSpare, 0);
    return sg::Verdict::Cancel;
}

// The four watches, one row each: a refused registration leaves the others standing.
constexpr int kRowCount = 4;
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowUnscrewPanel, &kRowPutTire, &kRowPlayerUsedOn,
                                                          &kRowActionOptionIndex};
const sg::PreFn kPres[kRowCount] = {&OnUnscrewPre, &OnPutTirePre, &OnPlayerUsedOnPre, &OnActionPre};
const char* const kRowTag[kRowCount] = {"unbolt", "fit", "spare stow", "spare take"};

// ---- HOST: the judge and the change --------------------------------------------------------------

// The tool is known by the class the hand lane records for the hold, as the house judges do: the item name the
// hold slot carries is rewritten by the game when the hand takes the same tool up again, the class is not.
bool HoldsUnscrewTool(uint8_t slot) {
    return coop::hand_item::HeldClassIs(slot, L"prop_wrenchtool_C") ||
           coop::hand_item::HeldClassIs(slot, L"prop_powertool_C");
}

bool SpotBesideSender(uint8_t slot, ue_wrap::FVector& out) {
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(slot);
    void* body = puppet ? puppet->GetActor() : nullptr;
    ue_wrap::FVector at{};
    if (!body || !E::TryGetActorLocation(body, at)) return false;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(body);
    out = ue_wrap::FVector{at.X + fwd.X * kBesideCm, at.Y + fwd.Y * kBesideCm, at.Z + kBesideUpCm};
    return true;
}

// The wheel a client's ask names, every scalar within the game's own domain: the one place a client's
// claim becomes the host's.
AP::Wheel WheelOf(const coop::net::AtvPartIntentPayload& p) {
    const int32_t fixes = p.fixes < kFixesMin ? kFixesMin : p.fixes > kFixesMax ? kFixesMax : p.fixes;
    return AP::Wheel{Clamp(p.durability, 0.0f, 100.0f), Clamp(p.dirt, 0.0f, 1.0f), fixes};
}

// The wheel a refused fit or stow took out of the sender's hand, born again beside the sender.
bool Refund(uint8_t slot, const coop::net::AtvPartIntentPayload& p) {
    ue_wrap::FVector at{};
    if (!AP::Resolve() || !SpotBesideSender(slot, at)) return false;
    void* part = AP::SpawnWheel(at, WheelOf(p));
    if (part) ++g_stats.refunded;
    return part != nullptr;
}

// What the ATV's own state and the sender's hand say against the op; null when they allow it.
const char* Judge(const coop::net::AtvPartIntentPayload& p, const AC::Snapshot& snap, uint8_t slot) {
    const uint8_t bit = static_cast<uint8_t>(1u << p.index);
    switch (p.op) {
    case op::kRemoveWheel:
        if (!(snap.mask & bit)) return "that wheel is off already";
        return ToolJudgeOff() || HoldsUnscrewTool(slot) ? nullptr : "the sender's hand holds no wrench or power tool";
    case op::kFitWheel:
        if (snap.mask & bit) return "the socket is occupied";
        return p.durability > 0.0f ? nullptr : "the wheel is broken";
    case op::kStowSpare:
        return snap.hasSpare ? "the spare box is occupied" : nullptr;
    case op::kTakeSpare:
        return snap.hasSpare ? nullptr : "the spare box is empty";
    default:
        return "malformed";
    }
}

// How far a change got on the host's copy. A part the sender gave up is returned only for kNothing: a
// refund after kPartly would be a second part beside the one the ATV already holds.
enum class Got { kNothing, kPartly, kAll };

// Whether the ATV's presence now reads as the op leaves it. An ATV that does not read back counts as landed: a refund
// beside a part the ATV may already hold could duplicate it, and no refund at worst loses one part the host never took.
bool Landed(void* atv, uint8_t what, uint8_t bit) {
    AC::Snapshot now;
    if (!AC::Read(atv, now)) return true;
    switch (what) {
    case op::kFitWheel:   return (now.mask & bit) != 0;
    case op::kStowSpare:  return now.hasSpare;
    case op::kTakeSpare:  return !now.hasSpare;
    default:              return false;
    }
}

// The change on the host's copy: the game's own unscrew for a part that leaves, the fields the body
// writes and its reducer for one that arrives. A spare's take clears the box first and spawns its wheel
// last, giving the spare back when no wheel came, so no failure leaves both a spare and a prop.
Got Run(void* atv, const coop::net::AtvPartIntentPayload& p, AC::Snapshot snap, uint8_t slot) {
    const uint8_t bit = static_cast<uint8_t>(1u << p.index);
    AC::Snapshot after;
    switch (p.op) {
    case op::kRemoveWheel:
        return AP::RunUnscrew(atv, p.index) && AC::Read(atv, after) && !(after.mask & bit) ? Got::kAll
                                                                                           : Got::kNothing;
    case op::kFitWheel: {
        const AP::Wheel w = WheelOf(p);
        snap.mask |= bit;
        snap.dur[p.index] = w.durability;
        snap.dirt[p.index] = w.dirt;
        snap.fixes[p.index] = static_cast<int8_t>(w.fixes);
        if (AC::WriteAccumulators(atv, snap) && AC::WritePresence(atv, snap.mask, snap.hasSpare) &&
            AC::CallVerb(atv, AC::Verb::UpdTires))
            return Got::kAll;
        return Landed(atv, p.op, bit) ? Got::kPartly : Got::kNothing;
    }
    case op::kStowSpare: {
        const AP::Wheel w = WheelOf(p);
        snap.spareDur = w.durability;
        snap.spareDirt = w.dirt;
        snap.spareFixes = static_cast<int8_t>(w.fixes);
        if (AC::WriteAccumulators(atv, snap) && AC::WritePresence(atv, snap.mask, true) &&
            AC::CallVerb(atv, AC::Verb::UpdSpareTire))
            return Got::kAll;
        return Landed(atv, p.op, bit) ? Got::kPartly : Got::kNothing;
    }
    case op::kTakeSpare: {
        ue_wrap::FVector at{};
        if (!SpotBesideSender(slot, at)) return Got::kNothing;
        if (!AC::WritePresence(atv, snap.mask, false) && !Landed(atv, p.op, bit)) return Got::kNothing;
        const bool shown = AC::CallVerb(atv, AC::Verb::UpdSpareTire);
        if (AP::SpawnWheel(at, AP::Wheel{snap.spareDur, snap.spareDirt, snap.spareFixes}))
            return shown ? Got::kAll : Got::kPartly;
        const bool back = AC::WritePresence(atv, snap.mask, true) && AC::CallVerb(atv, AC::Verb::UpdSpareTire);
        return back ? Got::kNothing : Got::kPartly;
    }
    default:
        return Got::kNothing;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchAsked) return;
    g_watchAsked = true;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (sg::WatchName(*kRows[i], kTagPart, kPres[i], nullptr)) {
            g_registered |= 1u << i;
            ++watched;
        } else {
            UE_LOGE("atv_part: the watch on ATV_C::%ls did not register -- a client's %s changes only its own copy",
                    kRows[i]->Name(), kRowTag[i]);
        }
    }
    UE_LOGI("atv_part: %d/%d ATV watches registered by class and name", watched, kRowCount);
}

void SettleWatches() {
    if (g_settled || !g_registered) return;
    for (int i = 0; i < kRowCount; ++i)
        if ((g_registered & (1u << i)) && !sg::NameWatchSettled(*kRows[i], kTagPart)) return;
    g_settled = true;
    for (int i = 0; i < kRowCount; ++i)
        if ((g_registered & (1u << i)) && !sg::NameWatchLive(*kRows[i], kTagPart))
            UE_LOGW("atv_part: the watch on ATV_C::%ls settled dead -- a client's %s changes only its own copy",
                    kRows[i]->Name(), kRowTag[i]);
}

void OnIntent(coop::net::Session& session, const coop::net::AtvPartIntentPayload& payload, uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("atv_part_intent::OnIntent");
    if (session.role() != coop::net::Role::Host || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    if (!session.IsSlotWorldReady(senderSlot)) return;  // a client asks nothing before its world; a forged send
    const std::wstring key = coop::net::StringFromWireKey(payload.key);
    const bool wheelOp = payload.op == op::kRemoveWheel || payload.op == op::kFitWheel;
    const char* why = nullptr;
    bool partOwed = false;  // a part left the sender's hand for this ask: a refusal gives it back
    coop::element::IntentSubject subject{};
    coop::atv_sync::TableView view{};
    AC::Snapshot snap;
    // A part-less op names no wheel, so its index is 0; Judge and Run shift by it.
    if (payload.op >= op::kOpCount || (wheelOp ? payload.index >= kWheels : payload.index != 0) ||
        !std::isfinite(payload.durability) || !std::isfinite(payload.dirt)) {
        why = "malformed";
    } else {
        // The hand is judged before the pace: the client destroyed its part when it asked, so a paced
        // ask that did carry a part owes it back. The hold owed by the hand lane's pace, announced ahead
        // of this ask on the one ordered lane, is applied first, so the ask is judged against the hand it
        // was sent from. One use per hold keeps a flood from minting more parts.
        coop::hand_item::ApplyOwedNow(session, senderSlot);
        if (TakesHeldPart(payload.op)) {
            if (!coop::hand_spend::Spend(senderSlot, kWheelClass, 1))
                why = "the sender's hand held no part of that hold to give";
            else
                partOwed = true;
        }
        if (!why && !g_buckets[senderSlot].Take(kBudget, coop::atv_sync::NowMs())) why = "paced";
    }
    if (!why) {
        if (!coop::atv_sync::ReadTable(key, view) || !view.actor) {
            why = "no live ATV by that key";
        } else if (!(subject = coop::element::IntentTarget::ForClientIntent(session, senderSlot, kReachUU)
                                   .Authorize(view.actor))) {
            why = coop::element::OutcomeName(subject.outcome);
        } else if (view.syncerSlot != 0) {
            // Presence reaches the clients from the host's own copy, which the stream carries only while
            // the host syncs the ATV.
            why = "a client syncs the ATV (it is driven or carried)";
        } else if (!AP::Resolve() || (!AC::Resolved() && !AC::Resolve()) || !AC::Read(view.actor, snap)) {
            why = "the ATV's condition does not read";
        } else {
            why = Judge(payload, snap, senderSlot);
        }
    }
    Got got = Got::kNothing;
    if (!why) {
        got = Run(view.actor, payload, snap, senderSlot);
        if (got == Got::kNothing) why = "the change did not take on the host's copy";
    }
    if (why) {
        ++g_stats.refused;
        const bool refunded = partOwed && Refund(senderSlot, payload);
        UE_LOGI("atv_part: DENY slot=%u %s on '%ls' index=%u -- %s (%.0f uu of %.0f)%s",
                static_cast<unsigned>(senderSlot), OpName(payload.op), key.c_str(), payload.index, why,
                subject.distUU, subject.reachUU,
                !partOwed ? "" : refunded ? "; the part is given back beside the sender"
                                          : "; the part could NOT be given back");
        return;
    }
    ++g_stats.ran;
    if (got == Got::kPartly)
        UE_LOGW("atv_part: slot %u's ask to %s on '%ls' took only partly on the host's copy -- nothing is "
                "given back, since a second part would duplicate the one the change already placed",
                static_cast<unsigned>(senderSlot), OpName(payload.op), key.c_str());
    // The host's copy changed under the row's expressed baseline: the next block applied to it seeds
    // the baseline from the actor again.
    if (auto* e = coop::atv_sync::FindEntryMutable(key)) e->condExpressedSeeded = false;
    UE_LOGI("atv_part: host ran slot %u's ask to %s on '%ls' index=%u (was mask=%02X spare=%d)",
            static_cast<unsigned>(senderSlot), OpName(payload.op), key.c_str(), payload.index, snap.mask,
            snap.hasSpare ? 1 : 0);
}

Stats GetStats() { return g_stats; }

void OnDisconnect() {
    if (g_stats.asked || g_stats.ran || g_stats.refused)
        UE_LOGI("atv_part: session end -- asked=%llu ran=%llu refused=%llu refunded=%llu",
                static_cast<unsigned long long>(g_stats.asked), static_cast<unsigned long long>(g_stats.ran),
                static_cast<unsigned long long>(g_stats.refused), static_cast<unsigned long long>(g_stats.refunded));
    g_stats = Stats{};
    g_lastAsk = LastAsk{};
    for (auto& b : g_buckets) b.Reset();
}

}  // namespace coop::atv_part_intent
