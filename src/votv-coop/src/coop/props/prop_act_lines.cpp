// coop/props/prop_act_lines.cpp -- see coop/props/prop_act_lines.h.

#include "coop/props/prop_act_lines.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"

#include "l10n/l10n.h"

#include <string>

namespace coop::prop_act_lines {
namespace {

// A line is shown at most once per kind per window (D1's shape, remover_request.cpp).
enum Line : size_t { kLineGone, kLineWrongFamily, kLineHeld, kLineReach, kLineQueueFull, kLineBadReason,
                     kLineNotAsked, kLineNotReady, kLineBusy, kLineCount };
constexpr uint64_t kShowEveryMs = 5000;
uint64_t g_shownAt[kLineCount] = {};
bool g_shown[kLineCount] = {};

void Show(Line which, const char* line) {
    const uint64_t now = coop::net::NowMs();
    if (g_shown[which] && now - g_shownAt[which] < kShowEveryMs) return;
    g_shown[which] = true;
    g_shownAt[which] = now;
    coop::chat_feed::Push(std::string(line), coop::chat_feed::Keep::Transient);
}

}  // namespace

void ShowRefusal(uint8_t reason) {
    switch (static_cast<coop::net::PropActReason>(reason)) {
    case coop::net::PropActReason::Gone:
        Show(kLineGone, l10n::T("That item is gone."));
        return;
    case coop::net::PropActReason::WrongFamily:
        Show(kLineWrongFamily, l10n::T("You can't do that with this item."));
        return;
    case coop::net::PropActReason::Held:
        Show(kLineHeld, l10n::T("Someone else has that item."));
        return;
    case coop::net::PropActReason::Reach:
        Show(kLineReach, l10n::T("That item is too far away."));
        return;
    case coop::net::PropActReason::QueueFull:
        Show(kLineQueueFull, l10n::T("Too many actions at once, try again."));
        return;
    default:
        return;
    }
}

void ShowBadReason() { Show(kLineBadReason, l10n::T("That can't be done right now.")); }
void ShowNotAsked() { Show(kLineNotAsked, l10n::T("The host could not be asked; try again.")); }
void ShowNotReady() { Show(kLineNotReady, l10n::T("Not ready yet, try again in a moment.")); }
void ShowBusy() { Show(kLineBusy, l10n::T("Another player is using that item.")); }

void Forget() {
    for (size_t i = 0; i < kLineCount; ++i) g_shown[i] = false;
}

bool NotReadyShown() { return g_shown[kLineNotReady]; }

}  // namespace coop::prop_act_lines
