// coop/permissions/grants_core.cpp -- see coop/permissions/grants_core.h.

#include "coop/permissions/grants_core.h"

namespace coop::permissions::grants {

const Entry kProjected[static_cast<size_t>(Projected::Count)] = {
    {"multivoid.dev.local.freecam", "The free camera (without its teleport).", false},
    {"multivoid.dev.local.hud", "The position readout.", false},
    {"multivoid.dev.local.overlay", "The object and ragdoll overlays.", false},
    {"multivoid.dev.local.stamina", "Set your own stamina low.", false},
    // A product right, not a developer tool: it sits under the dev.local prefix because that prefix names this table,
    // the rights the host projects to a machine as one bit each, and the cheat tools are projected the same way.
    // Granted unless the host withholds it; it counts only while the host's net.allow_player_cheats is on.
    {"multivoid.dev.local.cheats", "Use the host's cheat tools while the host allows players to.", true},
};

uint64_t Pack(uint32_t serial, uint32_t bits) {
    return (static_cast<uint64_t>(serial) << 32) | bits;
}

bool ReadBit(uint64_t packed, uint32_t currentSerial, bool isHost, Projected n) {
    if (static_cast<uint32_t>(packed >> 32) != currentSerial) return isHost;
    return ((static_cast<uint32_t>(packed) >> static_cast<uint32_t>(n)) & 1u) != 0;
}

bool Due(const DueState& last, uint64_t revision, uint32_t serial, int64_t nowSeconds) {
    return revision != last.revision || serial != last.serial ||
           (last.earliest != 0 && nowSeconds > last.earliest);
}

int64_t MergeEarliest(int64_t a, int64_t b) {
    if (a == 0) return b;
    if (b == 0) return a;
    return a < b ? a : b;
}

}  // namespace coop::permissions::grants
