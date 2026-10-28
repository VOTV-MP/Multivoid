// coop/net/receive_budget.h -- the host's bound on what each seated client sends it, counted at the receive edge on
// every packet. A stream type has a bucket per slot at twice the send rate; a datagram past it is dropped (a lost pose
// is one the next replaces, a lost voice frame a gap the decoder conceals), capping what the host relays. Every other
// packet counts against one message bucket per slot; a client that empties it is past anything a client of this build
// sends, so the host ends its link (Session::TakeBudget). A kind's own cost is its lane's. Net thread only, no lock.
//
// MTA counts each event a client triggers on an element that resolves, refused or not, toward a per-player threshold
// and raises an event for the server's scripts past it (CGame.cpp:2663-2711,4927-4976). Ours counts every packet,
// sync and an unresolvable one included, so its numbers sit far above MTA's 100 a second; a token bucket holds a burst
// and a rate in one, where MTA's fixed window, opened by the first event, passes up to twice its count across a
// boundary; the host answers by ending the link; and a rate of 0 turns it off, which MTA's row cannot
// (mtaserver.conf:316-319). MTA relays the puresync of a player in zone 0 every time and paces the farther zones for
// bandwidth (CPlayer.cpp:981-1028, CBandwidthSettings.h:37-55); the stream bound is Source's shape, itself a token
// bucket: +1 a server tick, capped, spent per command, the rest dropped (player.cpp:3243,3392-3414,
// player_command.cpp:318-335).

#pragma once

#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"             // MsgType
#include "coop/player/players_registry.h"  // kMaxPeers

#include <cstdint>

namespace coop::net {

class ReceiveBudget {
public:
    // The host's rows, applied at its session start: `burst` messages at once, refilled at `perSecond`. A rate of 0
    // turns the message bound off.
    void Configure(long burst, long perSecond);
    // The streams' bound: `perSecond` datagrams a second per slot and type, as many at once. 0 turns it off.
    void ConfigureStreams(int perSecond);

    // One packet of `type` from `slot`'s connection `hConn` at `nowMs`. A new connection on the slot starts full. A
    // stream type is counted against its own bucket and only dropped past it (StreamOver); anything else is a message:
    // Emptied on the packet that empties the message bucket, Spent on every later one from that connection.
    enum class Take { Ok, StreamOver, Emptied, Spent };
    Take TakePacket(int slot, uint32_t hConn, MsgType type, uint64_t nowMs);

    const IntentBudget& Budget() const { return budget_; }

    // The arithmetic: a burst, its refill, the latch, a new connection's full bucket, the streams apart, a rate of 0.
    static bool RunSelftest();

private:
    // The stream types a client sends: the nine scalar channels, voice and the driven-prop rows of a
    // prop it syncs.
    static constexpr int kStreamTypes = 11;
    static int StreamIndex(MsgType type);  // -1 for a type counted as a message

    struct Slot {
        uint32_t     hConn = 0;
        bool         spent = false;  // emptied once: latched until the slot's connection changes
        IntentBucket messages;
        IntentBucket streams[kStreamTypes];
    };
    Slot& For(int slot, uint32_t hConn);

    IntentBudget budget_{0.0f, 0.0f};  // off until Configure: the numbers live in the host's rows
    IntentBudget streamBudget_{0.0f, 0.0f};
    Slot slots_[coop::players::kMaxPeers];
};

}  // namespace coop::net
