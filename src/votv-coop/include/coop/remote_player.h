// coop/remote_player.h -- the network-driven remote player.
//
// Gameplay/network layer (principle 7). Parallel class hierarchy (principle 3):
// RemotePlayer owns the network/coop state and a POINTER to the engine entity it
// renders through -- a "skin-puppet": a bare ASkeletalMeshActor wearing the local
// player's exact skin (mesh_playerVisible) + the body AnimBP, which WE drive
// entirely (no pawn, no controller, no CharacterMovement). The engine actor owns
// rendering/animation; RemotePlayer owns the pose data. (MTA shape:
// CClientPed::m_pPlayerPed -> CPlayerPed*.)
//
// Why a puppet, not a 2nd mainPlayer_C: the protagonist class dragged its whole
// single-player tick surface along (the hijack/gamma cascade) and its AnimBP
// posed as a "stick" without a possessing controller. The puppet poses purely
// from AnimBP variables we set (root-cause finding, two converging agents) and
// has no hijack surface (RULE 1 root-cause; RULE 3 parallel hierarchy).
//
// No marshaling/engine-memory access lives here; it all goes through ue_wrap.

#pragma once

#include "ue_wrap/types.h"

#include <string>

namespace coop {

class RemotePlayer {
public:
    // Spawn the puppet in the live world, offset from the local player, wearing
    // the local player's skin. Requires gameplay loaded (mainPlayer_C present)
    // and the game thread. Returns true on success; sets actor().
    bool Spawn();

    bool valid() const;

    // Apply a full pose snapshot from a network packet (Phase 3): absolute
    // position (teleport), horizontal facing (yaw, degrees), and walk speed
    // (cm/s -> the AnimBP locomotion blend). The single per-frame apply path.
    void Drive(const ue_wrap::FVector& location, float yaw, float speed);

    // Apply an absolute position only (teleport, no sweep). Kept for the harness /
    // verification; Drive() is the network path.
    bool SetLocation(const ue_wrap::FVector& location);

    // Set horizontal facing independently of where the local player looks.
    bool SetFacing(float yaw);

    // Current engine-reported location (for verification / interpolation base).
    ue_wrap::FVector GetLocation() const;

    // World point to anchor the floating nameplate: actor location (feet) + a Z
    // offset to float just above the head.
    ue_wrap::FVector GetHeadPosition() const;

    // The display nickname rendered above the body (set from the network
    // handshake; defaults to "Player 2" so the harness shows something).
    void SetNickname(std::wstring name);
    const std::wstring& GetNickname() const { return nickname_; }

    void* actor() const { return actor_; }

private:
    void* actor_ = nullptr;  // the engine ASkeletalMeshActor puppet (owned by the engine)
    std::wstring nickname_ = L"Player 2";
};

}  // namespace coop
