// coop/interactables/coffee_intent.cpp -- a client's coffee action runs on the host's keyed actor.

#include "coop/interactables/coffee_intent.h"

#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_lifecycle.h"
#include "ue_wrap/core/object_index.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstring>
#include <map>
#include <utility>

namespace coop::coffee_intent {
namespace {
namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace EL = coop::element;

UE_FACT_FN(kAction, "coffee_intent", nullptr, L"actionOptionIndex", AnyClass);
UE_FACT_FN(kCupEaten, "coffee_intent", L"prop_food_coffeeCup_C", L"eaten", Declared);
UE_FACT_FN(kMakerUpdate, "coffee_intent", L"prop_coffeeMaker_C", L"upd", Declared);
UE_FACT_FN(kVendingGraph, "coffee_intent", L"prop_coffeevending_C",
           L"ExecuteUbergraph_prop_coffeevending", Declared);
constexpr int kTag = 0x434F4641; // 'COFA'
constexpr float kReachUU = 600.f;
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_actionRegistered = false;
bool g_cupEatenRegistered = false;
bool g_makerUpdateRegistered = false;
bool g_vendingGraphRegistered = false;
bool g_registrationWarningLogged = false;
uint64_t g_sent = 0, g_run = 0, g_refused = 0;
std::map<uint32_t, coop::net::CoffeeDeviceStatePayload> g_lastSent;
std::map<uint32_t, coop::net::CoffeeDeviceStatePayload> g_pendingApply;

bool IsCoffeeActor(void* actor) {
    if (!actor) return false;
    for (void* cls = R::ClassOf(actor); cls; cls = R::SuperStructOf(cls)) {
        const auto& name = R::NameOf(cls);
        if (R::NameEquals(name, L"prop_coffeevending_C") ||
            R::NameEquals(name, L"prop_coffeeMaker_C")) return true;
    }
    return false;
}

int DeviceKind(void* actor) {
    if (!actor) return -1;
    for (void* cls = R::ClassOf(actor); cls; cls = R::SuperStructOf(cls)) {
        const auto& name = R::NameOf(cls);
        if (R::NameEquals(name, L"prop_coffeevending_C")) return 0;
        if (R::NameEquals(name, L"prop_coffeeMaker_C")) return 1;
    }
    return -1;
}

bool IsCoffeeCup(void* actor) {
    if (!actor) return false;
    for (void* cls = R::ClassOf(actor); cls; cls = R::SuperStructOf(cls))
        if (R::NameEquals(R::NameOf(cls), L"prop_food_coffeeCup_C")) return true;
    return false;
}

bool ReadField(void* actor, const wchar_t* name, void* out, size_t size) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    if (off < 0) return false;
    std::memcpy(out, static_cast<const uint8_t*>(actor) + off, size);
    return true;
}

void SendState(coop::net::Session& session, void* actor, int slot = -1) {
    const int device = DeviceKind(actor);
    const bool coffeeCup = IsCoffeeCup(actor);
    if (device < 0 && !coffeeCup) return;
    coop::net::CoffeeDeviceStatePayload p{};
    p.elementId = static_cast<uint32_t>(EL::Registry::Get().EidForActor(actor));
    p.device = static_cast<uint8_t>(coffeeCup ? 2 : device);
    if (!p.elementId || p.elementId == static_cast<uint32_t>(EL::kInvalidId)) return;
    if (coffeeCup) {
        bool first = false;
        int32_t uses = 0;
        if (!ReadField(actor, L"first", &first, sizeof(first)) ||
            !ReadField(actor, L"uses", &uses, sizeof(uses))) return;
        p.flags = first ? 1u : 0u;
        p.balance = uses;
    } else if (device == 0) {
        int32_t balance = 0;
        bool processing = false;
        if (!ReadField(actor, L"balance", &balance, sizeof(balance)) ||
            !ReadField(actor, L"processing", &processing, sizeof(processing))) return;
        p.balance = balance;
        p.flags = processing ? 1u : 0u;
    } else {
        bool hasCup = false, hasCoffee = false, ready = false, power = false;
        if (!ReadField(actor, L"hasCup", &hasCup, sizeof(hasCup)) ||
            !ReadField(actor, L"hasCoffee", &hasCoffee, sizeof(hasCoffee)) ||
            !ReadField(actor, L"coffeeReady", &ready, sizeof(ready)) ||
            !ReadField(actor, L"hasPower", &power, sizeof(power))) return;
        p.flags = static_cast<uint8_t>((hasCup ? 1u : 0u) | (hasCoffee ? 2u : 0u) |
                                       (ready ? 4u : 0u) | (power ? 8u : 0u));
    }
    if (slot < 0) {
        const auto it = g_lastSent.find(p.elementId);
        if (it != g_lastSent.end() && std::memcmp(&it->second, &p, sizeof(p)) == 0) return;
        if (session.SendReliable(coop::net::ReliableKind::CoffeeDeviceState, &p, sizeof(p)))
            g_lastSent[p.elementId] = p;
    } else {
        session.SendReliableToSlot(slot, coop::net::ReliableKind::CoffeeDeviceState, &p, sizeof(p));
    }
}

void OnActionPost(const SG::Call& call) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->connected() || session->role() != coop::net::Role::Host ||
        !IsCoffeeActor(call.object)) return;
    SendState(*session, call.object);
}

