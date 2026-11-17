// coop/net/net_hooks.h -- the game-side calls a session makes, as function pointers the host
// program passes to Session::Start. The net layer reaches no engine object: what it needs from the
// game (the host's own player element, the host's pose ledger, the drive-merge counter) is a hook,
// and a null hook is "nothing to do", which is every hook of a program with no game behind it. They
// are stored with the session's config, for every role, before the net thread exists.

#pragma once

#include <cstdint>

namespace coop::net {

class Session;

// What AssignPeerSlot carries when no hook answers: the same "no element yet" value the registry
// answers before the host's player element exists, on which a client falls back to non-mirror routing.
inline constexpr uint32_t kNoHostElementId = 0xFFFFFFFFu;

struct NetHooks {
    // The host's own Player element id, sent to a joining client in AssignPeerSlot. Net thread.
    uint32_t (*localPlayerElementId)() = nullptr;
    // One accepted, fresh pose a client sent (host only), after the per-origin freshness check; the
    // host's movement ledger bills it. `stateTimeMs24` is the wire stamp. Net thread.
    void (*onClientPose)(Session& session, int slot, float x, float y, float z,
                         uint32_t stateTimeMs24) = nullptr;
    // One driven-prop row that passed the batch's format check, counted for the syncer drill.
    // Net thread.
    void (*noteDriveMerged)(uint32_t elementId) = nullptr;
    // The host's ban predicate over a joiner's PROVED player id and its connection's own address
    // (empty when the path is relayed or GNS knows none), asked right after the identity proof and
    // before a seat is given; true refuses the connection as banned, and whatever it wrote into
    // `whyOut` rides the close. Null admits everyone (a client, the load peer). MTA: the serial and
    // IP checks in CGame::Packet_PlayerJoinData (CGame.cpp:1956, :1973). Net thread.
    bool (*banCheck)(const char* playerId, const char* address, char* whyOut, int whyLen) = nullptr;
};

}  // namespace coop::net
