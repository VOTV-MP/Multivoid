// coop/props/prop_food_sync.cpp -- see coop/props/prop_food_sync.h.

#include "coop/props/prop_food_sync.h"

#include "coop/net/protocol.h"
#include "coop/element/intent_authority.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"  // KeyToWString

#include "ue_wrap/actors/food.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::props::prop_food_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace F  = ue_wrap::food;
using Clock  = std::chrono::steady_clock;

// A press on a world food: the eat is its action branch. A bite from the hand is another function, on a food this lane
// leaves alone.
UE_FACT_FN(kRowAction, "prop_food_sync", L"prop_food_C", L"actionOptionIndex", Declared);
// The two bodies that write the player's droppedItem: simulateDrop (a throw, a place, the inventory's drop) and
// forceDrop (a faint, a creature's grab). Each spawns the dropped food anew from the hand's data.
UE_FACT_FN(kRowSimulateDrop, "prop_food_sync", L"mainPlayer_C", L"simulateDrop", Declared);
UE_FACT_FN(kRowForceDrop, "prop_food_sync", L"mainPlayer_C", L"forceDrop", Declared);
constexpr int kTag = 0x464F4F44;  // 'FOOD'

constexpr auto   kPoll = std::chrono::milliseconds(500);
// The gap between two bites, or two releases, from one slot, counted from the last RECEIVED, accepted or not, so a
// flood is bounded before any other work. It starves only the sender's own slot. An honest press cannot repeat
// inside it (the eat interaction's own length, to be read on the rig).
constexpr auto   kBiteGap = std::chrono::milliseconds(200);
constexpr float  kCookStep = 0.05f;
constexpr float  kTemperatureStep = 1.f;   // the tick's target is re-rolled every tick, so a smaller step is noise
constexpr float  kRipenessStep = 5.f;     // a food starts near 7200 (a class's own row: 3600 to 1e9) and falls about
                                           // half a point a second
constexpr size_t kSendsPerPoll = 64;
constexpr size_t kMaxPending = 512;
constexpr size_t kMaxReleasesPerSlot = 64;
// A copy that has not appeared in this long will not: a release waiting for its copy is dropped after it. A host value
// waiting on a client is not timed: the host sends a food that stopped moving once, so it waits for the copy (kMaxPending).
constexpr auto   kReleaseWaitMs = std::chrono::milliseconds(30000);
constexpr auto   kSayGap = std::chrono::seconds(1);  // one refusal line, and one corrective FoodState, per slot per this
constexpr auto   kHandRepeat = std::chrono::seconds(5);  // a mirror rebuilt on a slot switch, a joiner's first copy
constexpr float  kMaxTemperature = 2000.f;
// A garbage guard, not a game rule: -1 for the tick guard's dip below 0, 1e9 the largest class default (the soap's).
constexpr float  kMinRipeness = -1.f;
constexpr float  kMaxRipeness = 1e9f;
// Twice the player's armLength 200: the door's and the coffee's reach (door_verb_intent.cpp kDoorReachUU,
// coffee_intent.cpp kReachUU). Authorize adds the food's bounds and the pose slack.
constexpr float  kReachUU = 400.f;

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_nextPoll{};
bool g_watched = false;
bool g_simulateDropWatched = false;
bool g_forceDropWatched = false;
bool g_dropLogged = false;

std::vector<void*> g_classes;
uint64_t g_classVersion = ~uint64_t{0};

struct Sent {
    F::Condition condition;
};
std::unordered_map<std::wstring, Sent> g_sent;  // HOST: by Key
size_t g_cursor = 0;                            // HOST: where the next poll starts, so a full budget starves no food

struct Pending {
    coop::net::FoodStatePayload payload;
    Clock::time_point           at;
};
std::unordered_map<std::wstring, Pending> g_pending;  // CLIENT: a food not born here yet
std::array<std::vector<std::wstring>, coop::net::kMaxPeers> g_owedKeys;  // HOST: by slot, the foods a joiner is owed
std::array<size_t, coop::net::kMaxPeers> g_owedAt{};                    // HOST: by slot, how far DrainOwed has got
uint32_t g_pendingDropped = 0;                       // CLIENT: values dropped at the cap, this session

