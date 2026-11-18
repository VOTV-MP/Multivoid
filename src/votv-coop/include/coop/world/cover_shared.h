// coop/world/cover_shared.h -- what the two halves of the cover lane share: cover_sync.cpp (the client's asks and the
// state it applies, and the watches) and cover_judge.cpp (the host's judge). Defined in cover_sync.cpp. Game thread.
#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::cover_sync {

coop::net::Session* Session();
bool IsHost();
bool IsClient();
uint64_t NowMs();
float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

// The plates' byte as ue_wrap::analog_desk::ReadPlates gives it: on in bits 0-3, unbolted in bits 4-7.
inline bool PlateOn(uint8_t bits, int i) { return (bits >> i) & 1u; }
inline bool PlateLoose(uint8_t bits, int i) { return (bits >> (4 + i)) & 1u; }

// The desk's and a station's own calls this lane makes, which the gate lets run: the plates written and repainted,
// the detach sound, the door swung.
bool WritePlates(void* desk, uint8_t bits);  // false when the arrays were not written (no repaint then)
void SoundDetach(void* desk, const ue_wrap::FVector& at);
void OpenDoor(void* gen, bool open);

// A station's `opened` and `moving`, `fallback` when unread.
bool OpenedOr(void* gen, bool fallback);
bool MovingOr(void* gen, bool fallback);

// Whether `prop` is more than 80 cm from the plate it came off.
bool AwayFromPlate(void* desk, uint8_t plate, void* prop);

// [dev] Whether this lane's own call into the desk or a station is running (the gate lets it through).
bool Applying();

}  // namespace coop::cover_sync
