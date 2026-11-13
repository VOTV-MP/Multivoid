// coop/interactables/desk_short_sync.cpp -- see coop/interactables/desk_short_sync.h.

#include "coop/interactables/desk_short_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_short.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

namespace coop::desk_short_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace DSh = ue_wrap::desk_short;
namespace sg = ue_wrap::script_gate;
namespace N = coop::net;
using Payload = N::DeskShortPayload;

// The desk's own event: a stub that runs the ubergraph from the entry that tests `shortCircuiting`, sets it and starts
// the ten seconds of sparks. A liquid's Blueprint calls it, and so do we.
UE_FACT_FN(kRowShort, "desk_short_sync", L"analogDScreenTest_C", L"shortCircuit", Declared);
constexpr int kTagShort = 0x44530001;   // 'DS'

// The console lanes' reach, an arm's length, measured to the desk's colliding bounds (the same 400 as the desk's own
// verb lane, coop/interactables/desk_verb_intent.cpp); the authority adds the desk's bounds and its allowance for the
// sender's pose lagging, so a liquid thrown from across the room is inside it.
constexpr float kReachUU = 400.0f;

// A short lasts ten seconds, so a sender's asks are bounded to a burst of two and one per five seconds; a client past
// it costs the host no work, no log line and no answer (coop/net/intent_bucket.h).
constexpr N::IntentBudget kBudget{2.0f, 0.2f};
constexpr size_t kMaxPending = 4;

// An ask the host never answered, a client's held state and an ask the host has not yet served are dropped after the ten
// seconds the short they stand for would have run.
constexpr uint64_t kAskAgeMs = 10000;

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;    // the watch is registered (it lives for the process: it is keyed on names)
bool g_settled = false;    // its names resolved, and a dead one said
bool g_applying = false;   // OnState's own call of shortCircuit is running: its edge is the host's, not a liquid's
bool g_preKnown = false;   // the pre read the flag
bool g_preShorting = false;
uint64_t g_askAtMs = 0;    // CLIENT: when its cancelled short was asked; 0 when nothing waits for an answer
bool g_askUnsent = false;  // CLIENT: the ask has not left yet
bool g_held = false;       // CLIENT: the host's short waits for this copy's desk to resolve
uint64_t g_heldAtMs = 0;
uint32_t g_heldWorld = 0;         // CLIENT: the world generation the held state arrived in
bool g_unannouncedSaid = false;   // CLIENT: a short of a world not yet announced was cancelled, said once a session
Counts g_c;

// An ask carries nothing but its sender: the queue holds the time each ask arrived.
struct Slot {
    N::IntentQueue<uint64_t> queue;
    bool waitSaid = false;   // a wait for the sender's body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The drill's red arms: no short is announced (the host tells no client, and a client's own call runs alone), and a
// joiner is not told of a short under way.
bool Off() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::desk_short_off);
    return off;
}
bool ReplayOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::desk_short_replay_off);
    return off;
}

N::Session* Live() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

void* Desk() { return CD::EnsureResolved() ? CD::Instance() : nullptr; }

// What running the desk's own short on a copy did.
enum class Ran : uint8_t {
    Started,  // the call dispatched on a desk whose flag was clear
    Already,  // the flag was set: the event's own guard leaves it
    Failed,   // the desk or its flag does not read, or the call did not dispatch
};

Ran Short(void* desk) {
    bool now = false;
    if (!DSh::ReadShortCircuiting(desk, now)) return Ran::Failed;
    if (now) return Ran::Already;
    return DSh::CallShortCircuit(desk) ? Ran::Started : Ran::Failed;
}

// The host's own flag, for the state it tells one sender: false (a short that does not run) when it does not read.
bool HostShorts() {
    bool on = false;
    return DSh::ReadShortCircuiting(Desk(), on) && on;
}

// The body's entry. A client's call that is not the host's word is CANCELLED and asked of the host: its desk sparks only
// when the host's does. MTA's closest precedent, an explosion the client's engine already ran and reported
// (CGame.cpp:2831-2950: the joined gate, a server event that may cancel, the relay back to the reporter), lets the
// effect run because the engine fired it before the server heard; here the gate refuses first, as MTA's client-to-server
// events do (CGame.cpp:2663-2711: the client asks, the server decides). Divergences: the host judges reach, which MTA
// does not for explosions, and the state goes to every peer, since the base's power is global where MTA's goes to
// the cameras near the blast (CGame.cpp:85, 2943). A refusal is answered to its sender, which MTA's veto is not: the
// cancelled call has no result, so a silent drop would leave the client's liquid with no word.
sg::Verdict OnShortPre(const sg::Call& call) {
    bool on = false;
    g_preKnown = DSh::ReadShortCircuiting(call.object, on);
    g_preShorting = on;
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Client || g_applying || Off()) return sg::Verdict::Run;
    // A world not yet announced is the host's save loading here: the desk shorts only by DeskShortState, which reaches
    // this copy at its ready edge when the host's desk is shorting.
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) {
        if (!g_unannouncedSaid) {
            g_unannouncedSaid = true;
            UE_LOGI("desk_short: a liquid reached the desk before this world was announced -- cancelled, nothing asked");
        }
        return sg::Verdict::Cancel;
    }
    if (g_preKnown && on) return sg::Verdict::Run;   // the event's own guard: the call would do nothing
    if (g_askAtMs == 0) {
        g_askAtMs = NowMs();
        g_askUnsent = true;
        ++g_c.asked;
        UE_LOGI("desk_short: this player's liquid reached the desk -- cancelled, the host is asked (#%u)", g_c.asked);
    }
    return sg::Verdict::Cancel;
}