std::array<Clock::time_point, coop::net::kMaxPeers> g_lastBite{};     // HOST: by slot, the last bite received
std::array<Clock::time_point, coop::net::kMaxPeers> g_lastRelease{};  // HOST: by slot, the last release received

// HOST: what a slot's refusals have said and owe. A refused act ran on the sender's copy, so the host answers it with the
// food's own FoodState; both the line and the answer are held to one per kSayGap, the answer to the latest refused food.
struct Latch {
    Clock::time_point nextSay{};
    Clock::time_point nextAnswer{};
    uint32_t          since = 0;
    std::wstring      answerKey;
};
std::array<Latch, coop::net::kMaxPeers> g_latch{};

// The bite in flight.
ue_wrap::CachedObjRef g_biteFood;
int32_t g_biteUses = 0;

// The hand here, each tick: what it holds. And the last thing the player dropped, read when a drop body returns.
ue_wrap::CachedObjRef g_held;
ue_wrap::CachedObjRef g_lastDropped;
F::Condition g_handSent;
Clock::time_point g_handSentAt{};
bool g_handSentAny = false;

// What each other slot's hand last said, for the mirror there.
struct MirrorState {
    coop::net::FoodHandPayload condition{};
    bool have = false;
};
std::array<MirrorState, coop::net::kMaxPeers> g_mirrors{};

// The mirror actor and the numbers it was last written and painted with, by slot: a mirror whose actor and numbers have
// not changed is not written or painted again.
struct Painted {
    ue_wrap::CachedObjRef      actor;
    coop::net::FoodHandPayload condition{};
};
std::array<Painted, coop::net::kMaxPeers> g_painted{};

// HOST: a release whose copy is not born yet, by (slot, key). It keeps where its sender stood when it arrived, since the
// copy is judged against that place when it appears.
struct Release {
    coop::net::FoodStatePayload payload;
    Clock::time_point           at;
    ue_wrap::FVector            from;
};
std::map<std::pair<int, std::wstring>, Release> g_releases;

uint32_t g_published = 0, g_applied = 0, g_bitesSent = 0, g_bitesTaken = 0, g_bitesRefused = 0, g_releasesRefused = 0;

void RefreshClasses() {
    const uint64_t version = ue_wrap::object_index::ClassSetVersion();
    if (version == g_classVersion) return;
    g_classVersion = version;
    g_classes.clear();
    ue_wrap::object_index::ForEachClass(
        [](void*, void* cls, void*) {
            if (F::IsFoodClass(cls)) g_classes.push_back(cls);
        },
        nullptr);
}

void Collect(std::vector<void*>& out) {
    struct Walk {
        void* world;
        std::vector<void*>* out;
    } walk{ue_wrap::world_identity::CurrentWorld(), &out};
    if (!walk.world) return;
    RefreshClasses();
    for (void* cls : g_classes)
        ue_wrap::object_index::ForEachInstance(
            cls,
            [](void* ctx, void* obj, int32_t) {
                auto* w = static_cast<Walk*>(ctx);
                if (ue_wrap::world_identity::WorldOf(obj) == w->world) w->out->push_back(obj);
            },
            &walk);
}

bool Moved(const F::Condition& a, const F::Condition& b) {
    return a.uses != b.uses || std::fabs(a.temperature - b.temperature) >= kTemperatureStep ||
           std::fabs(a.ripeness - b.ripeness) >= kRipenessStep || std::fabs(a.cookProgress - b.cookProgress) >= kCookStep;
}

std::wstring KeyOf(void* actor) {
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    return key == L"None" ? std::wstring{} : key;
}

void FillKey(const std::wstring& key, coop::net::WireKey& out) {
    for (size_t i = 0; i < key.size() && i < sizeof(out.data); ++i)
        out.data[out.len++] = static_cast<char>(key[i]);  // save keys are ASCII
}

coop::net::FoodStatePayload StatePayload(const std::wstring& key, const F::Condition& now) {
    coop::net::FoodStatePayload p{};
    FillKey(key, p.key);
    p.temperature = now.temperature;
    p.ripeness = now.ripeness;
    p.uses = now.uses;
    p.cookProgress = now.cookProgress;
    return p;
}

bool Publish(coop::net::Session& s, const std::wstring& key, const F::Condition& now) {
    const coop::net::FoodStatePayload p = StatePayload(key, now);
    if (!s.SendReliable(coop::net::ReliableKind::FoodState, &p, sizeof(p))) return false;
    g_sent[key] = {now};
    ++g_published;
    return true;
}

