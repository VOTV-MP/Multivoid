// coop/dev/cord_pos_drill_detail.h -- what the late-mirror drill's files share: its bounds, the makers' recipe, the
// rest clock and the roles' entry points. Impl-private: nothing outside coop/dev includes it. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::cord_pos_drill::detail {

// ---- bounds: failure bounds of the instrument, never a wait for a milestone ----

// A step not done this long after it began (the host's cords, a maker's rest; the client's whole judgement from its
// first bind).
inline constexpr uint64_t kFailMs = 90000;

// ---- the recipe ----

inline constexpr int kMakers = 3;
// The first maker: the coffee drill's socket by its save key, and the spot measured there (pos5).
inline constexpr const char* kFirstSocketKey = "vsQS8StrlE21NUBpOlQRrA";
inline constexpr float kFirstSpotX = -1565.f, kFirstSpotY = 2502.f, kFirstSpotZ = 6780.f;
// The other two makers: a socket's spot is this far out from its face, a ledge when the surface under it lies within
// kLedgeBandCm of the socket's height and nothing lies within kDropCm below that surface.
inline constexpr float kSpotOutCm = 80.f;
inline constexpr float kLedgeBandCm = 40.f;
inline constexpr float kDropCm = 200.f;
inline constexpr float kLiftCm = 30.f;      // a maker is spawned this far above its ledge
// Without two ledge sockets: spots beside the first, this far along the wall (plus and minus).
inline constexpr float kAlongWallCm = 60.f;
// A cord breaks at 460 uu from its root (cord.cpp); a socket the fallback plugs a maker into stays within this.
inline constexpr float kReachCm = 300.f;

// ---- the judgement ----

inline constexpr float kRestVelCmS = 1.f;          // a copy is at rest under this speed...
inline constexpr uint64_t kHostQuietMs = 1000;     // ...for this long (the host's)
inline constexpr uint64_t kClientQuietMs = 10000;  // ...for this long (the client's second parity read)
inline constexpr uint64_t kRevealReadMs = 2000;    // the client's first parity read: this long after the reveal
inline constexpr float kFellCm = 5.f;              // a hidden copy may lie this far under the host's rest
inline constexpr float kParityCm = 5.f;            // and a shown copy this far from it

// "at rest for `needMs`": the clock of one copy.
struct RestClock {
    uint64_t quietSince = 0;
    bool Update(void* actor, uint64_t nowMs, uint64_t needMs);
};

uint64_t NowMs();
// The maker's prop key, narrow; "?" when it has none.
std::string KeyOf(void* maker);
// The host's chat line when it cannot stage the drill: the client ends its run on it.
inline constexpr const char* kInvalidLine = "cordpos invalid";
// The host's chat line for one maker's rest, and the prefix the client reads it by.
std::string RestPrefix(const std::string& key);

void HostTick(coop::net::Session* s);
void HostReset();

void ClientTick(coop::net::Session* s);
void ClientOnBound(uint32_t eid, void* actor);
void ClientOnHidden(uint32_t eid, void* actor);
void ClientReset();

}  // namespace coop::dev::cord_pos_drill::detail
