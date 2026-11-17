// coop/props/prop_pry_sync.cpp -- see coop/props/prop_pry_sync.h for the design.

#include "coop/props/prop_pry_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"  // OverrideClientSyncer: a replay that writes the copy ends a client's row on it
#include "coop/props/prop_stick_sync.h"  // the message, its fill and its let-go; the wall-attach owners it keeps
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_events.h"  // CallCrowbarOpen
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::prop_pry_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// Watched by name: every class's crowbarOpen fires, so the callbacks gate on the object first.
UE_FACT_FN(kRowCrowbarOpen, "prop_pry_sync", nullptr, L"crowbarOpen", AnyClass);
constexpr int kTagPry = 0x50505259;  // 'PPRY'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchAsked = false;

// Inside ApplyPried: the copy's own crowbarOpen is this peer taking another's pry, and is not sent
// back. Game thread, as the gate's callbacks are.
int g_replayDepth = 0;

Stats g_stats{};

// A pry this lane carries: of a prop, with no wall-attach component (those free themselves through
// the component's unstick, which the stick lane carries). A door is no prop.
bool OwnsPryOf(void* obj) {
    return obj && R::IsLive(obj) && ue_wrap::prop::IsDescendantOfProp(obj) &&
           !coop::prop_stick_sync::IsWallAttachable(obj);
}

coop::net::Session* ConnectedSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}

// The body has run on this peer's copy: the prop is sent as it now is, with the velocity the body left it.
void OnPryPost(const sg::Call& c) {
    if (g_replayDepth > 0) return;
    auto* s = ConnectedSession();
    if (!s || !OwnsPryOf(c.object)) return;
    coop::net::PropStickStatePayload p{};
    std::wstring keyW;
    if (!coop::prop_stick_sync::FillFreedProp(p, c.object, keyW)) return;
    p.flags = coop::net::kPropStickPried;
    s->SendReliable(coop::net::ReliableKind::PropStickState, &p, sizeof(p));
    ++g_stats.sent;
    UE_LOGI("prop_pry_sync: broadcast PRY key='%ls' eid=%u pose=(%.0f,%.0f,%.0f) angVel=(%.0f,%.0f,%.0f) deg/s",
            keyW.c_str(), p.elementId, p.locX, p.locY, p.locZ, p.angVelX, p.angVelY, p.angVelZ);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchAsked) return;
    g_watchAsked = true;
    if (!sg::WatchName(kRowCrowbarOpen, kTagPry, nullptr, &OnPryPost))
        UE_LOGE("prop_pry_sync: the script gate refused the watch on crowbarOpen -- for the rest of this "
                "process a pried plank or radiator changes only where it is pried");
}

void ApplyPried(void* prop, const coop::net::PropStickStatePayload& payload, const std::wstring& keyW,
                uint8_t senderPeerSlot) {
    UE_ASSERT_GAME_THREAD("prop_pry_sync::ApplyPried");
    if (!OwnsPryOf(prop)) {
        UE_LOGW("prop_pry_sync: PRY for key='%ls' eid=%u names a copy this lane does not pry (a wall-attach "
                "owner) -- dropped (slot %u)", keyW.c_str(), payload.elementId, senderPeerSlot);
        return;
    }
    // The replay writes the copy's flags, pose and physics, so a client that syncs the prop loses it
    // first, as to a host verb. A no-op off the host and for a prop with no client row.
    coop::prop_drive_host::OverrideClientSyncer(prop, "pry replay");
    ++g_replayDepth;
    const bool ran = ue_wrap::prop::CallCrowbarOpen(prop);
    --g_replayDepth;
    if (!ran) {
        UE_LOGW("prop_pry_sync: PRY key='%ls' eid=%u -- the copy's crowbarOpen did not run here; the copy stays "
                "as it is", keyW.c_str(), payload.elementId);
        return;
    }
    ++g_stats.applied;
    const bool letGo = R::IsLive(prop) && coop::prop_stick_sync::LetGoFrom(prop, payload);
    UE_LOGI("prop_pry_sync: PRY applied key='%ls' eid=%u (%s; slot %u)", keyW.c_str(), payload.elementId,
            letGo ? "let go from the sender's pose" : "another hold moves it", senderPeerSlot);
}

Stats GetStats() { return g_stats; }

void OnDisconnect() {
    if (g_stats.sent || g_stats.applied)
        UE_LOGI("prop_pry_sync: session end -- sent=%llu applied=%llu",
                static_cast<unsigned long long>(g_stats.sent), static_cast<unsigned long long>(g_stats.applied));
    g_stats = Stats{};
}

}  // namespace coop::prop_pry_sync