// One slot's copy put back to the host's value. Not a publish: g_sent stays as it is, since every other peer already
// holds what it records.
bool PublishToSlot(coop::net::Session& s, int slot, const std::wstring& key, const F::Condition& now) {
    const coop::net::FoodStatePayload p = StatePayload(key, now);
    return s.SendReliableToSlot(slot, coop::net::ReliableKind::FoodState, &p, sizeof(p));
}

// HOST: the foods a joiner is owed, sent to its slot alone, up to kSendsPerPoll a poll. A food gone since the keys were
// taken has nothing to send; a refused send waits for the next poll where it stopped.
void DrainOwed(coop::net::Session& s) {
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        std::vector<std::wstring>& keys = g_owedKeys[slot];
        size_t& at = g_owedAt[slot];
        size_t sends = 0;
        while (at < keys.size() && sends < kSendsPerPoll) {
            void* const actor = coop::prop_key_index::Find(keys[at]);
            F::Condition now;
            if (actor && F::IsFood(actor) && !coop::hand_item::IsHandAxisActor(actor) && F::Read(actor, now)) {
                if (!PublishToSlot(s, slot, keys[at], now)) break;
                ++sends;
            }
            ++at;
        }
        if (at >= keys.size()) {
            std::vector<std::wstring>().swap(keys);
            at = 0;
        }
    }
}

void PollHost(coop::net::Session& s) {
    std::vector<void*> foods;
    Collect(foods);
    if (foods.empty()) return;
    size_t sends = 0;
    const size_t start = g_cursor % foods.size();
    for (size_t i = 0; i < foods.size() && sends < kSendsPerPoll; ++i) {
        const size_t at = (start + i) % foods.size();
        void* const actor = foods[at];
        g_cursor = at;
        if (coop::hand_item::IsHandAxisActor(actor)) continue;  // the mirror is written by PollMirrors, the hand is its holder's
        F::Condition now;
        if (!F::Read(actor, now)) continue;
        const std::wstring key = KeyOf(actor);
        if (key.empty()) continue;
        const auto it = g_sent.find(key);
        if (it != g_sent.end() && !Moved(it->second.condition, now)) continue;
        if (Publish(s, key, now)) ++sends;
    }
}

bool Apply(const std::wstring& key, const coop::net::FoodStatePayload& p) {
    void* const actor = coop::prop_key_index::Find(key);
    if (!actor || !F::IsFood(actor)) return false;
    if (coop::hand_item::IsHandAxisActor(actor)) return true;  // the hand owns it; its record carries it onward
    F::Write(actor, {p.temperature, p.ripeness, p.uses, p.cookProgress});
    F::PaintLooks(actor);
    ++g_applied;
    return true;
}

void PollMirrors() {
    const int local = coop::players::Registry::Get().LocalPeerId();
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        if (slot == local) continue;
        void* const mirror = coop::hand_item::MirrorActorForSlot(static_cast<uint8_t>(slot));
        if (!mirror || !F::IsFood(mirror)) continue;
        const MirrorState& state = g_mirrors[slot];
        if (!state.have) continue;
        Painted& painted = g_painted[slot];
        const coop::net::FoodHandPayload& c = state.condition;
        if (painted.actor.Is(mirror) && painted.condition.temperature == c.temperature &&
            painted.condition.ripeness == c.ripeness && painted.condition.uses == c.uses)
            continue;
        F::Write(mirror, {c.temperature, c.ripeness, c.uses});
        F::PaintLooks(mirror);
        painted.actor.Set(mirror);
        painted.condition = c;
    }
}

// One refusal line per slot per kSayGap, carrying the count since the last (coffee_state.cpp's shape), whatever the
// refusal.
void SayLatched(int slot, const char* verb, const std::wstring& key, const char* why) {
    Latch& l = g_latch[slot];
    const auto now = Clock::now();
    if (now < l.nextSay) {
        ++l.since;
        return;
    }
    UE_LOGI("prop_food_sync: slot %d's %s of '%ls' refused: %s (%u more since the last line)", slot, verb,
            key.c_str(), why, l.since);
    l.nextSay = now + kSayGap;
    l.since = 0;
}

