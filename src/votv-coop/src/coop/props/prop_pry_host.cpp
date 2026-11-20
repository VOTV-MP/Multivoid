// coop/props/prop_pry_host.cpp -- see coop/props/prop_pry_host.h.

#include "coop/props/prop_pry_host.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"  // pry_judge_off, pry_coast_off
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"  // what the sender's hand holds, and held last
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/constraint_ties.h"  // AddBreakListener: the one patch on BreakConstraint
#include "coop/props/prop_drive_host.h"  // OverrideClientSyncer: a write to the copy ends a client's row on it
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_pry_sync.h"
#include "coop/props/prop_sound.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_pry.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::prop_pry_host {
namespace {

namespace R  = ue_wrap::reflection;
namespace PP = ue_wrap::prop_pry;
namespace PS = coop::prop_pry_sync;

// The reach a sender's pry is judged against: twice mainPlayer.armLength (200), the door lane's bound for a verb
// made by looking at the target, to which the authority adds the prop's bounds and a 600 uu allowance for the puppet's
// pose lagging its sender (coop/element/intent_authority).
constexpr float kPryReachUU = 400.0f;

// A pry is a minigame of seconds, so a sender's honest rate is a few a minute: the bound is only felt by a stalled or
// flooding client.
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t   kMaxPending   = 8;
constexpr uint64_t kRefusalSayMs = 10000;

enum class Why : uint8_t { NoProp, OutOfReach, NoTarget, NoCrowbar, Already, NotStuck, Failed, QueueFull };

const char* WhyName(Why w) {
    switch (w) {
    case Why::NoProp:     return "the key names no pryable prop on the host";
    case Why::OutOfReach: return "the prop is out of the sender's reach";
    case Why::NoTarget:   return "the prop's place does not read, so its reach cannot be measured";
    case Why::NoCrowbar:  return "the sender's hand did not hold a crowbar last";
    case Why::Already:    return "the prop is already pried";
    case Why::NotStuck:   return "the host's copy is no longer static or frozen";
    case Why::Failed:     return "the pry did not run on the host's copy";
    case Why::QueueFull:  return "the sender's queue is full";
    }
    return "?";
}

// One sender: its queued asks with their bucket, and the latches that clear with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::PryAskPayload> queue;
    uint64_t nextSayMs = 0;  // a refusal is said once per kRefusalSayMs
    bool     waitSaid  = false;  // a wait for the sender's body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

Counts g_counts{};

// A refusal: counted, and said once per slot per kRefusalSayMs.
void Refuse(uint8_t slot, const std::wstring& key, Why why) {
    ++g_counts.refused;
    if (why == Why::OutOfReach) ++g_counts.farRefused;
    Slot& sl = g_slots[slot];
    const uint64_t now = coop::net::NowMs();
    if (now < sl.nextSayMs) return;
    sl.nextSayMs = now + kRefusalSayMs;
    UE_LOGW("prop_pry_host: slot %u's pry of '%ls' refused -- %s (#%llu this session)", static_cast<unsigned>(slot),
            key.c_str(), WhyName(why), static_cast<unsigned long long>(g_counts.refused));
}

// A pry on the host's copy. A plank: the flags its crowbarOpen clears, the sound it plays and the kick along the crowbar's
// right vector the client read, renormalised -- a client's kick is a direction, never a magnitude. A radiator or a
// security TV: its own crowbarOpen, whose breakingSequence draws its own shuffle and spin on the host's body alone and
// reaches the clients as the joints its BreakConstraint breaks (OnBreak).
bool RunPry(void* prop, const coop::net::PryAskPayload& p) {
    // The pry writes the copy's flags and physics, so a client that syncs the prop loses it first, as to a host verb. A
    // no-op for a prop with no client row.
    coop::prop_drive_host::OverrideClientSyncer(prop, "pry");
    if (PP::KindOf(prop) == PP::Kind::Hanging) return PS::RunOwn(prop);
    float x = p.kickX, y = p.kickY, z = p.kickZ;
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-3f) { x /= len; y /= len; z /= len; } else { x = y = z = 0.f; }
    if (!PP::FreePlank(prop, x, y, z)) return false;
    coop::prop_sound::PlayDamagedSound(prop);
    PS::MarkPried(prop);
    return true;
}

// UPhysicsConstraintComponent::BreakConstraint, native, so the gate cannot see it: constraint_ties' post patch on its Func
// fires for every route into it, the Blueprint's own call among them, and tells this listener. The host's break of a
// radiator's or a TV's joint goes to the clients as the joint, in the host's order.
bool g_hostActive = false;  // the host's session runs: set by Tick, read by the listener
bool g_listening = false;   // the listener is on constraint_ties

