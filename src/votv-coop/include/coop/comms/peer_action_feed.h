// coop/comms/peer_action_feed.h -- announce a peer's shared-world action to the
// local chat feed (gameplay layer, principle 7).
//
// The extensible home for "a player did a shared thing everyone should see". The
// first caller is email deletion (coop::email_sync): each peer renders the line
// LOCALLY from the existing EmailDelete wire event (which already carries who =
// senderSlot and which = content hash) -- no new packet. Each peer decides whether
// it SEES these lines via the ui.chat.peer_actions toggle (F1 > Cosmetics > Chat),
// default ON -- a local view preference, not a wire broadcast.
//
// Renders "<nick> <action>" with the nick colored per slot (chat_feed::PushChat), or
// "You <action>" for the local actor. Announce()/SetEnabled() are GAME THREAD (the
// callers -- email_sync's poll/apply -- are game-thread). Enabled() is lock-free.
#pragma once

#include <cstdint>
#include <string>

namespace coop::peer_action_feed {

// Announce that a peer performed `action` (a predicate like
// L"deleted an email: Server Alert!"). `slot` is the actor's peer slot (drives the
// nick + its color); `isLocalActor` renders the subject as "You" instead of the
// nick. No-op unless Enabled(). Game thread.
void Announce(uint8_t slot, bool isLocalActor, const std::wstring& action);

// The ui.chat.peer_actions toggle. SetEnabled persists to votv-coop.ini + updates
// the live value; Enabled reads it (lazy-loads the persisted value on first call).
void SetEnabled(bool on);
bool Enabled();

}  // namespace coop::peer_action_feed