// The sender's copy ran the act, so a refused act is answered with the food's own FoodState to that slot alone: once per
// kSayGap, for the latest food refused. A food that is gone here or in a hand has nothing to send.
void Answer(coop::net::Session& s, int slot) {
    Latch& l = g_latch[slot];
    if (l.answerKey.empty()) return;
    const auto now = Clock::now();
    if (now < l.nextAnswer) return;
    void* const actor = coop::prop_key_index::Find(l.answerKey);
    F::Condition condition;
    if (actor && F::IsFood(actor) && !coop::hand_item::IsHandAxisActor(actor) && F::Read(actor, condition) &&
        PublishToSlot(s, slot, l.answerKey, condition))
        l.nextAnswer = now + kSayGap;
    l.answerKey.clear();
}

// A bite or a release of `key` from `slot` changed nothing here.
void Refuse(int slot, const char* verb, const std::wstring& key, const char* why, uint32_t& counter) {
    ++counter;
    SayLatched(slot, verb, key, why);
    g_latch[slot].answerKey = key;
    if (auto* s = g_session.load(std::memory_order_acquire)) Answer(*s, slot);
}

// The slot's body as the host last placed it, or false when it has none that took a pose (Authorize's NoBody).
bool PlaceOf(int slot, ue_wrap::FVector& out) {
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* const puppet = (rp && rp->valid() && rp->HasPose()) ? rp->GetActor() : nullptr;
    return puppet && ue_wrap::engine::TryGetActorLocation(puppet, out);
}

// Which of a release's numbers is outside what any food holds. A garbage guard, not a game rule.
const char* ValueFault(const coop::net::FoodStatePayload& p) {
    if (p.temperature < 0.f || p.temperature > kMaxTemperature) return "temperature out of bound";
    if (p.ripeness < kMinRipeness || p.ripeness > kMaxRipeness) return "ripeness out of bound";
    return nullptr;
}

// Whether this host copy is one `slot` could have let go of, and whether the release refills it: uses never rise.
const char* CopyFault(int slot, void* actor, const coop::net::FoodStatePayload& p) {
    F::Condition current;
    if (!F::IsFood(actor) || !F::Read(actor, current)) return "not a food";
    if (coop::hand_item::IsHandAxisActor(actor)) return "in a hand";
    if (!coop::hand_item::LastHeldClassIs(static_cast<uint8_t>(slot), R::ClassNameOf(actor).c_str()))
        return "not the class that hand held";
    if (p.uses < 0 || p.uses > current.uses) return "uses out of bound";
    return nullptr;
}

void TakeRelease(coop::net::Session& s, int slot, const std::wstring& key, void* actor,
                 const coop::net::FoodStatePayload& p) {
    const F::Condition condition{p.temperature, p.ripeness, p.uses, p.cookProgress};
    F::Write(actor, condition);
    Publish(s, key, condition);
    UE_LOGI("prop_food_sync: slot %d let go of '%ls' -- condition taken", slot, key.c_str());
}

// A release that came ahead of its copy's birth, judged when the copy appears: against where its sender stood when it
// arrived, with the three terms Authorize sums (intent_authority.cpp:55): the lane's reach, the copy's bounds radius and
// the pose slack.
void DrainReleases(coop::net::Session& s) {
    const auto now = Clock::now();
    for (auto it = g_releases.begin(); it != g_releases.end();) {
        const int slot = it->first.first;
        const std::wstring& key = it->first.second;
        const Release& r = it->second;
        if (now - r.at > kReleaseWaitMs) {
            SayLatched(slot, "release", key, "its copy did not appear in time");
            it = g_releases.erase(it);
            continue;
        }
        void* const actor = coop::prop_key_index::Find(key);
        if (!actor) {
            ++it;
            continue;
        }
        ue_wrap::FVector centre{};
        float radius = 0.f;
        const char* why = CopyFault(slot, actor, r.payload);
        if (!why) {
            if (!ue_wrap::engine::ActorReachSphere(actor, centre, radius)) why = "no place to measure to";
            else {
                const float dx = centre.X - r.from.X, dy = centre.Y - r.from.Y, dz = centre.Z - r.from.Z;
                if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= kReachUU + radius + coop::element::kPoseStalenessUU))
                    why = "out of reach of where its sender stood";
            }
        }
        if (why) Refuse(slot, "release", key, why, g_releasesRefused);
        else TakeRelease(s, slot, key, actor, r.payload);
        it = g_releases.erase(it);
    }
}

