// coop/event_dispatch.h -- INTERNAL per-domain reliable-message handlers for
// coop::event_feed::Update's dispatch switch (extracted 2026-06-11: event_feed.cpp
// had grown to 1388 LOC, 112 from the 1500 hard cap; the case BODIES moved into
// three domain files, the exhaustive switch itself stays in event_feed.cpp so the
// kind->handler table remains greppable in one place).
//
// Sibling-internal header (the net/session_lanes.h precedent) -- NOT part of the
// public coop/ include surface; only event_feed.cpp and the event_dispatch_*.cpp
// translation units include it.
//
// Each Handle* function re-switches on msg.kind and contains the FULL original
// case body: payload-length check, trust-boundary validation (sender slot/role
// gates, finite/bounds checks, eid range), and the module dispatch. Kinds outside
// the family fall through their internal switch as no-ops (callers route only
// family members).

#pragma once

#include "coop/net/session.h"

#include <cstdint>

namespace coop::event_feed {

// PR-FOUNDATION-1 role-range validation for an inbound eid-carrying packet
// (see the definition's comment for the full trust model). Shared by the
// entity family (ItemActivate) and the world family (RedSky / LightningStrike /
// WeatherState). Defined in event_dispatch_world.cpp.
bool VerifySenderEidRange(int senderPeerSlot, uint32_t senderElementId,
                          const char* kind);

// Entity lifecycle + held-item family: PropRelease, PropSpawn, PropDestroy,
// PropConvert, EntitySpawn, EntityDestroy, ItemActivate.
// `localPlayer` threads into remote_prop for held-grab release / destroy safety.
void HandleEntityEvent(net::Session& session,
                       const net::Session::ReliableMessage& msg,
                       void* localPlayer);

// Keyed device-state family: DoorState/LightState/ContainerState/GarageDoorState/
// ApplianceState (the shared KeyedTogglePayload case), KeypadState,
// PowerControlState, AtvState, DroneState, OrderRequest, WindowCleanState,
// GrimeState, TrashPileState, DoorOpenRequest, KerfurConvert (v78 client apply).
// `localPlayer` threads into the KerfurConvert client apply (prop teardown/materialize).
void HandleStateEvent(net::Session& session,
                      const net::Session::ReliableMessage& msg,
                      void* localPlayer);

// Ambient / world-event family: FireflySpawn, InventoryPickup, TimeSync,
// SkyState, RedSky, LightningStrike, WeatherState.
void HandleWorldEvent(net::Session& session,
                      const net::Session::ReliableMessage& msg);

}  // namespace coop::event_feed