// The body's end, on the host: a flag that was clear and is set now is a short that began, and every client is told.
void OnShortPost(const sg::Call& call) {
    const bool known = g_preKnown, was = g_preShorting;
    g_preKnown = false;
    if (!known || was || Off()) return;
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host) return;
    bool on = false;
    if (!DSh::ReadShortCircuiting(call.object, on) || !on) return;
    Payload p{};
    p.on = 1;
    if (s->SendReliable(N::ReliableKind::DeskShortState, &p, sizeof(p))) {
        ++g_c.sent;
        UE_LOGI("desk_short: the host's desk shorts -- sent to every client (#%u)", g_c.sent);
    } else {
        UE_LOGI("desk_short: the host's desk shorts -- no client was told (none connected, or the send failed)");
    }
}

// The host's state, to one sender alone: its desk's flag as it is, so a refused ask's copy hears what is true.
void Answer(N::Session& s, uint8_t slot) {
    Payload p{};
    p.on = HostShorts() ? 1 : 0;
    if (s.SendReliableToSlot(slot, N::ReliableKind::DeskShortState, &p, sizeof(p))) ++g_c.answered;
}

void Refuse(N::Session& s, uint8_t slot, const char* why) {
    ++g_c.refused;
    UE_LOGW("desk_short: slot %u's ask refused -- %s (#%u)", static_cast<unsigned>(slot), why, g_c.refused);
    Answer(s, slot);
}

// A sender's head ask: the host judges it in the order its siblings do (the sender's world, that it lives, its reach),
// then runs the desk's own short. Wait keeps the ask queued while the host has no body to measure the reach from.
N::Served Execute(N::Session& s, uint8_t slot, uint64_t askedAtMs) {
    // An ask older than the short it stands for is dead: its client dropped it already, and a short started for it
    // would be one no liquid asked for.
    if (NowMs() - askedAtMs > kAskAgeMs) {
        ++g_c.stale;
        g_slots[slot].waitSaid = false;
        UE_LOGW("desk_short: slot %u's ask is older than %llu s -- dropped unserved (#%u)", static_cast<unsigned>(slot),
                static_cast<unsigned long long>(kAskAgeMs / 1000), g_c.stale);
        return N::Served::Done;
    }
    void* desk = Desk();
    if (!s.IsSlotWorldReady(slot) || !desk) {
        Refuse(s, slot, "its world is not up here, or this host has no desk");
        return N::Served::Done;
    }
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        Refuse(s, slot, "the sender is not alive");
        return N::Served::Done;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(desk);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("desk_short: slot %u's ask waits: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return N::Served::Wait;
    }
    g_slots[slot].waitSaid = false;
    if (!subject) {
        Refuse(s, slot, coop::element::OutcomeName(subject.outcome));
        return N::Served::Done;
    }
    switch (Short(desk)) {
    case Ran::Started:
        // The watch's end tells every client, the sender among them.
        ++g_c.served;
        UE_LOGI("desk_short: slot %u's liquid shorted the desk -- the host's desk shorts (#%u)",
                static_cast<unsigned>(slot), g_c.served);
        break;
    case Ran::Already:
        UE_LOGI("desk_short: slot %u's ask finds the host's desk shorting already -- it is told so", static_cast<unsigned>(slot));
        Answer(s, slot);
        break;
    case Ran::Failed:
        Refuse(s, slot, "the desk's flag does not read, or its short did not run");
        break;
    }
    return N::Served::Done;
}

// CLIENT: runs the host's short on this copy. False leaves it held until the desk resolves.
bool ApplyHostShort() {
    void* desk = Desk();
    if (!desk) return false;
    g_applying = true;
    const Ran ran = Short(desk);
    g_applying = false;
    if (ran == Ran::Failed) return false;
    if (ran == Ran::Started) {
        ++g_c.applied;
        UE_LOGI("desk_short: the host's desk shorts -- this desk shorts too (#%u)", g_c.applied);
    }
    return true;
}

}  // namespace

void Install(N::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = sg::WatchName(kRowShort, kTagShort, &OnShortPre, &OnShortPost);
    static bool sSaid = false;
    if (!g_watched && !sSaid) UE_LOGE("desk_short: the watch on the desk's shortCircuit was refused -- retried each tick");
    sSaid = !g_watched;
}