// The client's poll: the host's values that waited for their food. The food's own tick is refused at the gate
// (coop/props/food_tick_gate), so nothing here holds it off.
void PollClient() {
    for (auto it = g_pending.begin(); it != g_pending.end();)
        it = Apply(it->first, it->second.payload) ? g_pending.erase(it) : std::next(it);
}

sg::Verdict OnActionPre(const sg::Call& c) {
    g_biteFood.Reset();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (coop::hand_item::IsHandAxisActor(c.object)) return sg::Verdict::Run;
    F::Condition now;
    if (!F::Read(c.object, now)) return sg::Verdict::Run;
    g_biteFood.Set(c.object);
    g_biteUses = now.uses;
    return sg::Verdict::Run;
}

void OnActionPost(const sg::Call& c) {
    void* const actor = g_biteFood.Get();
    g_biteFood.Reset();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!actor || actor != c.object || !s || !s->connected()) return;  // the last bite: the destroy crosses
    F::Condition now;
    if (!F::Read(actor, now) || now.uses >= g_biteUses) return;
    const std::wstring key = KeyOf(actor);
    if (key.empty()) return;
    coop::net::FoodBitePayload p{};
    FillKey(key, p.key);
    if (s->SendReliableToSlot(0, coop::net::ReliableKind::FoodBite, &p, sizeof(p))) ++g_bitesSent;
    UE_LOGI("prop_food_sync: bit '%ls' here, uses %d->%d -- FoodBite sent (#%u)", key.c_str(), g_biteUses, now.uses,
            g_bitesSent);
}

void SendRelease(coop::net::Session& s, void* actor) {
    F::Condition now;
    const std::wstring key = KeyOf(actor);
    if (key.empty() || !F::Read(actor, now)) return;
    coop::net::FoodStatePayload p{};
    FillKey(key, p.key);
    p.temperature = now.temperature;
    p.ripeness = now.ripeness;
    p.uses = now.uses;
    p.cookProgress = now.cookProgress;
    s.SendReliableToSlot(0, coop::net::ReliableKind::FoodRelease, &p, sizeof(p));
    UE_LOGI("prop_food_sync: let go of '%ls' here, uses=%d -- FoodRelease sent", key.c_str(), now.uses);
}

// A drop body has returned on the local player: the food it spawned is the player's droppedItem, and its condition is
// the hand's only until the host's first value lands on it. The two bodies are Blueprint functions, so the script
// gate sees them on every route; the field itself signals nothing.
void OnDropPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    void* const local = coop::players::Registry::Get().Local();
    if (!local || c.object != local) return;
    void* const dropped = ue_wrap::engine::ReadMainPlayerDroppedItem(local);
    if (!dropped || g_lastDropped.Is(dropped)) return;  // a drop that spawned nothing leaves the older one named
    g_lastDropped.Set(dropped);
    if (F::IsFood(dropped)) SendRelease(*s, dropped);
}

// Every tick: the hand's food is read for FoodHand, which nothing signals (holding_actor is written by updateHold with
// no seam of its own), and a hand that emptied with its food still alive is sent as a release.
void TickHand(coop::net::Session& s) {
    void* const local = coop::players::Registry::Get().Local();
    if (!local) return;
    void* const hand = coop::hand_item::LocalHandActor();
    void* const food = F::IsFood(hand) ? hand : nullptr;
    void* const wasHeld = g_held.Get();
    if (s.role() == coop::net::Role::Client && wasHeld && wasHeld != food) SendRelease(s, wasHeld);
    if (wasHeld != food) {
        g_held.Set(food);
        g_handSentAny = false;
    }
    if (!food) return;
    F::Condition now;
    if (!F::Read(food, now)) return;
    const auto at = Clock::now();
    if (g_handSentAny && !Moved(g_handSent, now) && at - g_handSentAt < kHandRepeat) return;
    coop::net::FoodHandPayload p{now.temperature, now.ripeness, now.uses};
    const bool sent = s.role() == coop::net::Role::Host
                          ? s.SendReliable(coop::net::ReliableKind::FoodHand, &p, sizeof(p))
                          : s.SendReliableToSlot(0, coop::net::ReliableKind::FoodHand, &p, sizeof(p));
    if (!sent) return;
    g_handSent = now;
    g_handSentAt = at;
    g_handSentAny = true;
}