void OnDeviceUpdatePost(const SG::Call& call) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->connected() || session->role() != coop::net::Role::Host) return;
    SendState(*session, call.object);
}

bool ApplyState(const coop::net::CoffeeDeviceStatePayload& p) {
    if (p.device > 2) return true;
    void* actor = EL::LivePropActor(static_cast<EL::ElementId>(p.elementId));
    if (!actor) return false;
    if (p.device == 2) {
        if (!IsCoffeeCup(actor)) return true;
        const bool first = (p.flags & 1u) != 0;
        const int32_t firstOff = R::FindPropertyOffset(R::ClassOf(actor), L"first");
        const int32_t usesOff = R::FindPropertyOffset(R::ClassOf(actor), L"uses");
        if (firstOff >= 0) std::memcpy(static_cast<uint8_t*>(actor) + firstOff, &first, sizeof(first));
        if (usesOff >= 0) std::memcpy(static_cast<uint8_t*>(actor) + usesOff, &p.balance, sizeof(p.balance));
        return true;
    }
    if (DeviceKind(actor) != p.device) return true;
    auto write = [actor](const wchar_t* name, const void* value, size_t size) {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
        if (off >= 0) std::memcpy(static_cast<uint8_t*>(actor) + off, value, size);
    };
    if (p.device == 0) {
        const bool processing = (p.flags & 1u) != 0;
        write(L"balance", &p.balance, sizeof(p.balance));
        write(L"processing", &processing, sizeof(processing));
        return true;
    }
    const bool hasCup = (p.flags & 1u) != 0, hasCoffee = (p.flags & 2u) != 0;
    const bool ready = (p.flags & 4u) != 0, power = (p.flags & 8u) != 0;
    write(L"hasCup", &hasCup, sizeof(hasCup));
    write(L"hasCoffee", &hasCoffee, sizeof(hasCoffee));
    write(L"coffeeReady", &ready, sizeof(ready));
    write(L"hasPower", &power, sizeof(power));
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"upd");
    ue_wrap::ParamFrame frame(fn);
    if (frame.valid()) ue_wrap::Call(actor, frame);
    return true;
}

void SnapshotInstance(void* ctx, void* actor, int32_t) {
    auto* pair = static_cast<std::pair<coop::net::Session*, int>*>(ctx);
    SendState(*pair->first, actor, pair->second);
}

SG::Verdict OnActionPre(const SG::Call& call) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->connected() || session->role() != coop::net::Role::Client ||
        !IsCoffeeActor(call.object)) return SG::Verdict::Run;
    const int32_t actionOffset = R::FindParamOffset(call.function, L"Action");
    if (!call.locals || actionOffset < 0) return SG::Verdict::Cancel;
    coop::net::CoffeeActionIntentPayload payload{};
    payload.elementId = static_cast<uint32_t>(EL::Registry::Get().EidForActor(call.object));
    std::memcpy(&payload.action, call.locals + actionOffset, sizeof(payload.action));
    if (!payload.elementId || !session->SendReliableToSlot(0, coop::net::ReliableKind::CoffeeActionIntent,
                                                            &payload, sizeof(payload))) {
        UE_LOGW("coffee_intent: client coffee action was not sent; local action cancelled");
        return SG::Verdict::Cancel;
    }
    ++g_sent;
    return SG::Verdict::Cancel;
}

SG::Verdict OnCupEatenPre(const SG::Call& call) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->connected() || session->role() != coop::net::Role::Client ||
        !IsCoffeeCup(call.object)) return SG::Verdict::Run;
    coop::net::CoffeeActionIntentPayload payload{};
    payload.elementId = static_cast<uint32_t>(EL::Registry::Get().EidForActor(call.object));
    payload.verb = 1;
    if (!payload.elementId || payload.elementId == static_cast<uint32_t>(EL::kInvalidId) ||
        !session->SendReliableToSlot(0, coop::net::ReliableKind::CoffeeActionIntent,
                                     &payload, sizeof(payload))) {
        UE_LOGW("coffee_intent: client coffee drink was not sent; local eat cancelled");
        return SG::Verdict::Cancel;
    }
    ++g_sent;
    return SG::Verdict::Cancel;
}

