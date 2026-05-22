// coop/remote_player.h -- the network-driven remote player.
//
// Gameplay/network layer (principle 7). Parallel class hierarchy (principle 3):
// RemotePlayer owns the network/coop state and a POINTER to the engine entity
// (a 2nd mainPlayer_C pawn); the engine APawn/ACharacter owns rendering,
// animation, physics. (MTA shape: CClientPed::m_pPlayerPed -> CPlayerPed*.)
//
// For now it carries the validated Phase 2.1 orphan-spawn primitive in C++:
// spawn an unpossessed 2nd mainPlayer_C and drive its pose by absolute
// teleport -- the path a received network snapshot will apply (Phase 3.4/3.5).
// It does NOT contain marshaling/engine-memory access; that all goes through
// ue_wrap. Talks to the engine only via ue_wrap (clean API).

#pragma once

#include "ue_wrap/types.h"

namespace coop {

class RemotePlayer {
public:
    // Spawn a 2nd mainPlayer_C in the live world (unpossessed), offset from the
    // local player. Requires gameplay loaded (mainPlayer_C class present) and
    // the game thread. Returns true on success; sets actor().
    bool Spawn();

    bool valid() const;

    // Apply an absolute pose (teleport, no sweep) -- the network snapshot path.
    bool SetLocation(const ue_wrap::FVector& location);

    // Current engine-reported location (for verification / interpolation base).
    ue_wrap::FVector GetLocation() const;

    void* actor() const { return actor_; }

private:
    void* actor_ = nullptr;  // the engine mainPlayer_C pawn (owned by the engine)
};

}  // namespace coop
