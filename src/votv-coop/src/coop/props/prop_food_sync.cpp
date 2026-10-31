// coop/props/prop_food_sync.cpp -- see coop/props/prop_food_sync.h.

#include "coop/props/prop_food_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
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
constexpr int kTag = 0x464F4F44;  // 'FOOD'

constexpr auto   kPoll = std::chrono::milliseconds(500);
constexpr auto   kBiteGap = std::chrono::milliseconds(200);
constexpr float  kTemperatureStep = 1.f;   // the tick's target is re-rolled every tick, so a smaller step is noise
constexpr float  kRipenessStep = 5.f;     // a fresh food starts near 3600 and falls about half a point a second
constexpr size_t kSendsPerPoll = 64;
constexpr size_t kMaxPending = 512;
constexpr auto   kHandRepeat = std::chrono::seconds(5);  // a mirror rebuilt on a slot switch, a joiner's first copy
constexpr float  kMaxTemperature = 2000.f;
constexpr int32_t kMaxUses = 1000;

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_nextPoll{};
bool g_watched = false;

std::vector<void*> g_classes;
uint64_t g_classVersion = ~uint64_t{0};

struct Sent {
    F::Condition condition;
};
std::unordered_map<std::wstring, Sent> g_sent;  // HOST: by Key
size_t g_cursor = 0;                            // HOST: where the next poll starts, so a full budget starves no food

// CLIENT: a parked copy, and whether its tick ran before: a food's table row can leave it off, and the session's end
// must not start one the game never ran.
struct Parked {
    ue_wrap::CachedObjRef ref;
    bool wasTicking = false;
};
std::unordered_map<void*, Parked> g_parked;
std::unordered_map<std::wstring, coop::net::FoodStatePayload> g_pending;  // CLIENT: a food not born here yet

std::unordered_map<int, Clock::time_point> g_lastBite;  // HOST: by slot

// The bite in flight.
ue_wrap::CachedObjRef g_biteFood;
int32_t g_biteUses = 0;

// The hand here, each tick: what it holds, and the last thing the player dropped.
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

std::unordered_map<std::wstring, std::pair<int, coop::net::FoodStatePayload>> g_releases;  // HOST: a copy not born yet

uint32_t g_published = 0, g_applied = 0, g_bitesSent = 0, g_bitesTaken = 0, g_bitesRefused = 0;

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
           std::fabs(a.ripeness - b.ripeness) >= kRipenessStep;
}

std::wstring KeyOf(void* actor) {
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    return key == L"None" ? std::wstring{} : key;
}

void FillKey(const std::wstring& key, coop::net::WireKey& out) {
    for (size_t i = 0; i < key.size() && i < sizeof(out.data); ++i)
        out.data[out.len++] = static_cast<char>(key[i]);  // save keys are ASCII
}

bool Publish(coop::net::Session& s, const std::wstring& key, const F::Condition& now) {
    coop::net::FoodStatePayload p{};
    FillKey(key, p.key);
    p.temperature = now.temperature;
    p.ripeness = now.ripeness;
    p.uses = now.uses;
    if (!s.SendReliable(coop::net::ReliableKind::FoodState, &p, sizeof(p))) return false;
    g_sent[key] = {now};
    ++g_published;
    return true;
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
        if (coop::hand_item::IsHandAxisActor(actor)) continue;  // the mirror is parked below, the hand is its holder's
        F::Condition now;
        if (!F::Read(actor, now)) continue;
        const std::wstring key = KeyOf(actor);
        if (key.empty()) continue;
        const auto it = g_sent.find(key);
        if (it != g_sent.end() && !Moved(it->second.condition, now)) continue;
        if (Publish(s, key, now)) ++sends;
    }
}

// Not once per food: the food's Init turns its own tick back on from its table row, so a load or a birth undoes a park.
void Park(void* actor) {
    const bool ticking = ue_wrap::engine::IsActorTickEnabled(actor);
    auto it = g_parked.find(actor);
    if (it == g_parked.end() || !it->second.ref.Is(actor)) {
        Parked& row = g_parked[actor];
        row.ref.Set(actor);
        row.wasTicking = ticking;
    } else if (ticking) {
        it->second.wasTicking = true;
    }
    if (ticking) ue_wrap::engine::SetActorTickEnabled(actor, false);
}

void Unpark(void* actor) {
    auto it = g_parked.find(actor);
    if (it == g_parked.end()) return;
    if (it->second.ref.Is(actor) && it->second.wasTicking) ue_wrap::engine::SetActorTickEnabled(actor, true);
    g_parked.erase(it);
}

bool Apply(const std::wstring& key, const coop::net::FoodStatePayload& p) {
    void* const actor = coop::prop_key_index::Find(key);
    if (!actor || !F::IsFood(actor)) return false;
    if (coop::hand_item::IsHandAxisActor(actor)) return true;  // the hand owns it; its record carries it onward
    Park(actor);
    F::Write(actor, {p.temperature, p.ripeness, p.uses});
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
        Park(mirror);
        const MirrorState& state = g_mirrors[slot];
        if (!state.have) continue;
        F::Write(mirror, {state.condition.temperature, state.condition.ripeness, state.condition.uses});
        F::PaintLooks(mirror);
    }
}

