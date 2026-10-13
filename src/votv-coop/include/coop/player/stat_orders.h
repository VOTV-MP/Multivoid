// coop/player/stat_orders.h -- an admin's order to, and query of, one player's stats and effects
// (StatOrder, StatOrderReply, StatQuery, StatQueryReply): the engine half of coop/player/
// stat_orders_wire. The host sends and the owner applies, because a player's stats exist only in
// its own process. Game thread throughout.
//
// An order sets one row of the stat table, adds a status effect or removes one. On a row the host
// stores, the owner sends its fresh profile BEFORE its answer, on the lane the profile rides, so
// the host's stored profile follows the owner's own and the host checks it before saying so. The
// host's own player (slot 0) is ordered and read in place by the same apply and the same read.

#pragma once

// MTA stamps a synced set with a sync-time context so a stale puresync cannot overwrite it
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CElement.cpp:1281); here the host never writes
// its copy at send time, so nothing is stamped. For the query pair, MTA's resendPlayerModInfo
// (reference/mtasa-blue/Server/mods/deathmatch/logic/luadefs/CLuaPlayerDefs.cpp:49) asks a client
// for data it alone holds, and the Source SDK's convar query answers under a cookie and a status
// (reference/source-sdk-2013/src/public/engine/iserverplugin.h:36-43, 110-113, 160). The closer
// tagged-request and status-reply shape is MTA's takePlayerScreenShot
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:3329-3349,
// reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CPlayerScreenShotPacket.cpp:18-88); ours
// is stricter: the token, the slot and the generation must all match. Shapes only; the pull and the
// missing opt-out are explained at SendQuery.

#include "coop/net/session.h"
#include "coop/player/stat_orders_wire.h"

#include "ue_wrap/actors/vitals.h"

#include <cstdint>
#include <string>

namespace coop::stat_orders {

struct Order {
    Op op;
    ue_wrap::vitals::Field field;   // a set's row
    float value;                    // a set's value
    std::string effect;             // an effect op's name
    float strength;                 // an added effect's strength
    float seconds;                  // an added effect's seconds
};

enum class Sent : uint8_t { Ok, NotHost, BadName, NotReady, NoGuid, Busy, SendFailed };

// HOST. The checks, in order: NotHost; BadName (an effect op whose name fails ValidEffectName);
// slot 0, the host's own player, applied or read here with `done` called BEFORE the return, Ok;
// then NotReady (the slot is not connected or not world-ready), NoGuid (no proved GUID), Busy (the
// slot's allowance of that kind is out), SendFailed (the transport refused: a bad or dying slot,
// the token is taken back). For a client `done` is called later, once, from the answer's handler or
// the slot's disconnect. Anything but Ok: nothing was sent and `done` is never called. A closure
// owns what it touches.
Sent SendOrder(uint8_t slot, const Order& order, OrderDone done);
// The query is a pull. MTA's server holds health and armor fresh off the client's own puresync
// (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CPlayerPuresyncPacket.cpp:199, 327)
// and sets its stat array itself (reference/mtasa-blue/Server/mods/deathmatch/logic/CPed.h:199-203),
// so it reads its own copy. Here the host's stored profile follows only the nine profile rows, up
// to 30 s old; the other thirteen rows of the stat table and the status effects ride no stream, so
// the live value is in the owner's process alone. A client has no opt-out, unlike Source's
// FCVAR_SERVER_CANNOT_QUERY (reference/source-sdk-2013/src/public/tier1/iconvar.h:75) and MTA's
// allow_screen_upload, which a client answers with a DISABLED status
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:6339-6347), because the host is
// the session's admin.
Sent SendQuery(uint8_t slot, QueryDone done);

// How many orders and queries of `slot` still wait for an answer.
int PendingCount(uint8_t slot);

// Keep the session the sends go through. It is called on every pump tick (subsystems::Install)
// and by the order drill's Tick; an idempotent pointer store.
void Install(coop::net::Session* session);
// A slot's disconnect answers every token it holds with Left.
void OnSlotDisconnected(uint8_t slot);
// The session's end: every token answered Left; the kept session stays, as command_sync's.
void OnDisconnect();

// The state family's receivers (event_feed -> here). An order or a query comes from the host alone
// (the host's receive edge refuses a client's copy (client_authorship.h)); the host takes an answer only from
// the slot its token went to.
void HandleOrder(coop::net::Session& s, const coop::net::Session::ReliableMessage& m);       // client
void HandleOrderReply(coop::net::Session& s, const coop::net::Session::ReliableMessage& m);  // host
void HandleQuery(coop::net::Session& s, const coop::net::Session::ReliableMessage& m);       // client
void HandleQueryReply(coop::net::Session& s, const coop::net::Session::ReliableMessage& m);  // host

}  // namespace coop::stat_orders
