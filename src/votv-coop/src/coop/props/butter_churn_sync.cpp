// coop/props/butter_churn_sync.cpp -- see coop/props/butter_churn_sync.h.
//
// 0.9.0n. prop_butterchurn_C::playerUsedOn: with the player holding `milk_n` and `milkPercent` at 0, the held carton is
// destroyed and `milkPercent` set to 100 (above 0, a hint and nothing). actionOptionIndex runs prop_C's (an empty body),
// then action 4 adds 4.25 to `speed`, turns its tick on and starts its `sound`; action 7, with `hasButter`, makes the
// butter (`lib.prop to object` of the row `butter`) at the churn, puts it in the player's inventory
// (putObjectInventory2), and clears `hasButter` and `milkPercent`. Its tick eases `speed_lerp` to `speed`, decays
// `speed`, turns the crank, and with milk in and no butter yet takes `dt * speed_lerp * 0.4` off `milkPercent`, the
// butter made when it runs out; at a `speed` of 0 it turns its tick and sound off.

#include "coop/props/butter_churn_sync.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::butter_churn_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

UE_FACT_FN(kRowAction, "butter_churn_sync", L"prop_butterchurn_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowUsedOn, "butter_churn_sync", L"prop_butterchurn_C", L"playerUsedOn", Declared);
constexpr int kTagAction = 0x42434801, kTagUsedOn = 0x42434802;  // 'BCH'

constexpr const wchar_t* kChurnClass = L"prop_butterchurn_C";
constexpr uint8_t kActChurn = 4, kActTake = 7;
constexpr uint8_t kOpChurn = 1, kOpPour = 2, kOpTake = 3;
constexpr float kReachCm = 600.f;   // the sender's body to the churn
constexpr uint64_t kGapMs = 120;    // a sender's asks: about eight a second, faster than a hand cranks
constexpr float kPourMilk = 100.f;  // the milk a carton pours
constexpr size_t kMaxHeld = 32;
constexpr int kCap = 256;

N::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;
void* g_applying = nullptr;  // CLIENT: the churn whose own action body runs for the host's state or grant
void* g_pouring = nullptr;   // CLIENT: the churn a pour runs on, and its milk before it
float g_milkBefore = 0.f;
uint64_t g_lastAt[N::kMaxPeers] = {};
std::vector<N::ButterChurnStatePayload> g_held;
void* g_churns[kCap];

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

bool IsChurn(void* obj) { return obj && R::IsLive(obj) && R::NameEquals(R::NameOf(R::ClassOf(obj)), kChurnClass); }

float* FloatAt(void* obj, R::InstanceOffset& field) {
    const int32_t off = obj ? field.Of(obj) : -1;
    return off >= 0 ? reinterpret_cast<float*>(static_cast<char*>(obj) + off) : nullptr;
}
R::InstanceOffset s_speed(L"speed"), s_milk(L"milkPercent");

bool ReadButter(void* obj, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), L"hasButter", off, mask) || off < 0) return false;
    out = (*(static_cast<uint8_t*>(obj) + off) & mask) != 0;
    return true;
}
void WriteButter(void* obj, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), L"hasButter", off, mask) || off < 0) return;
    uint8_t& b = *(static_cast<uint8_t*>(obj) + off);
    b = on ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

uint32_t EidOf(void* obj) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(obj);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

// The churn's own action body, `action` pressed by `player` (null for the host's run of a client's churn: the body
// reads no player for it).
bool RunAction(void* churn, uint8_t action, void* player) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(churn), L"actionOptionIndex"));
    if (!f.valid()) return false;
    f.Set<void*>(L"player", player);
    f.Set<uint8_t>(L"action", action);
    return ue_wrap::Call(churn, f);
}

bool SendState(void* churn, int slot, bool grant) {
    if (!g_session || !g_session->connected()) return false;
    N::ButterChurnStatePayload p{};
    p.eid = EidOf(churn);
    const float* const speed = FloatAt(churn, s_speed);
    const float* const milk = FloatAt(churn, s_milk);
    bool butter = false;
    if (!p.eid || !speed || !milk || !ReadButter(churn, butter)) return false;
    p.speed = *speed;
    p.milk = *milk;
    p.hasButter = butter ? 1 : 0;
    p.grant = grant ? 1 : 0;
    const bool ok = slot >= 0 ? g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::ButterChurnState,
                                                              &p, sizeof(p))
                              : g_session->SendReliable(N::ReliableKind::ButterChurnState, &p, sizeof(p));
    UE_LOGI("butter_churn: HOST churn eid %u: speed %.2f milk %.1f butter %u%hs -- sent to %hs (ok=%d)", p.eid, p.speed,
            p.milk, p.hasButter, grant ? ", the take granted" : "", slot >= 0 ? "one slot" : "every client", ok ? 1 : 0);
    return ok;
}

