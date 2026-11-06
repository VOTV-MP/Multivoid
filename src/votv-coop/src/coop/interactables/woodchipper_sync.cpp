// coop/interactables/woodchipper_sync.cpp -- see coop/interactables/woodchipper_sync.h.

#include "coop/interactables/woodchipper_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_key_index.h"
#include "coop/requester_feedback/requester_feedback.h"

#include "ue_wrap/actors/woodchipper.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::woodchipper_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace W  = ue_wrap::woodchipper;
namespace N  = coop::net;
using Clock = std::chrono::steady_clock;
using Payload = N::WoodchipperPayload;

// The two bodies whose end announces the machine's state to the host: checkActive (every pour, every press with fuel in
// the tank, the burn's switch-off) and actionOptionIndex (a press at fuel 0 sets `active` and skips checkActive).
UE_FACT_FN(kRowCheckActive, "woodchipper_sync", L"woodchipper_C", L"checkActive", Declared);
UE_FACT_FN(kRowPress, "woodchipper_sync", L"woodchipper_C", L"actionOptionIndex", Declared);
constexpr int kTagCheckActive = 0x57435341;   // 'WCSA'
constexpr int kTagPress = 0x57435350;         // 'WCSP'

// A player's use reaches as far as the look-at trace, whose length is mainPlayer.armLength: 200 by default. Twice the
// default is the reach; the authority adds the machine's bounds radius and a 600 uu allowance for the puppet's pose
// lagging the sender (coop/element/intent_authority), as the cremator lever's and the coffee press's judges do.
constexpr float kReachUU = 400.0f;
// A sender's intents run from a bounded queue at a bounded rate (the cremator lever's and the coffee press's bound).
constexpr N::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t kMaxPending = 8;
constexpr uint64_t kRefusalSayMs = 10000;
constexpr auto kBurnPeriod = std::chrono::seconds(1);
constexpr const wchar_t* kCanClass = L"prop_gascan_C";

std::atomic<N::Session*> g_session{nullptr};

N::Session* Running(N::Role role) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == role ? s : nullptr;
}

// The drill's red arms: the host's pour judge off (no can, no room check), and the join replay off.
bool PourCheckOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::woodchipper_pour_check_off);
    return off;
}
bool ReplayOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::woodchipper_replay_off);
    return off;
}

bool WireKeyable(const std::wstring& key) { return !key.empty() && key.size() <= sizeof(N::WireKey::data); }
bool ValidFuel(float f) { return std::isfinite(f) && f >= 0.f && f <= W::kMaxFuel; }
bool ValidAmount(float f) { return std::isfinite(f) && f > 0.f && f <= W::kMaxFuel; }

Payload MakeState(const std::wstring& key, float fuel, bool active, uint8_t op) {
    Payload p{};
    N::WireKeyFromString(key, p.key);
    p.fuel = std::fmin(std::fmax(fuel, 0.f), W::kMaxFuel);
    p.active = active ? 1 : 0;
    p.op = op;
    return p;
}

DrillView g_view;

// ---- the host's table: the chippers this lane has heard of, and what each peer was last told ----

struct Tracked {
    ue_wrap::CachedObjRef actor;
    std::wstring key;
    float sentFuel = -1.f;
    bool sentActive = false;
    bool dirty = false;               // a body announced a change since the last send
    Clock::time_point nextCheck{};    // the burn's poll, once a second a chipper
};
std::vector<Tracked> g_tracked;

Tracked* FindTracked(void* chipper) {
    for (Tracked& t : g_tracked)
        if (t.actor.Is(chipper)) return &t;
    return nullptr;
}

// A chipper joins the table at its first change, whoever caused it, and leaves at its destroy.
Tracked* Track(void* chipper) {
    if (Tracked* t = FindTracked(chipper)) return t;
    std::wstring key = W::KeyOf(chipper);
    if (!WireKeyable(key)) return nullptr;
    Tracked t;
    t.actor.Set(chipper);
    t.key = std::move(key);
    g_tracked.push_back(std::move(t));
    return &g_tracked.back();
}

void NoteChange(void* chipper) {
    if (Tracked* t = Track(chipper)) t->dirty = true;
}

void OnCheckActivePost(const sg::Call& call) {
    if (Running(N::Role::Host)) NoteChange(call.object);
}

