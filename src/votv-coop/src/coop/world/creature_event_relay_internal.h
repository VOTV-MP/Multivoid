// coop/world/creature_event_relay_internal.h -- what the three files of the creature event relay share
// (creature_event_relay.cpp, _host.cpp, _client.cpp). Declarations only; the definitions sit in the file named at
// each group. Impl-private: nothing outside coop/world includes this.

#pragma once

#include "coop/element/death_seam.h"
#include "coop/element/element.h"
#include "coop/net/session_config.h"

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
struct CreatureEventActivePayload;
}  // namespace coop::net

namespace coop::creature_event_relay::detail {

// A creature's count is held at 8 on the host and refused above it on the wire; at most 256 creatures hold one.
inline constexpr int    kMaxCount   = 8;
inline constexpr size_t kMaxEntries = 256;

// ---- creature_event_relay.cpp ----------------------------------------------------------------
// The cached session when it is connected with `role`, else null.
coop::net::Session* ActiveSession(coop::net::Role role);

// The context of the relay's own setEvent call while it runs, else null: the client's PRE lets it through.
extern void* g_applyingCtx;

// Does `actor`'s class chain name one of the four per-viewer classes? Game thread.
bool IsPerViewer(void* actor);

// lib_C::setEvent(active, false, ctx) by reflection on the library's default object, with g_applyingCtx = ctx
// around it. A `false` at a read count of 0 is skipped with a WARN. True when the call ran or was skipped, false
// when it could not be made. Game thread.
bool Call(bool active, void* ctx);

// True once setEvent, the event registry and the library's default object resolve: everything Call needs.
bool Ready();

// ---- creature_event_relay_host.cpp -----------------------------------------------------------
// The eid when the registry names `actor` an Npc element, a mirror or not as asked; kInvalidId otherwise.
coop::element::ElementId NpcEidOf(void* actor, bool mirror);

// How many copies of `actor` the game's own event registry holds; false when it cannot be read.
bool CopiesInRegistry(void* actor, int& copies);

void HostPost(void* ctx, bool active);
void HostOnNpcEnrolled(coop::element::ElementId eid, void* actor);
void HostOnNpcEnd(const coop::element::death_seam::Death& death);
void HostSendJoinSnapshot(coop::net::Session* session, int slot);
void HostOnDisconnect();

// ---- creature_event_relay_client.cpp ---------------------------------------------------------
// True when the call is the mirror's own and must not run.
bool ClientPre(void* ctx, bool active);
void ClientOnReliable(const coop::net::CreatureEventActivePayload& payload);
void ClientOnMirrorBound(coop::element::ElementId eid, void* actor);
void ClientOnNpcEnd(const coop::element::death_seam::Death& death);
void ClientOnDisconnect();
int  ClientTargetFor(coop::element::ElementId eid);
int  ClientAppliedFor(coop::element::ElementId eid);
uint32_t ClientViewerTrues(coop::element::ElementId eid);
uint32_t ClientViewerFalses(coop::element::ElementId eid);

}  // namespace coop::creature_event_relay::detail
