// coop/creatures/kerfur_command_answer.cpp -- see coop/creatures/kerfur_command_answer.h.

#include "coop/creatures/kerfur_command_answer.h"

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
#include <string>
#include <vector>

namespace coop::kerfur_command_answer {
namespace {

namespace KA = coop::net::kerfur_command_answer;
using coop::net::KerfurCommandAnswerPayload;

// The ids sent and not yet answered. A command the host ran is never answered, so its id stays until newer ones push
// it out: only a refusal or a wait names an id, and a refused command is the latest the player picked.
constexpr size_t kHeld = 16;
// A line per kind for the first few, then the counts at the session's end say the rest.
constexpr uint64_t kLinesPerKind = 20;
// The game's own refusal hint type ("You need to hold a proper floppy disc", "This kerfur is active").
constexpr uint8_t kRefusalHintType = 1;

uint32_t g_nextId = 0;
std::vector<uint32_t> g_held;

struct Counts {
    uint64_t refusedSent = 0, waitsSent = 0, refusedShown = 0, waitsShown = 0, foreign = 0, lost = 0;
};
Counts g_counts;

const char* ReasonName(uint8_t reason) {
    switch (static_cast<KA::KerfurRefusal>(reason)) {
    case KA::KerfurRefusal::Busy:      return "busy";
    case KA::KerfurRefusal::Stale:     return "stale";
    case KA::KerfurRefusal::KillMode:  return "kill mode";
    case KA::KerfurRefusal::QueueFull: return "queue full";
    case KA::KerfurRefusal::Unknown:   return "unknown";
    }
    return "?";
}

// The player's own line for an answer, in its language, or null for a value no build of this protocol sends.
const char* LineFor(const KerfurCommandAnswerPayload& p) {
    if (p.result == KA::kResultWaits) return l10n::T("The host will run that kerfur command in a moment.");
    if (p.result != KA::kResultRefused) return nullptr;
    switch (static_cast<KA::KerfurRefusal>(p.reason)) {
    case KA::KerfurRefusal::Busy:      return l10n::T("That kerfur is busy with a task.");
    case KA::KerfurRefusal::Stale:     return l10n::T("That kerfur is no longer there.");
    case KA::KerfurRefusal::KillMode:  return l10n::T("That kerfur will not take orders.");
    case KA::KerfurRefusal::QueueFull: return l10n::T("Too many kerfur commands at once, try again.");
    case KA::KerfurRefusal::Unknown:   return l10n::T("The kerfur could not take that command.");
    }
    return nullptr;
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
    const uint64_t n = result == KA::kResultWaits ? ++g_counts.waitsSent : ++g_counts.refusedSent;
    if (n > kLinesPerKind) return;
    if (result == KA::kResultWaits)
        UE_LOGI("kerfur_command_answer: HOST told slot %u its command #%u waits", static_cast<unsigned>(slot),
                requestId);
    else
        UE_LOGI("kerfur_command_answer: HOST told slot %u its command #%u is refused: %s", static_cast<unsigned>(slot),
                requestId, ReasonName(reason));
}

}  // namespace

uint32_t NextRequestId() {
    const uint32_t id = ++g_nextId;
    if (g_held.size() >= kHeld) g_held.erase(g_held.begin());
    g_held.push_back(id);
    return id;
}

void SendRefused(coop::net::Session* session, uint8_t slot, uint32_t requestId, uint8_t reason) {
    if (reason > KA::kRefusalMax) return;
    Send(session, slot, requestId, KA::kResultRefused, reason);
}

void SendWaits(coop::net::Session* session, uint8_t slot, uint32_t requestId) {
    Send(session, slot, requestId, KA::kResultWaits, 0);
}

void OnAnswer(const KerfurCommandAnswerPayload& p) {
    const auto held = std::find(g_held.begin(), g_held.end(), p.requestId);
    const char* line = LineFor(p);
    if (held == g_held.end() || !line) {
        ++g_counts.foreign;
        UE_LOGW("kerfur_command_answer: the host's answer for command #%u (result %u, reason %u) names no command sent "
                "here, or a value this build does not send -- dropped", p.requestId, static_cast<unsigned>(p.result),
                static_cast<unsigned>(p.reason));
        return;
    }
    // A wait is not the end of its command: the id stays, and a later refusal of it is shown.
    if (p.result == KA::kResultRefused) g_held.erase(held);
    std::wstring text;
    void* local = coop::players::Registry::Get().Local();
    if (!local || !coop::text::FromUtf8Strict(line, std::strlen(line), &text) ||
        !ue_wrap::votv_lib::AddHint(text, kRefusalHintType, local)) {
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
    if (c.refusedSent || c.waitsSent || c.refusedShown || c.waitsShown || c.foreign || c.lost)
        UE_LOGI("kerfur_command_answer: session end -- sent refusals=%llu waits=%llu, showed refusals=%llu waits=%llu, "
                "foreign=%llu lost=%llu", c.refusedSent, c.waitsSent, c.refusedShown, c.waitsShown, c.foreign, c.lost);
    g_counts = Counts{};
    g_nextId = 0;
    g_held.clear();
}

}  // namespace coop::kerfur_command_answer