void OnPressPost(const sg::Call& call) {
    if (!Running(N::Role::Host) || !call.locals) return;
    static R::ParamOffset sAction{L"action"};
    const int32_t off = sAction.Of(call.function);
    if (off >= 0 && call.locals[off] == W::kButtonAction) NoteChange(call.object);
}

// The table's sends. `slot` -1 is every client. Said on the first send of a chipper and at each on/off edge.
void SendState(N::Session& s, int slot, const Tracked& t, float fuel, bool active, bool announce) {
    const Payload p = MakeState(t.key, fuel, active, N::kWoodchipperOpState);
    if (slot < 0) s.SendReliable(N::ReliableKind::WoodchipperState, &p, sizeof(p));
    else s.SendReliableToSlot(slot, N::ReliableKind::WoodchipperState, &p, sizeof(p));
    ++g_view.statesSent;
    if (announce)
        UE_LOGI("woodchipper: '%ls' is %s with %.1f fuel", t.key.c_str(), active ? "on" : "off", p.fuel);
}

void Flush(N::Session& s, Clock::time_point now) {
    if (g_tracked.empty()) return;
    const bool peers = s.AnyWorldReadyPeer();
    for (size_t i = 0; i < g_tracked.size();) {
        Tracked& t = g_tracked[i];
        if (!t.dirty && now < t.nextCheck) {
            ++i;
            continue;
        }
        float fuel = 0.f;
        bool active = false;
        void* c = t.actor.Get();
        if (!c || !W::Read(c, fuel, active)) {
            g_tracked.erase(g_tracked.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        // The one poll here: nothing announces the burn, which writes `fuel` every frame of the machine's tick with no
        // seam to watch. It reads a chipper once a second, and sends only a fuel or an on/off that moved since the last
        // send, so a machine at rest, or one switched on with nothing to burn, costs a read and no message.
        const bool moved = fuel != t.sentFuel || active != t.sentActive;
        t.nextCheck = now + kBurnPeriod;
        if (!t.dirty && !moved) {
            ++i;
            continue;
        }
        if (peers) {
            SendState(s, -1, t, fuel, active, t.sentFuel < 0.f || active != t.sentActive);
            t.sentFuel = fuel;
            t.sentActive = active;
        }
        t.dirty = false;
        ++i;
    }
}

// ---- the host's judge of a client's intent ----

enum Reason : uint8_t {
    kNotWorldReady, kNoChipper, kWrongClass, kSenderDead, kOutOfReach, kNoCan, kTankFull, kAlready, kNotRun,
    kQueueFull, kReasonCount
};
const char* const kReasonName[kReasonCount] = {
    "the sender's world is not ready", "no prop holds the key", "the key names no woodchipper",
    "the sender's player is not alive", "the sender is not within reach", "the sender's hand holds no gas can",
    "the tank has no room", "the chipper is already in the state the press wanted", "the host's copy did not take the call",
    "the sender's queue is full",
};
uint64_t g_refused[kReasonCount] = {};
struct Said { uint64_t nextMs = 0; uint64_t since = 0; };
Said g_said[kReasonCount];

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

void Refuse(Reason why, uint8_t slot, const std::wstring& key, const char* more) {
    ++g_refused[why];
    switch (why) {
    case kOutOfReach: ++g_view.refusedReach; break;
    case kNoCan: ++g_view.refusedCan; break;
    case kTankFull: ++g_view.refusedFull; break;
    case kAlready: ++g_view.refusedAlready; break;
    default: ++g_view.refusedOther; break;
    }
    Said& sd = g_said[why];
    const uint64_t now = NowMs();
    if (now < sd.nextMs) { ++sd.since; return; }
    UE_LOGI("woodchipper: DENY slot=%u key='%ls' -- %s%s%s (%llu more since the last line)", static_cast<unsigned>(slot),
            key.c_str(), kReasonName[why], more ? ": " : "", more ? more : "", static_cast<unsigned long long>(sd.since));
    sd.nextMs = now + kRefusalSayMs;
    sd.since = 0;
}

// The host's answer to a refused intent: the state, to the sender alone, so its copy goes back to the host's.
void Answer(N::Session& s, uint8_t slot, void* chipper) {
    float fuel = 0.f;
    bool active = false;
    Tracked* t = Track(chipper);
    if (!t || !W::Read(chipper, fuel, active)) return;
    SendState(s, slot, *t, fuel, active, false);
    ++g_view.answers;
}

struct Slot {
    N::IntentQueue<Payload> queue;
    bool waitSaid = false;   // a wait for the body or the feedback seams, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

bool Pour(N::Session& s, const Payload& p, uint8_t slot, void* chipper, const std::wstring& key) {
    float fuel = 0.f;
    bool active = false;
    if (!W::Read(chipper, fuel, active)) {
        Refuse(kNotRun, slot, key, "unreadable");
        return true;
    }
    float add = p.fuel;
    if (!PourCheckOff()) {
        if (!coop::hand_item::HeldClassIs(slot, kCanClass)) {
            Refuse(kNoCan, slot, key, nullptr);
            Answer(s, slot, chipper);
            return true;
        }
        // The client's copy ran the pour as the game wrote it, so the amount is the fuel its own can gave; the host's
        // copy of that can is the hand mirror, which carries no amount, so the bound is the tank's room.
        const float room = W::kMaxFuel - fuel;
        if (!(room > 0.f)) {
            Refuse(kTankFull, slot, key, nullptr);
            Answer(s, slot, chipper);
            return true;
        }
        if (add > room) {
            ++g_view.clamped;
            UE_LOGI("woodchipper: slot %u poured %.1f into '%ls' with room for %.1f -- %.1f of it is not taken",
                    static_cast<unsigned>(slot), add, key.c_str(), room, add - room);
            add = room;
        }
    }
    // gascanFuel's own end: the fuel in, then checkActive (its POST marks the chipper changed).
    if (!W::WriteFuel(chipper, fuel + add) || !W::CallCheckActive(chipper)) {
        Refuse(kNotRun, slot, key, "the pour");
        return true;
    }
    NoteChange(chipper);
    ++g_view.pours;
    UE_LOGI("woodchipper: slot %u poured %.1f into '%ls' on the host: %.1f -> %.1f (#%llu)", static_cast<unsigned>(slot),
            add, key.c_str(), fuel, fuel + add, g_view.pours);
    return true;
}

// False keeps the intent at its sender's queue head, as the cremator's judge does: the host has no body for the sender
// yet, or requester_feedback's seams are still settling. Everything else is run or refused, and consumed.
bool SetActive(N::Session& s, const Payload& p, uint8_t slot, void* chipper, const std::wstring& key) {
    float fuel = 0.f;
    bool active = false;
    if (!W::Read(chipper, fuel, active)) {
        Refuse(kNotRun, slot, key, "unreadable");
        return true;
    }
    const bool wanted = p.active != 0;
    if (active == wanted) {   // the copy trailed the host by a send: the press must not invert the machine
        Refuse(kAlready, slot, key, wanted ? "on" : "off");
        Answer(s, slot, chipper);
        return true;
    }
    if (coop::requester_feedback::Refused()) {
        Refuse(kNotRun, slot, key, "requester_feedback's seams are refused");
        Answer(s, slot, chipper);
        return true;
    }
    if (!coop::requester_feedback::Ready()) return false;
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;   // Authorize found it
    bool ran = false;
    {
        // The game's own press, as the cremator lever's and the coffee press's judges run theirs: the sender's puppet as
        // the player, whose click sound and hints go to the sender.
        coop::requester_feedback::Scope feedback(slot, 0);
        ran = W::CallPress(chipper, body);
    }
    // A press at fuel 0 sets `active` and the machine's tick never switches it off; the state is sent either way.
    NoteChange(chipper);
    if (!ran) {
        Refuse(kNotRun, slot, key, "the press");
        Answer(s, slot, chipper);
        return true;
    }
    ++g_view.presses;
    UE_LOGI("woodchipper: host ran slot %u's press on '%ls' (wanted %s, fuel %.1f) (#%llu)", static_cast<unsigned>(slot),
            key.c_str(), wanted ? "on" : "off", fuel, g_view.presses);
    return true;
}

bool Execute(N::Session& s, const Payload& p, uint8_t slot) {
    const std::wstring key = N::StringFromWireKey(p.key);
    if (!s.IsSlotWorldReady(slot)) {
        Refuse(kNotWorldReady, slot, key, nullptr);
        return true;
    }
    void* chipper = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    if (!chipper) {
        Refuse(kNoChipper, slot, key, nullptr);
        return true;
    }
    // Authorize answers reach only; the key is the sender's word, so the actor it names must be a chipper.
    if (!W::IsChipper(chipper)) {
        Refuse(kWrongClass, slot, key, nullptr);
        return true;
    }
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        Refuse(kSenderDead, slot, key, nullptr);
        return true;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(chipper);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("woodchipper: slot %u's intents wait: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        Refuse(kOutOfReach, slot, key, coop::element::OutcomeName(subject.outcome));
        Answer(s, slot, chipper);
        return true;
    }
    return p.op == N::kWoodchipperOpPour ? Pour(s, p, slot, chipper, key) : SetActive(s, p, slot, chipper, key);
}

void Serve(N::Session& s) {
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, NowMs, [&](const Payload& p) {
            if (!Execute(s, p, slot)) return N::Served::Wait;   // a wait runs nothing: no token spent
            sl.waitSaid = false;
            return N::Served::Done;
        });
    }
}

// ---- the client's held states ----

struct Held {
    float fuel = 0.f;
    bool hasFuel = false;
    bool active = false;
};
std::unordered_map<std::wstring, Held> g_held;
uint64_t g_heldVersion = 0;

// True when the state is dealt with: written onto its chipper, or dropped because the key names no chipper. False
// leaves it held until the prop lane resolves the key.
bool TryApply(const std::wstring& key, const Held& h) {
    void* c = coop::prop_key_index::Find(key);
    if (!c) return false;
    if (!W::IsChipper(c)) {
        UE_LOGW("woodchipper: the host's state names '%ls', which is no woodchipper here -- dropped", key.c_str());
        return true;
    }
    float fuel = 0.f;
    bool was = false;
    if (!W::Read(c, fuel, was)) return false;
    if (h.hasFuel) W::WriteFuel(c, h.fuel);
    W::WriteActive(c, h.active);
    ++g_view.applied;
    if (was != h.active)
        UE_LOGI("woodchipper: the host's '%ls' is %s here (%.1f fuel)", key.c_str(), h.active ? "on" : "off",
                h.hasFuel ? h.fuel : fuel);
    return true;
}

void RetryHeld() {
    if (g_held.empty()) return;
    const uint64_t version = coop::prop_key_index::Version();
    if (version == g_heldVersion) return;   // a read that missed cannot answer otherwise until the index moves
    g_heldVersion = version;
    for (auto it = g_held.begin(); it != g_held.end();)
        it = TryApply(it->first, it->second) ? g_held.erase(it) : std::next(it);
    g_view.held = g_held.size();
}

// ---- the watches ----

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg g_regCheck = Reg::Pending, g_regPress = Reg::Pending;
bool g_saidLive = false;

void RegisterOnce() {
    struct Def { const ue_wrap::facts::Function& row; int tag; sg::PostFn post; Reg& reg; };
    const Def defs[] = {
        {kRowCheckActive, kTagCheckActive, &OnCheckActivePost, g_regCheck},
        {kRowPress, kTagPress, &OnPressPost, g_regPress},
    };
    for (const Def& d : defs) {
        if (d.reg != Reg::Pending) continue;
        if (sg::WatchName(d.row, d.tag, nullptr, d.post)) {
            d.reg = Reg::Registered;
        } else {
            d.reg = Reg::Refused;
            UE_LOGE("woodchipper: the script-body gate refused the watch on %ls::%ls -- the host's chipper is not "
                    "announced for the rest of this process", d.row.Class(), d.row.Name());
        }
    }
}

void SettleWatches() {
    if (g_saidLive || g_regCheck != Reg::Registered || g_regPress != Reg::Registered) return;
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRowCheckActive, kTagCheckActive) && sg::NameWatchLive(kRowPress, kTagPress)) {
        g_saidLive = true;
        UE_LOGI("woodchipper: the chipper's checkActive and button press are watched");
    }
}

