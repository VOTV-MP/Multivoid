// coop/comms/peer_action_feed.h -- announce a peer's shared-world action to the local chat feed
// (gameplay layer, principle 7).
//
// The extensible home for "a player did a shared thing everyone should see". Email deletion
// (coop::email_sync) is one caller: each peer renders the line LOCALLY from the existing
// EmailDelete wire event, which already carries who (senderSlot) and which (a content hash), so
// no new packet is needed. Each peer decides whether it SEES these lines through the
// ui.chat.peer_actions toggle (F1 > Cosmetics > Chat), default on -- a local view preference,
// not a wire broadcast.
//
// Renders "<nick> <action>" with the nick coloured per slot (chat_feed::PushChat). The subject
// is ALWAYS the actor's nickname, so the local actor sees the same line everyone else sees.
// Announce(), AnnounceDirect() and SetEnabled() are GAME THREAD, as every caller is; Enabled()
// is lock-free.
#pragma once

#include <cstdint>
#include <string>

namespace coop::peer_action_feed {

// Announce that a peer performed `action` (a predicate like
// L"deleted an email: Server Alert!"). `slot` is the actor's peer slot (drives the
// nick + its color); the local slot resolves to LocalNickname(), any other to
// NicknameForSlot(). No-op unless Enabled(). Game thread.
void Announce(uint8_t slot, const std::wstring& action);

// Same rendering, NOT gated on the ui.chat.peer_actions toggle. The one grammar
// owner for peer-attributed lines that are FUNCTIONAL feedback rather than
// cosmetic ambience (device_occupancy's busy-deny notice: suppressing it would
// reduce the deny to a bare click sound). Game thread.
void AnnounceDirect(uint8_t slot, const std::wstring& action);

// The ui.chat.peer_actions toggle. SetEnabled persists to multivoid.ini + updates
// the live value; Enabled reads it (lazy-loads the persisted value on first call).
void SetEnabled(bool on);
bool Enabled();

}  // namespace coop::peer_action_feed
