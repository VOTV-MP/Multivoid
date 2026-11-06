// coop/creatures/kerfur_command_answer.cpp -- see coop/creatures/kerfur_command_answer.h.

#include "coop/creatures/kerfur_command_answer.h"

#include "coop/creatures/kerfur_command.h"
#include "coop/creatures/kerfur_disc_carry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/text/utf8_codec.h"

#include "l10n/l10n.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/votv_lib.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace coop::kerfur_command_answer {
namespace {

namespace KA = coop::net::kerfur_command_answer;
using coop::net::KerfurCommandAnswerPayload;

// The ids sent and not yet answered: as many as the host holds for a sender (kMaxPending queued) plus the burst its
// bucket lets through at once (kBudget.burst), the most it can have outstanding. A command the host ran is never
// answered, so its id stays until newer ones push it out; only a refusal or a wait names an id.
constexpr size_t kHeld = coop::kerfur_command::kMaxPending +
                         static_cast<size_t>(coop::kerfur_command::kBudget.burst);
// A line per kind for the first few, then the counts at the session's end say the rest.
constexpr uint64_t kLinesPerKind = 20;
// The game's own refusal hint type ("You need to hold a proper floppy disc", "This kerfur is active").
constexpr uint8_t kRefusalHintType = 1;

uint32_t g_nextId = 0;
// A command sent and not yet answered, and the disc it pledged: the pledge is evicted with its id.
struct Held {
    uint32_t id;
    std::optional<coop::kerfur_disc_carry::Pledge> pledge;
};
std::vector<Held> g_held;

struct Counts {
    uint64_t refusedSent = 0, waitsSent = 0, refusedShown = 0, waitsShown = 0, local = 0, foreign = 0, lost = 0;
    uint64_t consumedSent = 0, consumed = 0, consumeMiss = 0;
};
Counts g_counts;

const char* ReasonName(uint8_t reason) {
    switch (static_cast<KA::KerfurRefusal>(reason)) {
    case KA::KerfurRefusal::Busy:        return "busy";
    case KA::KerfurRefusal::Stale:       return "stale";
    case KA::KerfurRefusal::KillMode:    return "kill mode";
    case KA::KerfurRefusal::QueueFull:   return "queue full";
    case KA::KerfurRefusal::NotKerfur:   return "not a kerfur";
    case KA::KerfurRefusal::Unavailable: return "unavailable";
    case KA::KerfurRefusal::TooFar:      return "too far";
    }
    return "?";
}

// The player's own line for a refusal, in its language, or null for a value no build of this protocol sends.
const char* RefusalLine(uint8_t reason) {
    switch (static_cast<KA::KerfurRefusal>(reason)) {
    case KA::KerfurRefusal::Busy:        return l10n::T("That kerfur is busy with a task.");
    case KA::KerfurRefusal::Stale:       return l10n::T("That kerfur is no longer there.");
    case KA::KerfurRefusal::KillMode:    return l10n::T("That kerfur will not take orders.");
    case KA::KerfurRefusal::QueueFull:   return l10n::T("Too many kerfur commands at once, try again.");
    case KA::KerfurRefusal::NotKerfur:   return l10n::T("That cannot take kerfur commands.");
    case KA::KerfurRefusal::Unavailable: return l10n::T("Kerfur commands are not available right now.");
    case KA::KerfurRefusal::TooFar:      return l10n::T("You are too far from that kerfur.");
    }
    return nullptr;
}

const char* LineFor(uint8_t result, uint8_t reason) {
    if (result == KA::kResultWaits) return l10n::T("The host will run that kerfur command in a moment.");
    return result == KA::kResultRefused ? RefusalLine(reason) : nullptr;
}

// The line as the game's refusal hint on this machine's player; false when it could not be shown.
bool ShowLine(const char* line, std::wstring& text) {
    void* local = coop::players::Registry::Get().Local();
    return local && coop::text::FromUtf8Strict(line, std::strlen(line), &text) &&
           ue_wrap::votv_lib::AddHint(text, kRefusalHintType, local);
}

void Send(coop::net::Session* session, uint8_t slot, uint32_t requestId, uint8_t result, uint8_t reason) {
    if (!session || requestId == 0 || slot == 0 || slot >= coop::net::kMaxPeers) return;
    KerfurCommandAnswerPayload p{};
    p.requestId = requestId;
    p.result = result;
    p.reason = reason;
    if (!session->connected() ||
        !session->SendReliableToSlot(slot, coop::net::ReliableKind::KerfurCommandAnswer, &p,
                                     static_cast<int>(sizeof(p)))) {
        ++g_counts.lost;
        return;
    }
    const uint64_t n = result == KA::kResultWaits          ? ++g_counts.waitsSent
                       : result == KA::kResultConsumedHeld ? ++g_counts.consumedSent
                                                           : ++g_counts.refusedSent;
    if (n > kLinesPerKind) return;
    if (result == KA::kResultWaits)
        UE_LOGI("kerfur_command_answer: HOST told slot %u its command #%u waits", static_cast<unsigned>(slot),
                requestId);
    else if (result == KA::kResultConsumedHeld)
        UE_LOGI("kerfur_command_answer: HOST told slot %u its command #%u took the disc it carried",
                static_cast<unsigned>(slot), requestId);
    else
        UE_LOGI("kerfur_command_answer: HOST told slot %u its command #%u is refused: %s", static_cast<unsigned>(slot),
                requestId, ReasonName(reason));
}

}  // namespace

