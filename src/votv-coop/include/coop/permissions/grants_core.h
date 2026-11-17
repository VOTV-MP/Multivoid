// coop/permissions/grants_core.h -- the local grants' engine-free core: the table of nodes a machine
// shows locally (the dev features, and a player's right to the host's cheat tools), their packing into one 64-bit word, the rule that reads a bit,
// and the rule that says when the host computes them again. Time and the session's serial are
// parameters; nothing here logs, reads a clock or includes the engine layer, so the selftest drives
// it whole and the future arbiter links it as it is. The session glue is coop/session/local_grants
// (the lock-free answer) and coop/session/grants_sync (the host's tick and kind 168).
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::permissions::grants {

// A node a machine shows locally, in wire order: kind 168 carries one bit per entry, entry 0 the
// lowest bit.
enum class Projected : uint8_t { Freecam, Hud, Overlay, Stamina, Cheats, Count };

static_assert(static_cast<size_t>(Projected::Count) <= 32, "the bits travel in a uint32_t");

struct Entry {
    const char* node;
    const char* description;
    bool defaultGranted;
};

// A WIRE table (kind 168 carries its bits in this order): adding, removing or reordering an entry
// bumps kProtocolVersion.
extern const Entry kProjected[static_cast<size_t>(Projected::Count)];

// The serial in the high half, the bits in the low half: one word is one atomic store, so a reader
// never pairs one session's serial with another's bits.
uint64_t Pack(uint32_t serial, uint32_t bits);

// The bit of `n` when the word was written for `currentSerial`; otherwise the word is another
// session's and reads as the role's default: the host (the owner of the session) passes until its
// first compute, a client has nothing.
bool ReadBit(uint64_t packed, uint32_t currentSerial, bool isHost, Projected n);

// What the host knew when it last computed: the model's revision, the session's serial, and the
// earliest expiry of any answer it computed (0 = none).
struct DueState {
    uint64_t revision = 0;
    uint32_t serial = 0;
    int64_t earliest = 0;
};

// True when the bits must be computed again: the model changed, a new session began, or the clock
// is past the earliest expiry (now > earliest, as the resolver's own cache, resolution.cpp).
bool Due(const DueState& last, uint64_t revision, uint32_t serial, int64_t nowSeconds);

// The earlier of two expiries, 0 meaning none.
int64_t MergeEarliest(int64_t a, int64_t b);

}  // namespace coop::permissions::grants