void WatchWhenLoaded() {
    // A refusal is logged once by the gate itself, and retried here; each watch has its own latch, so one that
    // registered is not registered again for the other's sake.
    if (!g_simulateDropWatched) g_simulateDropWatched = sg::WatchName(kRowSimulateDrop, kTag, nullptr, &OnDropPost);
    if (!g_forceDropWatched) g_forceDropWatched = sg::WatchName(kRowForceDrop, kTag, nullptr, &OnDropPost);
    if (g_simulateDropWatched && g_forceDropWatched && !g_dropLogged) {
        g_dropLogged = true;
        UE_LOGI("prop_food_sync: the watch on the player's drop is registered");
    }
    if (g_watched) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowAction);
    if (!fn) return;  // the class loads with the world
    g_watched = sg::Watch(fn, kTag, &OnActionPre, &OnActionPost);
    if (g_watched) UE_LOGI("prop_food_sync: the watch on a food's press is live");
    else UE_LOGE("prop_food_sync: the watch on a food's press is dead -- a client's bite stays on its copy");
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    TickHand(*s);
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPoll;
    WatchWhenLoaded();
    PollMirrors();
    if (s->role() == coop::net::Role::Host) {
        DrainReleases(*s);
        for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) Answer(*s, slot);
        DrainOwed(*s);
        PollHost(*s);
    } else {
        PollClient();
    }
}

void OnState(const coop::net::FoodStatePayload& p) {
    const std::wstring key = coop::remote_prop::KeyToWString(p.key);
    if (Apply(key, p)) {
        g_pending.erase(key);
        return;
    }
    // A food that stopped moving is never sent again, so its value waits for the copy however long that takes. At the
    // cap the oldest waiting value goes, and is said.
    if (g_pending.size() >= kMaxPending && !g_pending.count(key)) {
        auto oldest = g_pending.begin();
        for (auto it = g_pending.begin(); it != g_pending.end(); ++it)
            if (it->second.at < oldest->second.at) oldest = it;
        if (g_pendingDropped++ % 64 == 0)
            UE_LOGW("prop_food_sync: %zu foods wait for their copy; dropped the oldest, '%ls' (#%u)", g_pending.size(),
                    oldest->first.c_str(), g_pendingDropped);
        g_pending.erase(oldest);
    }
    g_pending[key] = {p, Clock::now()};
}

void OnBite(int senderSlot, const coop::net::FoodBitePayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const std::wstring key = coop::remote_prop::KeyToWString(p.key);
    const auto now = Clock::now();
    const bool tooSoon = now - g_lastBite[senderSlot] < kBiteGap;
    g_lastBite[senderSlot] = now;  // every bite received, accepted or not
    if (tooSoon) {
        Refuse(senderSlot, "bite", key, "inside the gap", g_bitesRefused);
        return;
    }
    void* const actor = coop::prop_key_index::Find(key);
    F::Condition condition;
    if (!actor || !F::IsFood(actor) || coop::hand_item::IsHandAxisActor(actor) || !F::Read(actor, condition)) {
        Refuse(senderSlot, "bite", key, "no such food here, or in a hand", g_bitesRefused);
        return;
    }
    // The bite already ran on the sender's copy: an act, not a press that can wait, so a refusal is final and answered
    // below. A joiner's bite before its body is here is refused the same way. Reach as the door's and the coffee's.
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(*s, static_cast<uint8_t>(senderSlot), kReachUU).Authorize(actor);
    if (!subject) {
        Refuse(senderSlot, "bite", key, coop::element::OutcomeName(subject.outcome), g_bitesRefused);
        return;
    }
    ++g_bitesTaken;
    condition.uses = condition.uses > 0 ? condition.uses - 1 : 0;
    UE_LOGI("prop_food_sync: slot %d bit '%ls', uses now %d (#%u)", senderSlot, key.c_str(), condition.uses,
            g_bitesTaken);
    if (condition.uses == 0) {
        ue_wrap::engine::DestroyActor(actor);
        return;
    }
    F::Write(actor, condition);
    Publish(*s, key, condition);
}