void Tick() {
    if (g_watched && !g_settled) {
        sg::ResolvePendingNames();
        g_settled = sg::NameWatchSettled(kRowShort, kTagShort);
        if (g_settled && !sg::NameWatchLive(kRowShort, kTagShort))
            UE_LOGE("desk_short: the watch on the desk's shortCircuit is dead -- a short reaches no other peer");
    }
    N::Session* s = Live();
    if (!s) return;
    if (s->role() == N::Role::Host) {
        for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot)
            g_slots[slot].queue.Serve(kBudget, NowMs, [&](const uint64_t& at) { return Execute(*s, slot, at); });
        return;
    }
    if (g_askAtMs != 0) {
        if (g_askUnsent) {
            Payload p{};
            p.on = 1;
            if (s->SendReliable(N::ReliableKind::DeskShortRequest, &p, sizeof(p))) g_askUnsent = false;
        }
        if (NowMs() - g_askAtMs > kAskAgeMs) {
            ++g_c.aged;
            UE_LOGW("desk_short: the host did not answer this player's short in %llu s -- dropped (#%u)",
                    static_cast<unsigned long long>(kAskAgeMs / 1000), g_c.aged);
            g_askAtMs = 0;
            g_askUnsent = false;
        }
    }
    // Nothing announces the desk's arrival, so a held state is retried each tick, and only while one is held. A held
    // short ends with the host's ten seconds and with the world it arrived in.
    if (g_held && (g_heldWorld != ue_wrap::world_identity::Generation() || NowMs() - g_heldAtMs > kAskAgeMs)) {
        g_held = false;
        ++g_c.lapsed;
        UE_LOGW("desk_short: the held short of the host lapsed before this desk resolved -- dropped (#%u)", g_c.lapsed);
    }
    if (g_held && ApplyHostShort()) {
        g_held = false;
        UE_LOGI("desk_short: the desk resolved -- the held short of the host runs (%llu ms after it arrived)",
                static_cast<unsigned long long>(NowMs() - g_heldAtMs));
    }
}

void OnState(const Payload& p) {
    // Any state is the host's word on the ask: its desk shorts (this copy shorts with it), or it does not.
    g_askAtMs = 0;
    g_askUnsent = false;
    if (!p.on) {
        g_held = false;   // the host's newest word replaces a short still held
        ++g_c.heard;
        UE_LOGI("desk_short: the host answered that its desk does not short -- this player's short is not run (#%u)",
                g_c.heard);
        return;
    }
    if (ApplyHostShort()) return;
    if (!g_held) {
        g_held = true;
        g_heldAtMs = NowMs();
        g_heldWorld = ue_wrap::world_identity::Generation();
        ++g_c.held;
        UE_LOGI("desk_short: the host's desk shorts but this copy's desk is not found or not readable -- held (#%u)",
                g_c.held);
    }
}

void OnRequest(uint8_t senderSlot) {
    if (senderSlot < 1 || senderSlot >= N::kMaxPeers) return;
    uint64_t ask = NowMs();
    if (!g_slots[senderSlot].queue.Push(std::move(ask), kMaxPending)) ++g_c.dropped;  // a flood: not answered, so no amplifier
}

void SendCurrentToSlot(int slot) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host || ReplayOff()) return;
    // Nothing announces a short to a peer that was not there: the host's flag is read once, here, at the ready edge. A
    // desk that does not read is not shorting, and the joiner's own copy holds what it is told until its desk resolves.
    if (!HostShorts()) return;
    Payload p{};
    p.on = 1;
    if (s->SendReliableToSlot(slot, N::ReliableKind::DeskShortState, &p, sizeof(p))) ++g_c.replayed;
    UE_LOGI("desk_short: slot %d's world is up while the host's desk shorts -- its desk shorts from the start (#%u)", slot,
            g_c.replayed);
}

Counts GetCounts() { return g_c; }

void OnDisconnect() {
    if (g_c.sent || g_c.asked || g_c.served || g_c.refused || g_c.answered || g_c.dropped || g_c.applied || g_c.heard ||
        g_c.aged || g_c.held || g_c.lapsed || g_c.stale || g_c.replayed)
        UE_LOGI("desk_short: session end -- host sent=%u served=%u refused=%u answered=%u dropped=%u stale=%u replayed=%u; "
                "client asked=%u applied=%u heard=%u aged=%u held=%u lapsed=%u",
                g_c.sent, g_c.served, g_c.refused, g_c.answered, g_c.dropped, g_c.stale, g_c.replayed, g_c.asked,
                g_c.applied, g_c.heard, g_c.aged, g_c.held, g_c.lapsed);
    g_c = Counts{};
    g_preKnown = g_preShorting = g_applying = g_askUnsent = g_held = g_unannouncedSaid = false;
    g_askAtMs = g_heldAtMs = 0;
    g_heldWorld = 0;
    g_session.store(nullptr);
}

}  // namespace coop::desk_short_sync