// Host: a release that came ahead of its copy's birth.
bool ApplyRelease(coop::net::Session& s, int slot, const std::wstring& key, const coop::net::FoodStatePayload& p) {
    void* const actor = coop::prop_key_index::Find(key);
    if (!actor) return false;
    if (!F::IsFood(actor) || coop::hand_item::IsHandAxisActor(actor) ||
        !coop::hand_item::LastHeldClassIs(static_cast<uint8_t>(slot), R::ClassNameOf(actor).c_str())) {
        UE_LOGI("prop_food_sync: slot %d's release of '%ls' refused", slot, key.c_str());
        return true;
    }
    const F::Condition condition{p.temperature, p.ripeness, p.uses};
    F::Write(actor, condition);
    Publish(s, key, condition);
    UE_LOGI("prop_food_sync: slot %d let go of '%ls' -- condition taken", slot, key.c_str());
    return true;
}

void PollClient() {
    std::vector<void*> foods;
    Collect(foods);
    for (void* actor : foods) {
        if (coop::hand_item::IsLocalHandViewActor(actor)) Unpark(actor);
        else if (!coop::hand_item::IsHandAxisActor(actor)) Park(actor);
    }
    for (auto it = g_pending.begin(); it != g_pending.end();)
        it = Apply(it->first, it->second) ? g_pending.erase(it) : std::next(it);
    if (g_parked.size() > foods.size() * 2 + 64)
        for (auto it = g_parked.begin(); it != g_parked.end();)
            it = it->second.ref.Get() ? std::next(it) : g_parked.erase(it);
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
    s.SendReliableToSlot(0, coop::net::ReliableKind::FoodRelease, &p, sizeof(p));
    UE_LOGI("prop_food_sync: let go of '%ls' here, uses=%d -- FoodRelease sent", key.c_str(), now.uses);
}

// Every tick, since a thrown food's condition is the hand's only until the host's first value lands on it.
void TickHand(coop::net::Session& s) {
    void* const local = coop::players::Registry::Get().Local();
    if (!local) return;
    void* const hand = coop::hand_item::LocalHandActor();
    void* const food = F::IsFood(hand) ? hand : nullptr;
    void* const wasHeld = g_held.Get();
    if (s.role() == coop::net::Role::Client) {
        // A throw spawns a new actor and keeps it as droppedItem; a place releases the hand's own.
        void* const dropped = ue_wrap::engine::ReadMainPlayerDroppedItem(local);
        if (dropped && !g_lastDropped.Is(dropped)) {
            g_lastDropped.Set(dropped);
            if (F::IsFood(dropped)) SendRelease(s, dropped);
        }
        if (wasHeld && wasHeld != food) SendRelease(s, wasHeld);
    }
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
        for (auto it = g_releases.begin(); it != g_releases.end();)
            it = ApplyRelease(*s, it->second.first, it->first, it->second.second) ? g_releases.erase(it) : std::next(it);
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
    if (g_pending.size() < kMaxPending || g_pending.count(key)) g_pending[key] = p;
}

void OnBite(int senderSlot, const coop::net::FoodBitePayload& p) {
    const std::wstring key = coop::remote_prop::KeyToWString(p.key);
    const auto now = Clock::now();
    auto& last = g_lastBite[senderSlot];
    void* const actor = coop::prop_key_index::Find(key);
    F::Condition condition;
    if (now - last < kBiteGap || !actor || coop::hand_item::IsHandAxisActor(actor) || !F::Read(actor, condition)) {
        ++g_bitesRefused;
        UE_LOGI("prop_food_sync: slot %d's bite of '%ls' refused", senderSlot, key.c_str());
        return;
    }
    last = now;
    ++g_bitesTaken;
    condition.uses = condition.uses > 0 ? condition.uses - 1 : 0;
    UE_LOGI("prop_food_sync: slot %d bit '%ls', uses now %d (#%u)", senderSlot, key.c_str(), condition.uses,
            g_bitesTaken);
    if (condition.uses == 0) {
        ue_wrap::engine::DestroyActor(actor);
        return;
    }
    F::Write(actor, condition);
    auto* s = g_session.load(std::memory_order_acquire);
    if (s) Publish(*s, key, condition);
}

void OnRelease(int senderSlot, const coop::net::FoodStatePayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || p.temperature < 0.f || p.temperature > kMaxTemperature || p.uses < 0 || p.uses > kMaxUses) return;
    const std::wstring key = coop::remote_prop::KeyToWString(p.key);
    if (ApplyRelease(*s, senderSlot, key, p)) return;
    if (g_releases.size() < kMaxPending) g_releases[key] = {senderSlot, p};
}

void OnHand(int senderSlot, const coop::net::FoodHandPayload& p) {
    g_mirrors[senderSlot].condition = p;
    g_mirrors[senderSlot].have = true;
}

void QueueConnectBroadcastForSlot(int /*slot*/) { g_sent.clear(); }

void OnDisconnect() {
    if (g_published || g_applied || g_bitesSent || g_bitesTaken || g_bitesRefused)
        UE_LOGI("prop_food_sync: session end -- published=%u applied=%u bites sent=%u taken=%u refused=%u", g_published,
                g_applied, g_bitesSent, g_bitesTaken, g_bitesRefused);
    for (auto& [actor, row] : g_parked)
        if (row.ref.Is(actor) && row.wasTicking) ue_wrap::engine::SetActorTickEnabled(actor, true);
    g_parked.clear();
    g_pending.clear();
    g_sent.clear();
    g_lastBite.clear();
    g_biteFood.Reset();
    g_held.Reset();
    g_lastDropped.Reset();
    g_handSentAny = false;
    g_mirrors = {};
    g_releases.clear();
    g_cursor = 0;
    g_nextPoll = {};
    g_published = g_applied = g_bitesSent = g_bitesTaken = g_bitesRefused = 0;
}

}  // namespace coop::props::prop_food_sync
