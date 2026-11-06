// coop/dev/cremator_dev_support.cpp -- see cremator_dev_support.h.

#include "cremator_dev_support.h"  // co-located private header (src tree, not include/)

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstring>

namespace coop::dev::cremator_support {
namespace {

namespace C  = ue_wrap::cremator;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;

// The cremator's own begin-overlap of its `burning` component: the event that collects the sitter.
UE_FACT_FN(kRowBurningOverlap, "dev.cremator_support", L"cremator_C",
           L"BndEvt__cremator_burning_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
constexpr int kTagOverlap = 0x43445350;  // 'CDSP'

std::atomic<OverlapFn> g_onOverlap{nullptr};

// Post, on the game thread: the body has run, so the cremator's own begin-overlap handling is done.
void OnOverlapPost(const sg::Call& c) {
    const OverlapFn fn = g_onOverlap.load(std::memory_order_acquire);
    if (!fn || !c.object || !c.function || !c.locals) return;
    static void*   sFn = nullptr;
    static int32_t sOtherOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sOtherOff = ue_wrap::reflection::FindParamOffset(c.function, L"OtherActor");
    }
    if (sOtherOff < 0) return;
    void* other = nullptr;
    std::memcpy(&other, c.locals + sOtherOff, sizeof(other));
    fn(c.object, other);
}

}  // namespace

bool WatchBurningOverlap(OverlapFn onOverlap) {
    g_onOverlap.store(onOverlap, std::memory_order_release);
    static bool sWatching = false;
    if (!sWatching) sWatching = sg::WatchName(kRowBurningOverlap, kTagOverlap, nullptr, &OnOverlapPost);
    return sWatching;
}

bool SlotSettled(coop::net::Session& session, uint8_t slot) {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(slot);
    ue_wrap::FVector at{};
    return p && p->valid() && p->TryGetLocation(at) && session.IsSlotWorldReady(slot) &&
           coop::prop_snapshot::IsBracketClosed(slot) && !coop::join_window_baseline::IsLateWindowOpen(slot) &&
           session.SlotReliableIdle(slot);
}

bool FindFirebox(uint8_t slot, Firebox& out, const char*& why) {
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(slot);
    ue_wrap::FVector from{};
    if (!puppet || !puppet->TryGetLocation(from)) { why = "the client's puppet has no location"; return false; }
    const C::Nearest n = C::FindNearestFirebox(from);
    if (!n.actor) { why = "no cremator with a firebox is loaded"; return false; }
    if (!E::TryGetActorRotation(n.actor, out.rot)) { why = "the cremator's rotation is unreadable"; return false; }
    out.cremator = n.actor;
    out.at = n.at;
    return true;
}

bool OpenAndSend(coop::net::Session& session, uint8_t slot, Firebox& fb, const char*& why) {
    if (!C::OpenDoor(fb.cremator, fb.doorOpened)) { why = "the door's open() did not run"; return false; }
    coop::net::TeleportClientPayload pose{};
    pose.locX = fb.at.X; pose.locY = fb.at.Y; pose.locZ = fb.at.Z;
    pose.rotPitch = fb.rot.Pitch; pose.rotYaw = fb.rot.Yaw; pose.rotRoll = fb.rot.Roll;
    if (!session.SendReliableToSlot(slot, coop::net::ReliableKind::TeleportClient, &pose, sizeof(pose))) {
        why = "the teleport order was not sent";
        return false;
    }
    return true;
}

}  // namespace coop::dev::cremator_support
