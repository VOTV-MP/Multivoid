// coop/net/origin_context.h -- which occupancy of a slot a relayed stream packet belongs to. A client
// hears every other client through the host's relay, and a slot's next occupant (a rejoin from a new
// process) numbers its packets from zero while the receiver still holds the last occupant's highest
// number, so a newest-wins store refused the newcomer until its count passed the old one, and the
// departed occupant's stored pose respawned its puppet. The host numbers each occupancy of a slot and
// stamps the number into every packet it relays (the header's originContext) and into the slot's
// roster row. A receiver stores a relayed packet only when its number is the one the slot's roster
// names, or either is 0, and the roster alone moves that number. MTA's rule unchanged: a sync time
// context set only by a reliable packet (the player list and the spawn,
// reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp), a sync packet whose context
// differs ignored (CNetAPI.cpp, ReadPlayerPuresync), 0 accepting all (CClientEntity.cpp,
// CanUpdateSync). Ours alone is the retirement: MTA destroys a departed player's element, while a
// slot's streams here outlive its occupant, so the one that left is refused by name.
#pragma once

#include "coop/player/players_registry.h"  // kMaxPeers

#include <array>
#include <atomic>
#include <cstdint>

namespace coop::net {

class OriginContext {
public:
    static constexpr int kSlots = coop::players::kMaxPeers;

    // HOST: the context for the slot's newly admitted occupant, 1..255, one more than the last (0 is
    // the unnumbered value, skipped). Net thread.
    std::uint8_t Admit(int slot);
    // HOST: the current occupancy's context, for the relay and the roster row. Any thread.
    std::uint8_t Stamp(int slot) const;

    // RECEIVER, all under the session's remote-state lock.
    // May a relayed packet of context `ctx` be stored: it is the latched one and not retired, or either
    // is 0 (a packet the host did not number, or a slot whose roster row has not come yet).
    bool Accepts(int slot, std::uint8_t ctx) const;
    // A roster row names the slot's live occupancy, which is latched. True when it replaces another
    // occupancy's: the caller resets the slot's streams, which were that one's.
    bool Conform(int slot, std::uint8_t ctx);
    // A roster edge says the occupancy `ctx` left: latched as retired, so its late packets are refused
    // until a row names the next. True: the caller resets the slot's streams.
    bool Retire(int slot, std::uint8_t ctx);
    void Clear(int slot);

private:
    std::array<std::atomic<std::uint8_t>, kSlots> hostLast_{};
    std::array<std::uint8_t, kSlots> latched_{};
    std::array<bool, kSlots> retired_{};
};

}  // namespace coop::net