void OnCupEatenPost(const SG::Call& call) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->connected() || session->role() != coop::net::Role::Host ||
        !IsCoffeeCup(call.object) || !R::IsLive(call.object)) return;
    SendState(*session, call.object);
}
}

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    if (!g_actionRegistered)
        g_actionRegistered = SG::WatchName(kAction, kTag, &OnActionPre, &OnActionPost);
    if (!g_cupEatenRegistered)
        g_cupEatenRegistered = SG::WatchName(kCupEaten, kTag + 3, &OnCupEatenPre, &OnCupEatenPost);
    if (!g_makerUpdateRegistered)
        g_makerUpdateRegistered = SG::WatchName(kMakerUpdate, kTag + 1, nullptr, &OnDeviceUpdatePost);
    if (!g_vendingGraphRegistered)
        g_vendingGraphRegistered = SG::WatchName(kVendingGraph, kTag + 2, nullptr, &OnDeviceUpdatePost);
    if ((!g_actionRegistered || !g_cupEatenRegistered || !g_makerUpdateRegistered || !g_vendingGraphRegistered) &&
        !g_registrationWarningLogged) {
        UE_LOGE("coffee_intent: action/state watch registration incomplete");
        g_registrationWarningLogged = true;
    }
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->connected()) return;
    if (session->role() != coop::net::Role::Client) return;
    for (auto it = g_pendingApply.begin(); it != g_pendingApply.end();) {
        if (ApplyState(it->second)) it = g_pendingApply.erase(it);
        else ++it;
    }
}

void SnapshotToSlot(int slot) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || session->role() != coop::net::Role::Host || slot < 1 || slot >= coop::net::kMaxPeers) return;
    void* vending = ue_wrap::object_index::ClassByName(L"prop_coffeevending_C");
    void* maker = ue_wrap::object_index::ClassByName(L"prop_coffeeMaker_C");
    void* cup = ue_wrap::object_index::ClassByName(L"prop_food_coffeeCup_C");
    std::pair<coop::net::Session*, int> ctx{session, slot};
    if (vending) ue_wrap::object_index::ForEachInstance(vending, &SnapshotInstance, &ctx);
    if (maker) ue_wrap::object_index::ForEachInstance(maker, &SnapshotInstance, &ctx);
    if (cup) ue_wrap::object_index::ForEachInstance(cup, &SnapshotInstance, &ctx);
}

void OnState(const coop::net::CoffeeDeviceStatePayload& p) {
    if (!ApplyState(p) && g_pendingApply.size() < 64) g_pendingApply[p.elementId] = p;
}

void OnAction(coop::net::Session& session, const coop::net::CoffeeActionIntentPayload& payload,
              uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host || senderSlot == 0 || senderSlot >= coop::net::kMaxPeers) return;
    if (!EL::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        ++g_refused;
        UE_LOGW("coffee_intent: slot %u named non-host element %u", senderSlot, payload.elementId);
        return;
    }
    void* actor = EL::LivePropActor(static_cast<EL::ElementId>(payload.elementId));
    const bool eatCup = payload.verb == 1;
    if (!actor || (eatCup ? !IsCoffeeCup(actor) : (payload.verb != 0 || !IsCoffeeActor(actor)))) {
        ++g_refused;
        return;
    }
    const auto target = EL::IntentTarget::ForClientIntent(session, senderSlot, kReachUU);
    const auto subject = target.Authorize(actor);
    if (!subject) { ++g_refused; UE_LOGI("coffee_intent: slot %u action refused (%s)", senderSlot,
                                          EL::OutcomeName(subject.outcome)); return; }
    auto* puppet = coop::players::Registry::Get().Puppet(senderSlot);
    void* player = puppet && puppet->valid() ? puppet->GetActor() : nullptr;
    if (!player) { ++g_refused; return; }
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), eatCup ? L"eaten" : L"actionOptionIndex");
    ue_wrap::ParamFrame frame(fn);
    if (!frame.valid() || !frame.Set<void*>(L"Player", player) ||
        (!eatCup && !frame.Set<uint8_t>(L"Action", payload.action)) || !ue_wrap::Call(actor, frame)) {
        ++g_refused;
        UE_LOGW("coffee_intent: host could not replay slot %u action %u on element %u",
                senderSlot, payload.action, payload.elementId);
        return;
    }
    if (!eatCup && DeviceKind(actor) == 1) {
        // Some coffee-cup births happen inside the maker's Blueprint graph. Re-run the canonical
        // keyed express on live coffee cups; prop_lifecycle's processed-init latch makes normal
        // Init-observed births a no-op here and covers a birth whose Init body was not dispatched.
        void* cupClass = ue_wrap::object_index::ClassByName(L"prop_food_coffeeCup_C");
        if (cupClass) {
            auto expressCup = [](void*, void* cup, int32_t) {
                coop::prop_lifecycle::ExpressSpawnedProp(cup);
            };
            ue_wrap::object_index::ForEachInstance(cupClass, expressCup, nullptr);
        }
    }
    if (eatCup && R::IsLive(actor)) SendState(session, actor);
    ++g_run;
}

void OnDisconnect() {
    if (g_sent || g_run || g_refused)
        UE_LOGI("coffee_intent: session end -- sent=%llu host-ran=%llu refused=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_run),
                static_cast<unsigned long long>(g_refused));
    g_sent = g_run = g_refused = 0;
    g_lastSent.clear();
    g_pendingApply.clear();
}
}