struct ReplayCtx {
    N::Session* s;
    int slot;
};

void EachForReplay(void* ctx, void* chipper) {
    ReplayCtx& c = *static_cast<ReplayCtx*>(ctx);
    float fuel = 0.f;
    bool active = false;
    if (!W::Read(chipper, fuel, active) || !active) return;
    Tracked* t = Track(chipper);
    if (!t) return;
    // `active` alone: a chipper's fuel at the join is the prop record lane's, which this send follows.
    const Payload p = MakeState(t->key, 0.f, true, N::kWoodchipperOpActiveOnly);
    c.s->SendReliableToSlot(c.slot, N::ReliableKind::WoodchipperState, &p, sizeof(p));
    ++g_view.replayed;
    UE_LOGI("woodchipper: '%ls' is on -- sent to the joiner in slot %d", t->key.c_str(), c.slot);
}

}  // namespace

void Install(N::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::requester_feedback::Install(session);
    RegisterOnce();
}

void Tick() {
    RegisterOnce();
    SettleWatches();
    if (N::Session* s = Running(N::Role::Host)) {
        Serve(*s);
        Flush(*s, Clock::now());
    } else if (Running(N::Role::Client)) {
        RetryHeld();
    }
}

void OnState(const Payload& p) {
    if ((p.op != N::kWoodchipperOpState && p.op != N::kWoodchipperOpActiveOnly) || p.active > 1) return;
    if (p.op == N::kWoodchipperOpState && !ValidFuel(p.fuel)) return;
    const std::wstring key = N::StringFromWireKey(p.key);
    if (key.empty()) return;
    // The newest state per key: an op 0 carries both fields, an op 3 only `active`, which leaves a fuel already held.
    Held h = g_held.count(key) ? g_held[key] : Held{};
    if (p.op == N::kWoodchipperOpState) {
        h.fuel = p.fuel;
        h.hasFuel = true;
    }
    h.active = p.active != 0;
    if (TryApply(key, h)) {
        g_held.erase(key);
    } else {
        g_held[key] = h;
        g_heldVersion = coop::prop_key_index::Version();
    }
    g_view.held = g_held.size();
}