bool SendAsk(void* churn, uint8_t op) {
    N::ButterChurnAskPayload p{};
    p.eid = EidOf(churn);
    p.op = op;
    if (!p.eid) {
        UE_LOGW("butter_churn: CLIENT the churn has no element id -- op %u is not asked", op);
        return false;
    }
    const bool ok = g_session->SendReliable(N::ReliableKind::ButterChurnAsk, &p, sizeof(p));
    UE_LOGI("butter_churn: CLIENT asked the host op %u (1 churn, 2 pour, 3 take) of churn eid %u (ok=%d)", op, p.eid,
            ok ? 1 : 0);
    return ok;
}

// ---- the gate --------------------------------------------------------------------------------------------------------
// CLIENT: a churn or a take is the host's to judge. Any other action, or the body run for the host's state, runs.
sg::Verdict OnActionPre(const sg::Call& c) {
    if (!IsClient() || !c.object || !c.function || !c.locals || c.object == g_applying) return sg::Verdict::Run;
    static R::ParamOffset s_action(L"action");
    const int32_t off = s_action.Of(c.function);
    if (off < 0) return sg::Verdict::Run;
    const uint8_t action = c.locals[off];
    if (action != kActChurn && action != kActTake) return sg::Verdict::Run;
    SendAsk(c.object, action == kActChurn ? kOpChurn : kOpTake);
    return sg::Verdict::Cancel;
}

// HOST: a churn or a take ran on its copy (its player's, or a client's churn it ran): every client hears it.
void OnActionPost(const sg::Call& c) {
    if (IsHost() && c.object) SendState(c.object, -1, false);
}

// The pour runs where the carton is: on the client's copy, then asked. CLIENT: its milk before it.
sg::Verdict OnUsedOnPre(const sg::Call& c) {
    if (IsClient() && c.object) {
        const float* const milk = FloatAt(c.object, s_milk);
        g_pouring = c.object;
        g_milkBefore = milk ? *milk : 0.f;
    }
    return sg::Verdict::Run;
}

void OnUsedOnPost(const sg::Call& c) {
    if (!c.object) return;
    if (IsHost()) {
        SendState(c.object, -1, false);
        return;
    }
    if (!IsClient() || c.object != g_pouring) return;
    g_pouring = nullptr;
    const float* const milk = FloatAt(c.object, s_milk);
    if (milk && g_milkBefore <= 0.f && *milk > 0.f) SendAsk(c.object, kOpPour);  // the carton went in
}

// CLIENT: the host's state on its copy. A churn the host ran replays through the copy's own action 4, so its crank
// turns and its sound plays; a granted take runs the copy's own action 7 into this player's inventory.
bool TakeState(const N::ButterChurnStatePayload& p) {
    void* const churn = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::Prop);
    float* const speed = FloatAt(churn, s_speed);
    float* const milk = FloatAt(churn, s_milk);
    if (!IsChurn(churn) || !speed || !milk) return false;
    g_applying = churn;
    if (p.grant) {
        WriteButter(churn, true);
        const bool ran = RunAction(churn, kActTake, coop::players::Registry::Get().Local());
        UE_LOGI("butter_churn: CLIENT the host granted churn eid %u's butter -- taken (ran=%d)", p.eid, ran ? 1 : 0);
    } else if (p.speed > *speed + 0.5f) {
        RunAction(churn, kActChurn, nullptr);
    }
    g_applying = nullptr;
    *speed = p.speed;
    *milk = p.milk;
    WriteButter(churn, p.hasButter != 0);
    return true;
}

}  // namespace

void Install(N::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    const bool a = sg::WatchName(kRowAction, kTagAction, &OnActionPre, &OnActionPost);
    const bool u = sg::WatchName(kRowUsedOn, kTagUsedOn, &OnUsedOnPre, &OnUsedOnPost);
    if (!a || !u) UE_LOGE("butter_churn: a watch was refused (action=%d usedOn=%d)", a ? 1 : 0, u ? 1 : 0);
}

