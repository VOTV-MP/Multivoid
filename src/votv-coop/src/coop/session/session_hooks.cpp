// coop/session/session_hooks.cpp -- see coop/session/session_hooks.h.

#include "coop/session/session_hooks.h"

#include "coop/moderation/ban_list.h"
#include "coop/net/session.h"
#include "coop/player/movement_ledger.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"
#include "ue_wrap/core/types.h"

namespace coop::session_hooks {

namespace {

uint32_t HostElementId() { return coop::players::Registry::Get().LocalPlayerElementId(); }
static_assert(coop::net::kNoHostElementId == coop::element::kInvalidId, "the registry's no-element value");

void ClientPose(coop::net::Session& s, int slot, float x, float y, float z, uint32_t stamp24) {
    coop::movement_ledger::OnClientPose(s, slot, ue_wrap::FVector{x, y, z}, stamp24);
}

void DriveMerged(uint32_t eid) { coop::prop_drive_stream::dev::NoteMerged(eid); }

bool BanCheck(const char* playerId, const char* address, char* whyOut, int whyLen) {
    return coop::ban_list::IsBanned(playerId, address, whyOut, whyLen);
}

}  // namespace

const coop::net::NetHooks& Game(coop::net::Role role) {
    static const coop::net::NetHooks host{&HostElementId, &ClientPose, &DriveMerged, &BanCheck};
    static const coop::net::NetHooks client{&HostElementId, &ClientPose, &DriveMerged, nullptr};
    return role == coop::net::Role::Host ? host : client;
}

}  // namespace coop::session_hooks
