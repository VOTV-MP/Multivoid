// coop/items/point_sack_intent.cpp -- see coop/items/point_sack_intent.h.

#include "coop/items/point_sack_intent.h"

#include "coop/element/intent_authority.h"     // IntentTarget: the sender's reach to the sack
#include "coop/element/registry.h"             // LivePropActor, the eid fallback
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"      // kMaxPeers
#include "coop/props/prop_element_tracker.h"   // FindLiveActorByKey

#include "ue_wrap/actors/prop.h"               // GetInteractableKeyString
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"             // DestroyActor
#include "ue_wrap/world/economy.h"             // AddPoints

#include <atomic>
#include <cstring>
#include <string>

namespace coop::point_sack_intent {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

constexpr const wchar_t* kSackClass = L"prop_pointSack_C";
constexpr const wchar_t* kVerb      = L"actionOptionIndex";
constexpr int kTag = 0x50534B31;  // 'PSK1'
// The game's own reach for a prop action: mainPlayer.armLength, as the coin collect uses it.
constexpr float kReachUU = 200.0f;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchRegistered = false;
bool g_watchSettled = false;
uint64_t g_sent = 0, g_paid = 0, g_refused = 0;

bool IsSack(void* actor) {
    if (!actor) return false;
    for (void* cls = R::ClassOf(actor); cls; cls = R::SuperStructOf(cls))
        if (R::ToString(R::NameOf(cls)) == kSackClass) return true;
    return false;
}

// CLIENT: the sack's own action, refused and sent to the host. The host's own press, and a solo
// game's, run the game's body.
sg::Verdict OnActionPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!IsSack(c.object)) return sg::Verdict::Run;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    // The registry's reverse names a mirror as well as a local: a sack loaded from the host's save or
    // received as a spawn is a mirror here, and a key-less one is named by this alone.
    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    coop::net::PointSackRedeemPayload p{};
    if (!key.empty() && key != L"None") coop::net::WireKeyFromString(key, p.key);
    p.elementId = eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
    if (p.key.len == 0 && p.elementId == 0u) {
        UE_LOGW("point_sack: this client's sack %p has neither a key nor an eid -- nothing to name; "
                "left alone (the host's sack stays)", c.object);
        return sg::Verdict::Cancel;
    }
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::PointSackRedeem, &p, sizeof(p))) {
        UE_LOGW("point_sack: redeem request not sent -- sack left intact for retry");
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    UE_LOGI("point_sack: this client's sack key='%ls' eid=%u sent to the host (%llu sent) -- the host pays it",
            key.c_str(), p.elementId, static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

void Refuse(uint8_t slot, const std::wstring& key, uint32_t eid, const char* why) {
    ++g_refused;
    if (g_refused <= 20 || g_refused % 50 == 0)
        UE_LOGI("point_sack: slot %u's sack key='%ls' eid=%u not paid (%s)", slot, key.c_str(), eid, why);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (g_watchSettled) return;
    if (!g_watchRegistered) {
        g_watchRegistered = sg::WatchClassName(kSackClass, kVerb, kTag, &OnActionPre, nullptr);
        if (!g_watchRegistered) {
            g_watchSettled = true;
            UE_LOGE("point_sack: the gate took no watch on %ls::%ls -- a client's sack pays only its own screen",
                    kSackClass, kVerb);
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::ClassNameWatchSettled(kSackClass, kVerb, kTag)) return;
    g_watchSettled = true;
    if (sg::ClassNameWatchLive(kSackClass, kVerb, kTag))
        UE_LOGI("point_sack: the watch on %ls::%ls is live", kSackClass, kVerb);
    else
        UE_LOGE("point_sack: the watch on %ls::%ls settled dead -- a client's sack pays only its own screen",
                kSackClass, kVerb);
}

void OnRedeem(coop::net::Session& session, const coop::net::PointSackRedeemPayload& p, uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("point_sack_intent::OnRedeem");
    if (session.role() != coop::net::Role::Host) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (p.elementId != 0u && !EL::Registry::IsAllowedHostAllocatedEid(p.elementId) &&
        !EL::Registry::IsAllowedPeerAllocatedEid(p.elementId)) {
        Refuse(senderSlot, key, p.elementId, "eid in neither band");
        return;
    }
    // Key first, as a sale resolves its prop: the eid is the keyless fallback. Index only, no walk.
    // A named key that does not resolve is that sack gone; it never falls back to the eid, which may
    // name another prop by now. Only a keyless sack is named by its eid.
    void* sack = nullptr;
    if (!key.empty()) sack = coop::prop_element_tracker::FindLiveActorByKey(key);
    else if (p.elementId != 0u) sack = EL::LivePropActor(static_cast<EL::ElementId>(p.elementId));
    // No sack is the ordinary end of a second request, or of two players opening one sack.
    if (!sack) { Refuse(senderSlot, key, p.elementId, "no such sack here -- already opened"); return; }
    if (!IsSack(sack)) { Refuse(senderSlot, key, p.elementId, "not a point sack"); return; }
    const auto tok = EL::IntentTarget::ForClientIntent(session, senderSlot, kReachUU);
    if (const auto sub = tok.Authorize(sack); !sub) {
        Refuse(senderSlot, key, p.elementId, EL::OutcomeName(sub.outcome));
        return;
    }
    static void* sCls = nullptr;
    static int32_t sOffPoints = -1;
    void* cls = R::ClassOf(sack);
    if (cls != sCls) { sCls = cls; sOffPoints = R::FindPropertyOffset(cls, L"points"); }
    if (sOffPoints < 0) { Refuse(senderSlot, key, p.elementId, "the sack's points did not resolve"); return; }
    int32_t points = 0;
    std::memcpy(&points, static_cast<const uint8_t*>(sack) + sOffPoints, sizeof(points));
    // Call success is dispatch, not consumption. Pay only after this incarnation is dead.
    ue_wrap::CachedObjRef incarnation;
    incarnation.Set(sack);
    ue_wrap::engine::DestroyActor(sack);
    if (incarnation.Is(sack)) {
        Refuse(senderSlot, key, p.elementId, "the sack is still live after destroy -- not paid");
        return;
    }
    if (!ue_wrap::economy::AddPoints(points)) {
        UE_LOGW("point_sack: slot %u's sack key='%ls' was consumed but the payment of %d did not run", senderSlot,
                key.c_str(), points);
        return;
    }
    ++g_paid;
    UE_LOGI("point_sack: HOST paid slot %u's sack key='%ls' eid=%u: %+d (%llu paid)", senderSlot, key.c_str(),
            p.elementId, points, static_cast<unsigned long long>(g_paid));
}

void OnDisconnect() {
    if (g_sent || g_paid || g_refused)
        UE_LOGI("point_sack: session end -- sent=%llu paid=%llu refused=%llu", static_cast<unsigned long long>(g_sent),
                static_cast<unsigned long long>(g_paid), static_cast<unsigned long long>(g_refused));
    g_sent = g_paid = g_refused = 0;
}

}  // namespace coop::point_sack_intent