void OnIntent(int senderSlot, const Payload& p) {
    if (!Running(N::Role::Host) || senderSlot < 1 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    const bool pour = p.op == N::kWoodchipperOpPour && ValidAmount(p.fuel);
    const bool set = p.op == N::kWoodchipperOpSetActive && p.active <= 1;
    const std::wstring key = N::StringFromWireKey(p.key);
    if ((!pour && !set) || key.empty()) {
        Refuse(kNotRun, static_cast<uint8_t>(senderSlot), key, "a malformed intent");
        return;
    }
    Payload copy = p;
    if (!g_slots[static_cast<uint8_t>(senderSlot)].queue.Push(std::move(copy), kMaxPending))
        Refuse(kQueueFull, static_cast<uint8_t>(senderSlot), key, nullptr);
}

void QueueConnectBroadcastForSlot(int slot) {
    N::Session* s = Running(N::Role::Host);
    if (!s || slot < 1 || slot >= static_cast<int>(N::kMaxPeers) || ReplayOff()) return;
    ReplayCtx ctx{s, slot};
    W::ForEachChipper(&EachForReplay, &ctx);
}

void NoteDestroyed(void* actor) {
    if (g_tracked.empty()) return;
    for (size_t i = 0; i < g_tracked.size(); ++i) {
        if (g_tracked[i].actor.Raw() != actor) continue;
        g_tracked.erase(g_tracked.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

DrillView ReadForDrill() { return g_view; }

void OnDisconnect() {
    uint64_t refused = 0;
    for (const uint64_t n : g_refused) refused += n;
    if (g_view.pours || g_view.presses || g_view.statesSent || g_view.applied || refused)
        UE_LOGI("woodchipper: session end -- host pours=%llu presses=%llu clamped=%llu answers=%llu denied=%llu "
                "states sent=%llu replayed=%llu; client applied=%llu held=%llu", g_view.pours, g_view.presses,
                g_view.clamped, g_view.answers, static_cast<unsigned long long>(refused), g_view.statesSent,
                g_view.replayed, g_view.applied, g_view.held);
    g_view = DrillView{};
    for (uint64_t& n : g_refused) n = 0;
    for (Said& sd : g_said) sd = Said{};
    g_tracked.clear();
    g_held.clear();
    g_heldVersion = 0;
    g_session.store(nullptr);
}

}  // namespace coop::woodchipper_sync