void OnBreak(void* constraint) {
    if (!constraint || !g_hostActive) return;
    void* owner = R::OuterOf(constraint);
    const int joint = PP::JointOf(owner, constraint);
    if (joint == 0) return;
    PS::NoteBroken(owner, joint);
    // The loop's random spin follows this call on the host's body, and the radiator hangs between breaks long enough for
    // a row to end (kRestMs, 500, against 1.5 to 3 s), so each break opens or re-times a row of its own, whatever the body
    // is doing at this instant: CoastLetGo would refuse a body at rest. Every copy parks on the row's first pose again.
    static const bool s_coastOff = coop::config::ResolveFlag(coop::config_registry::rows::pry_coast_off);  // the drill's red
    if (!s_coastOff && !coop::prop_drive_host::CoastCapReached(owner, "pry-break"))
        coop::prop_drive_host::Coast(owner, "pry-break");
}

// False keeps the ask at its sender's queue head: the host has no body for the sender yet (its first pose has not
// arrived), so its reach cannot be measured, and the ask waits for the body rather than being lost. Everything else is
// run or refused, and consumed.
bool Execute(coop::net::Session& s, const coop::net::PryAskPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* prop = coop::prop_key_index::Find(key);
    if (!prop || !R::IsLive(prop) || PP::KindOf(prop) == PP::Kind::None) {
        Refuse(slot, key, Why::NoProp);
        return true;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kPryReachUU).Authorize(prop);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("prop_pry_host: slot %u's pry waits: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return false;
    }
    // The pried drill's far red arm: the reach is not checked, so an out-of-reach ask runs. Resolved once.
    static const bool s_judgeOff = coop::config::ResolveFlag(coop::config_registry::rows::pry_judge_off);
    if (!subject && !s_judgeOff) {
        Refuse(slot, key, subject.outcome == coop::element::IntentOutcome::NoTarget ? Why::NoTarget : Why::OutOfReach);
        return true;
    }
    // The prying crowbar destroys the held one as it goes into the prop, so a pry ends with the hand empty: the sender's
    // hand must have held a crowbar last. The hand state announced ahead of the ask on the one ordered lane is applied
    // first, so an ask that rests on the hand it was sent from is judged against it (container_break's order).
    coop::hand_item::ApplyOwedNow(s, slot);
    if (!coop::hand_item::LastHeldClassIs(slot, L"prop_crowbar_C")) {
        Refuse(slot, key, Why::NoCrowbar);
        return true;
    }
    PS::ForgetIfStale(prop);
    if (PS::IsPried(key)) {
        Refuse(slot, key, Why::Already);
        return true;
    }
    // A reload clears the record and not the game's flags: a copy that is no longer stuck is pried, whatever the record.
    if (!ue_wrap::prop::IsStatic(prop) && !ue_wrap::prop::IsFrozen(prop)) {
        Refuse(slot, key, Why::NotStuck);
        return true;
    }
    if (!RunPry(prop, p)) {
        Refuse(slot, key, Why::Failed);
        return true;
    }
    ++g_counts.granted;
    UE_LOGI("prop_pry_host: slot %u's pry of '%ls' GRANTED (#%llu)", static_cast<unsigned>(slot), key.c_str(),
            static_cast<unsigned long long>(g_counts.granted));
    return true;
}

}  // namespace

void OnAsk(coop::net::Session&, const coop::net::PryAskPayload& payload, uint8_t senderSlot) {
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    coop::net::PryAskPayload copy = payload;
    if (!sl.queue.Push(std::move(copy), kMaxPending))
        Refuse(senderSlot, coop::net::StringFromWireKey(payload.key), Why::QueueFull);
}

void Tick(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("prop_pry_host::Tick");
    g_hostActive = session.running() && session.role() == coop::net::Role::Host;
    // The host reports joints from constraint_ties' one patch on BreakConstraint.
    if (!g_listening) g_listening = coop::constraint_ties::AddBreakListener(&OnBreak);
    if (!g_hostActive) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, coop::net::NowMs, [&](const coop::net::PryAskPayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;  // a consumed ask ends the wait's streak
            return coop::net::Served::Done;
        });
    }
}

Counts GetCounts() { return g_counts; }

void OnDisconnect() {
    if (g_counts.granted || g_counts.refused)
        UE_LOGI("prop_pry_host: session end -- granted=%llu refused=%llu (out of reach %llu)",
                static_cast<unsigned long long>(g_counts.granted), static_cast<unsigned long long>(g_counts.refused),
                static_cast<unsigned long long>(g_counts.farRefused));
    g_counts = Counts{};
    g_hostActive = false;
}

}  // namespace coop::prop_pry_host