uint32_t NextRequestId() { return ++g_nextId; }

void HoldSent(uint32_t requestId, std::optional<coop::kerfur_disc_carry::Pledge> pledge) {
    if (g_held.size() >= kHeld) g_held.erase(g_held.begin());
    g_held.push_back(Held{requestId, std::move(pledge)});
}

void SendRefused(coop::net::Session* session, uint8_t slot, uint32_t requestId, uint8_t reason) {
    if (reason > KA::kRefusalMax) return;
    Send(session, slot, requestId, KA::kResultRefused, reason);
}

void SendWaits(coop::net::Session* session, uint8_t slot, uint32_t requestId) {
    Send(session, slot, requestId, KA::kResultWaits, 0);
}

void SendConsumed(coop::net::Session* session, uint8_t slot, uint32_t requestId) {
    Send(session, slot, requestId, KA::kResultConsumedHeld, 0);
}

void ShowLocal(uint8_t reason) {
    const char* line = reason <= KA::kRefusalMax ? RefusalLine(reason) : nullptr;
    std::wstring text;
    if (!line || !ShowLine(line, text)) {
        ++g_counts.lost;
        UE_LOGW("kerfur_command_answer: the local refusal (reason %u) did not show here", static_cast<unsigned>(reason));
        return;
    }
    if (++g_counts.local <= kLinesPerKind)
        UE_LOGI("kerfur_command_answer: showed a local refusal (%s): \"%ls\"", ReasonName(reason), text.c_str());
}

void OnAnswer(const KerfurCommandAnswerPayload& p) {
    const auto held = std::find_if(g_held.begin(), g_held.end(), [&](const Held& h) { return h.id == p.requestId; });
    const bool consumedHeld = p.result == KA::kResultConsumedHeld && p.reason == 0;
    const char* line = LineFor(p.result, p.reason);
    if (held == g_held.end() || (!line && !consumedHeld)) {
        ++g_counts.foreign;
        UE_LOGW("kerfur_command_answer: the host's answer for command #%u (result %u, reason %u) names no command sent "
                "here, or a value this build does not send -- dropped", p.requestId, static_cast<unsigned>(p.result),
                static_cast<unsigned>(p.reason));
        return;
    }
    if (consumedHeld) {
        // The host's verb took the disc this command carried: the pledge ends here, and the disc is destroyed on this
        // machine only if it is still the one pledged. A command that carried none names no disc to destroy.
        const std::optional<coop::kerfur_disc_carry::Pledge> pledge = held->pledge;
        g_held.erase(held);
        if (pledge && coop::kerfur_disc_carry::OnConsumed(*pledge)) {
            ++g_counts.consumed;
        } else {
            ++g_counts.consumeMiss;
            if (!pledge)
                UE_LOGW("kerfur_command_answer: the host's verb took a disc for command #%u, which pledged none here -- "
                        "consume-miss", p.requestId);
        }
        return;
    }
    // A wait is not the end of its command: the id stays, and a later refusal of it is shown.
    if (p.result == KA::kResultRefused) g_held.erase(held);
    std::wstring text;
    if (!ShowLine(line, text)) {
        ++g_counts.lost;
        UE_LOGW("kerfur_command_answer: the host's answer for command #%u did not show here", p.requestId);
        return;
    }
    const uint64_t n = p.result == KA::kResultWaits ? ++g_counts.waitsShown : ++g_counts.refusedShown;
    if (n <= kLinesPerKind)
        UE_LOGI("kerfur_command_answer: showed %s for command #%u: \"%ls\"",
                p.result == KA::kResultWaits ? "the wait" : "the refusal", p.requestId, text.c_str());
}

void OnDisconnect() {
    const Counts& c = g_counts;
    if (c.refusedSent || c.waitsSent || c.refusedShown || c.waitsShown || c.local || c.foreign || c.lost ||
        c.consumedSent || c.consumed || c.consumeMiss)
        UE_LOGI("kerfur_command_answer: session end -- sent refusals=%llu waits=%llu consumed=%llu, showed refusals=%llu "
                "waits=%llu local=%llu, foreign=%llu lost=%llu, consumed here=%llu consume-miss=%llu", c.refusedSent,
                c.waitsSent, c.consumedSent, c.refusedShown, c.waitsShown, c.local, c.foreign, c.lost, c.consumed,
                c.consumeMiss);
    g_counts = Counts{};
    g_nextId = 0;
    g_held.clear();
}

}  // namespace coop::kerfur_command_answer
