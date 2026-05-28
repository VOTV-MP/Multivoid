// coop/element/player.h -- the Player Element subclass.
//
// Second subclass of `coop::element::Element` (after Npc). Each remote peer
// gets a Player Element whose ElementId is the unified runtime address used
// by future event_feed dispatch (`Registry::Get(id) -> Element*` resolves
// uniformly across Player/Npc/Prop without per-type switch cases).
//
// The Player Element does NOT own the engine actor or the `coop::RemotePlayer`
// puppet object -- those stay in `coop::players::Registry` and `harness.cpp`
// respectively. The Element carries:
//   - `m_peerSlot` (uint8_t): the legacy peer-slot id [0, kMaxPeers). Wire
//     payloads still use `peerSessionId` (uint8_t) -- this slot is the same
//     value, kept on the Element for human-readable diagnostic logs +
//     compatibility while the v12 protocol bump (rename to ElementId) is
//     still queued. `PeerSlot()` is the accessor; `m_id` (Element::GetId())
//     is the unified ElementId.
//   - `m_puppet` (RemotePlayer*): the puppet for this peer, OR nullptr for
//     the local peer's own Player Element (the local doesn't have a puppet
//     -- it IS the local player).
//
// Lifecycle: owned by `coop::players::Registry` keyed by peerSlot via a
// fixed-size `std::unique_ptr<Player>[kMaxPeers]`. Constructed by
// `RegisterPuppet` (or by the local-init path for the local peer in
// `SetLocalPeerId`). Destroyed by `UnregisterPuppet` (or by replacement
// in `EnsurePlayerElement_` when the puppet pointer changes for the same
// slot). There is no global `Registry::Reset` -- per-subsystem drain only.
//
// Per the audit (`research/findings/votv-mta-cclientelement-audit-2026-05-28.md`
// section 4.3): MTA's `CClientPlayer` is just a refinement of `CClientPed`.
// Local and remote are the same class; the discriminator is `IsLocalPlayer()`.
// Our scope keeps the local-vs-puppet split inside `coop::players::Registry`
// (the local is tracked via `localCached_`; puppets via `puppetByPeer_`).
// `Player` here is the Element shadow of EITHER -- the m_puppet pointer
// distinguishes them.

#pragma once

#include "coop/element/element.h"

#include <cstdint>

namespace coop { class RemotePlayer; }

namespace coop::element {

class Player : public Element {
public:
    Player(uint8_t peerSlot, coop::RemotePlayer* puppet)
        : Element(ElementType::Player),
          m_peerSlot(peerSlot),
          m_puppet(puppet) {}

    uint8_t              PeerSlot() const { return m_peerSlot; }
    coop::RemotePlayer*  Puppet() const   { return m_puppet; }
    bool                 IsLocal() const  { return m_puppet == nullptr; }

private:
    uint8_t              m_peerSlot;
    coop::RemotePlayer*  m_puppet;  // nullptr for the local peer
};

}  // namespace coop::element