void OnRelease(int senderSlot, const coop::net::FoodStatePayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const std::wstring key = coop::remote_prop::KeyToWString(p.key);
    const auto now = Clock::now();
    const bool tooSoon = now - g_lastRelease[senderSlot] < kBiteGap;
    g_lastRelease[senderSlot] = now;  // every release received, accepted or not
    if (tooSoon) {
        Refuse(senderSlot, "release", key, "inside the gap", g_releasesRefused);
        return;
    }
    if (const char* why = ValueFault(p)) {
        Refuse(senderSlot, "release", key, why, g_releasesRefused);
        return;
    }
    void* const actor = coop::prop_key_index::Find(key);
    if (!actor) {
        // Not born here yet: judged when it is, against where the sender stands now. No body, no release.
        ue_wrap::FVector from{};
        if (!PlaceOf(senderSlot, from)) {
            Refuse(senderSlot, "release", key, "the sender has no body here yet", g_releasesRefused);
            return;
        }
        size_t held = 0;
        auto oldest = g_releases.end();
        for (auto it = g_releases.begin(); it != g_releases.end(); ++it) {
            if (it->first.first != senderSlot || it->first.second == key) continue;
            ++held;
            if (oldest == g_releases.end() || it->second.at < oldest->second.at) oldest = it;
        }
        if (held >= kMaxReleasesPerSlot) {
            SayLatched(senderSlot, "release", oldest->first.second, "dropped for the slot's cap");
            g_releases.erase(oldest);
        }
        g_releases[{senderSlot, key}] = {p, now, from};
        return;
    }
    const char* why = CopyFault(senderSlot, actor, p);
    if (!why) {
        const coop::element::IntentSubject subject =
            coop::element::IntentTarget::ForClientIntent(*s, static_cast<uint8_t>(senderSlot), kReachUU).Authorize(actor);
        if (!subject) why = coop::element::OutcomeName(subject.outcome);
    }
    if (why) Refuse(senderSlot, "release", key, why, g_releasesRefused);
    else TakeRelease(*s, senderSlot, key, actor, p);
}

void OnHand(int senderSlot, const coop::net::FoodHandPayload& p) {
    g_mirrors[senderSlot].condition = p;
    g_mirrors[senderSlot].have = true;
}

void OnBadHand(int senderSlot) {
    if (senderSlot < 0 || senderSlot >= coop::net::kMaxPeers) return;
    SayLatched(senderSlot, "hand", std::wstring{}, "a negative use count");
}

void QueueConnectBroadcastForSlot(int slot) {
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    // A recycled slot starts clean: nothing of the last occupant's gaps, refusals or waiting releases is the joiner's.
    g_lastBite[slot] = g_lastRelease[slot] = {};
    g_latch[slot] = {};
    for (auto it = g_releases.begin(); it != g_releases.end();)
        it = it->first.first == slot ? g_releases.erase(it) : std::next(it);
    // Every food again, to this slot alone: g_sent stays, since every other peer holds what it records. The keys are
    // taken now and sent on the poll's budget by DrainOwed.
    g_owedKeys[slot].clear();
    g_owedAt[slot] = 0;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    std::vector<void*> foods;
    Collect(foods);
    for (void* actor : foods) {
        const std::wstring key = KeyOf(actor);
        if (!key.empty()) g_owedKeys[slot].push_back(key);
    }
}

void OnDisconnect() {
    if (g_published || g_applied || g_bitesSent || g_bitesTaken || g_bitesRefused || g_releasesRefused)
        UE_LOGI("prop_food_sync: session end -- published=%u applied=%u bites sent=%u taken=%u refused=%u releases "
                "refused=%u", g_published, g_applied, g_bitesSent, g_bitesTaken, g_bitesRefused, g_releasesRefused);
    g_pending.clear();
    g_pendingDropped = 0;
    for (auto& keys : g_owedKeys) keys.clear();
    g_owedAt = {};
    g_sent.clear();
    g_lastBite = {};
    g_lastRelease = {};
    g_latch = {};
    g_biteFood.Reset();
    g_held.Reset();
    g_lastDropped.Reset();
    g_handSentAny = false;
    g_mirrors = {};
    for (Painted& p : g_painted) p = Painted{};
    g_releases.clear();
    g_cursor = 0;
    g_nextPoll = {};
    g_published = g_applied = g_bitesSent = g_bitesTaken = g_bitesRefused = g_releasesRefused = 0;
}

}  // namespace coop::props::prop_food_sync
