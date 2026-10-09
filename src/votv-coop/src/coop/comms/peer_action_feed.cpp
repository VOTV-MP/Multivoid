// coop/comms/peer_action_feed.cpp -- see coop/comms/peer_action_feed.h.

#include "coop/comms/peer_action_feed.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_nick_color.h"
#include "coop/player/players_registry.h"
#include "coop/session/player_handshake.h"

#include "coop/config/config.h"

#include "l10n/l10n.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <mutex>
#include <string>

namespace coop::peer_action_feed {
namespace {

// Default ON; the persisted value (multivoid.ini ui.chat.peer_actions) is loaded on
// first access (Enabled/Announce) via g_loadOnce -- no session-install hook needed.
std::atomic<bool> g_enabled{true};
std::once_flag    g_loadOnce;

void EnsureLoaded() {
    std::call_once(g_loadOnce, [] {
        g_enabled.store(coop::config::ResolveFlag(coop::config_registry::rows::ui_chat_peer_actions),
                        std::memory_order_relaxed);
    });
}

// The row's subscriber. The lazy load runs first so a call_once still resolving on another
// thread cannot store an older read after this one.
void OnPeerActionsRowChanged() {
    EnsureLoaded();
    g_enabled.store(coop::config::ResolveFlag(::coop::config_registry::rows::ui_chat_peer_actions),
                    std::memory_order_relaxed);
}

// The sentences, one per Action and in its order: the actor is %1$s, a name %2$s, a count %2$lld.
// What argument each takes is the row's kind.
enum class Kind : uint8_t { None, Name, Count };
struct Row {
    Action      action;
    const char* msgid;
    const char* plural;   // a count's plural msgid, or null
    Kind        kind;
};
constexpr Row kRows[] = {
    {Action::DeletedEmail, L10N_MARK("%1$s deleted an email: %2$s"), nullptr, Kind::Name},
    {Action::CaughtSignal, L10N_MARK("%1$s caught signal '%2$s'"), nullptr, Kind::Name},
    {Action::UsingDevice, L10N_MARK("%1$s is using %2$s"), nullptr, Kind::Name},
    {Action::SoldFor, L10N_MARK_N("%1$s sold it for %2$lld point (the host's price)",
                                  "%1$s sold it for %2$lld points (the host's price)"), Kind::Count},
    {Action::SellNoSuchProp, L10N_MARK("%1$s could not sell that: the host does not have it"), nullptr, Kind::None},
    {Action::SellAlreadySold, L10N_MARK("%1$s could not sell that: it was already sold"), nullptr, Kind::None},
    {Action::SellNoGun, L10N_MARK("%1$s could not sell that: no coin gun exists in the host's world"), nullptr,
     Kind::None},
    {Action::SellNotSellable, L10N_MARK("%1$s could not sell that: the host's store will not take it"), nullptr,
     Kind::None},
    {Action::SellHostInternal, L10N_MARK("%1$s could not sell that: the host hit an internal error"), nullptr,
     Kind::None},
    {Action::SellTooFarAway, L10N_MARK("%1$s could not sell that: the host does not see you next to it"), nullptr,
     Kind::None},
    {Action::SellRefused, L10N_MARK("%1$s could not sell that"), nullptr, Kind::None},
    {Action::OrderTooMany, L10N_MARK("%1$s could not order: too many items in one order"), nullptr, Kind::None},
    {Action::OrderUnknownItem, L10N_MARK("%1$s could not order: an item was not in the host's store"), nullptr,
     Kind::None},
    {Action::OrderUnaffordable, L10N_MARK("%1$s could not order: there were not enough credits"), nullptr,
     Kind::None},
    {Action::OrderNoCatalog, L10N_MARK("%1$s could not order: the host could not read its store"), nullptr,
     Kind::None},
    {Action::OrderCommitFailed, L10N_MARK("%1$s could not order: the delivery could not be placed"), nullptr,
     Kind::None},
    {Action::OrderRefused, L10N_MARK("%1$s could not order: the host refused it"), nullptr, Kind::None},
};
static_assert(sizeof(kRows) / sizeof(kRows[0]) == static_cast<size_t>(Action::kCount),
              "one sentence per Action");

constexpr bool RowsInOrder() {
    for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); ++i)
        if (static_cast<size_t>(kRows[i].action) != i) return false;
    return true;
}
static_assert(RowsInOrder(), "kRows is indexed by Action");