void Tick() {
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowAction, kTagAction) || !sg::NameWatchSettled(kRowUsedOn, kTagUsedOn)) return;
    g_settled = true;
    const bool a = sg::NameWatchLive(kRowAction, kTagAction), u = sg::NameWatchLive(kRowUsedOn, kTagUsedOn);
    if (a && u) UE_LOGI("butter_churn: the watches on the churn's actions and pour are live");
    else UE_LOGE("butter_churn: a watch is dead (action=%d usedOn=%d) -- the churn is each machine's own", a ? 1 : 0,
                 u ? 1 : 0);
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot <= 0) return;
    const int n = ue_wrap::world_instances::Find(kChurnClass, g_churns, kCap);
    int sent = 0;
    for (int i = 0; i < n; ++i)
        if (IsChurn(g_churns[i]) && SendState(g_churns[i], slot, false)) ++sent;
    if (sent) UE_LOGI("butter_churn: HOST sent %d churns to joining slot %d", sent, slot);
}

void OnAsk(int senderSlot, const N::ButterChurnAskPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    void* const churn = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::Prop);
    float* const milk = FloatAt(churn, s_milk);
    bool butter = false;
    if (!IsChurn(churn) || !milk || !ReadButter(churn, butter)) {
        UE_LOGW("butter_churn: HOST slot %d asked op %u of eid %u, which is no churn here -- refused", senderSlot, p.op, p.eid);
        return;
    }
    const uint64_t now = ::GetTickCount64();
    const bool soon = now - g_lastAt[senderSlot] < kGapMs;
    g_lastAt[senderSlot] = now;
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(senderSlot));
    void* const body = rp ? rp->actor() : nullptr;
    ue_wrap::FVector bodyAt{}, at{};
    const bool reach = body && E::TryGetActorLocation(body, bodyAt) && E::TryGetActorLocation(churn, at) &&
                       (bodyAt.X - at.X) * (bodyAt.X - at.X) + (bodyAt.Y - at.Y) * (bodyAt.Y - at.Y) +
                               (bodyAt.Z - at.Z) * (bodyAt.Z - at.Z) <= kReachCm * kReachCm;
    if (soon || !reach) {
        UE_LOGW("butter_churn: HOST slot %d's op %u of churn eid %u refused: %hs", senderSlot, p.op, p.eid,
                soon ? "too soon after its last" : "its body is not within 6 m");
        SendState(churn, senderSlot, false);  // the asker's copy takes the host's
        return;
    }
    if (p.op == kOpChurn) {
        const bool ran = RunAction(churn, kActChurn, nullptr);  // its end is watched: every client hears it
        UE_LOGI("butter_churn: HOST slot %d churned churn eid %u (ran=%d)", senderSlot, p.eid, ran ? 1 : 0);
    } else if (p.op == kOpPour) {
        if (*milk <= 0.f && !butter) {
            *milk = kPourMilk;
            UE_LOGI("butter_churn: HOST slot %d poured milk into churn eid %u", senderSlot, p.eid);
            SendState(churn, -1, false);
        } else {
            UE_LOGW("butter_churn: HOST slot %d's pour into churn eid %u refused: it holds milk or butter", senderSlot,
                    p.eid);
            SendState(churn, senderSlot, false);
        }
    } else if (butter) {
        WriteButter(churn, false);
        *milk = 0.f;
        UE_LOGI("butter_churn: HOST slot %d takes churn eid %u's butter", senderSlot, p.eid);
        SendState(churn, senderSlot, true);
        SendState(churn, -1, false);
    } else {
        UE_LOGW("butter_churn: HOST slot %d's take of churn eid %u refused: no butter in it", senderSlot, p.eid);
        SendState(churn, senderSlot, false);
    }
}

void OnState(const N::ButterChurnStatePayload& p) {
    if (!IsClient() || TakeState(p)) return;
    if (p.grant) return;  // a grant for a copy not here is not kept: the butter stays the host's to give again
    for (N::ButterChurnStatePayload& h : g_held)
        if (h.eid == p.eid) {
            h = p;
            return;
        }
    if (g_held.size() < kMaxHeld) g_held.push_back(p);
}

void OnBound(uint32_t eid, void* actor) {
    if (!IsClient() || !actor || g_held.empty()) return;
    for (size_t i = 0; i < g_held.size(); ++i) {
        if (g_held[i].eid != eid) continue;
        const N::ButterChurnStatePayload h = g_held[i];
        g_held.erase(g_held.begin() + static_cast<std::ptrdiff_t>(i));
        TakeState(h);
        return;
    }
}

void OnClientWorldReady() {
    g_held.clear();
    g_applying = nullptr;
    g_pouring = nullptr;
}

void OnDisconnect() {
    OnClientWorldReady();
    for (uint64_t& t : g_lastAt) t = 0;
}

}  // namespace coop::butter_churn_sync
