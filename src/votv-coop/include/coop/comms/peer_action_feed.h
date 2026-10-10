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
// The subject is ALWAYS the actor's nickname, so the local actor sees the same line everyone else
// sees. Announce*() are GAME THREAD, as every caller is; Enabled() is lock-free. The toggle follows
// its config row through SubscribeRow()'s subscriber (game thread).
#pragma once

#include <cstdint>
#include <string>

namespace coop::peer_action_feed {

// The one grammar owner for peer-attributed lines: a caller names an Action and its argument, and
// the feed holds the sentence -- one msgid per action, the actor's nick as %1$s wherever the
// translation puts it -- looks it up in the player's language (l10n), formats it and colours the
// nick's span (chat_feed::PushAction). A caller never composes text and never imports l10n; Source
// sends a token and its parameters for each client to resolve (UTIL_ClientPrintAll), and this is that
// shape inside one peer. An action whose sentence takes a name takes it as `arg`; SoldFor takes a
// count. The sentences are the table in peer_action_feed.cpp.
enum class Action : uint8_t {
    DeletedEmail,       // arg: the email's subject
    CaughtSignal,       // arg: the signal's name
    UsingDevice,        // arg: the device's name
    SoldFor,            // a count: the price the host minted
    SellNoSuchProp,
    SellAlreadySold,
    SellNoGun,
    SellNotSellable,
    SellHostInternal,
    SellTooFarAway,
    SellRefused,
    OrderTooMany,
    OrderUnknownItem,
    OrderUnaffordable,
    OrderNoCatalog,
    OrderCommitFailed,
    OrderRefused,
    kCount
};

// Announce that a peer performed `action`. `slot` is the actor's peer slot (drives the nick and its
// colour); the local slot resolves to LocalNickname(), any other to NicknameForSlot(). An action
// that takes no name ignores `arg`. No-op unless Enabled(). Game thread.
void Announce(uint8_t slot, Action action, const std::wstring& arg = {});

// Same rendering, NOT gated on the ui.chat.peer_actions toggle: for lines that are FUNCTIONAL
// feedback rather than cosmetic ambience (device_occupancy's busy-deny notice: suppressing it would
// reduce the deny to a bare click sound; a refused sale or order). Game thread.
void AnnounceDirect(uint8_t slot, Action action, const std::wstring& arg = {});

// AnnounceDirect for an action that takes a count (SoldFor): the plural form follows `n`.
void AnnounceDirectCount(uint8_t slot, Action action, long long n);


// The ui.chat.peer_actions toggle: Enabled reads the live value (lazy-loads the row's
// resolved value on first call). A change is a config SetValue; the subscriber applies it.
bool Enabled();

// Follow the `ui.chat.peer_actions` row: once, at boot.
void SubscribeRow();

}  // namespace coop::peer_action_feed