const Row* RowOf(Action a) {
    const size_t i = static_cast<size_t>(a);
    return i < static_cast<size_t>(Action::kCount) ? &kRows[i] : nullptr;
}

// The actor is ALWAYS a nickname (the Minecraft feed principle): the local actor's own line renders
// its own nick, as every other peer sees it. Roster's resolution pattern (coop/player/roster.cpp).
std::string NickOf(uint8_t slot) {
    const bool isLocal = (slot == coop::players::Registry::Get().LocalPeerId());
    const std::wstring& nickW =
        isLocal ? coop::player_handshake::LocalNickname()
                : coop::player_handshake::NicknameForSlot(static_cast<int>(slot));
    return coop::chat_feed::ToUtf8(nickW.empty() ? std::wstring(L"Player") : nickW);
}

// The sentence in the player's language, formatted with the nick as argument 1, and the nick's span
// coloured per `slot`; the rest is the predicate in the ACTION colour (yellow), so a world-state
// action reads apart from typed chat.
void Say(uint8_t slot, const Row& row, const std::wstring& arg, long long n) {
    const std::string nick = NickOf(slot);
    char line[256];
    l10n::Span span;
    int written = -1;
    switch (row.kind) {
    case Kind::None:
        written = l10n::FmtSpan(line, sizeof(line), &span, l10n::T(row.msgid), nick.c_str());
        break;
    case Kind::Name: {
        const std::string name = coop::chat_feed::ToUtf8(arg);
        written = l10n::FmtSpan(line, sizeof(line), &span, l10n::T(row.msgid), nick.c_str(), name.c_str());
        break;
    }
    case Kind::Count: {
        const unsigned long long magnitude =
            n < 0 ? 0ull - static_cast<unsigned long long>(n) : static_cast<unsigned long long>(n);
        written = l10n::FmtSpan(line, sizeof(line), &span, l10n::Tn(row.msgid, row.plural, magnitude), nick.c_str(), n);
        break;
    }
    }
    if (written < 0) return;   // a format the grammar refuses: Fmt said so, once
    // An out-of-range slot (a local actor before slot assignment) clamps to 0 for the colour only.
    const uint8_t colorSlot = slot < coop::players::kMaxPeers ? slot : 0;
    const bool hasSpan = span.begin >= 0 && span.len > 0;
    coop::chat_feed::PushAction(line, static_cast<uint8_t>(hasSpan ? span.begin : 0),
                                static_cast<uint8_t>(hasSpan ? span.len : 0),
                                coop::chat_nick_color::ForSlot(colorSlot));
}

// A caller that names an action with the wrong argument shape is a defect at that call site.
bool Fits(const Row* row, Kind want) {
    if (row && row->kind == want) return true;
    static std::atomic<bool> s_said{false};
    if (!s_said.exchange(true))
        UE_LOGE("peer_action_feed: action %d announced with the wrong argument -- not shown",
                row ? static_cast<int>(row->action) : -1);
    return false;
}

}  // namespace

void SubscribeRow() {
    coop::config::Subscribe(::coop::config_registry::rows::ui_chat_peer_actions,
                            &OnPeerActionsRowChanged);
}

bool Enabled() {
    EnsureLoaded();
    return g_enabled.load(std::memory_order_relaxed);
}

void AnnounceDirect(uint8_t slot, Action action, const std::wstring& arg) {
    const Row* row = RowOf(action);
    if (!row || row->kind == Kind::Count) {
        Fits(row, Kind::None);
        return;
    }
    Say(slot, *row, arg, 0);
}

void Announce(uint8_t slot, Action action, const std::wstring& arg) {
    if (!Enabled()) return;
    AnnounceDirect(slot, action, arg);
}

void AnnounceDirectCount(uint8_t slot, Action action, long long n) {
    const Row* row = RowOf(action);
    if (!Fits(row, Kind::Count)) return;
    Say(slot, *row, std::wstring(), n);
}

const char* English(Action action) {
    const Row* row = RowOf(action);
    return row ? row->msgid : "?";
}

}  // namespace coop::peer_action_feed
