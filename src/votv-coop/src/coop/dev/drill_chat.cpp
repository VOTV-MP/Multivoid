// coop/dev/drill_chat.cpp -- see coop/dev/drill_chat.h.

#include "coop/dev/drill_chat.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_rate.h"
#include "coop/comms/chat_sync.h"

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <deque>
#include <string_view>
#include <utility>

namespace coop::dev::drill_chat {
namespace {

// A line the host refused or lost leaves no answer, so the echo is waited for this long and then the line is sent again,
// up to kMaxSends times in all; past that it is dropped with one WARN.
constexpr uint64_t kEchoBoundMs = 5000;
constexpr int      kMaxSends = 3;

struct Item {
    std::string line;
    int         sends = 0;
};

// The one line in flight: sent, its echo not yet seen. `keyAfter` is the newest feed key at the send, so the echo is a
// row newer than it; `shaped` is the text the host commits.
struct Flight {
    bool        on = false;
    Item        item;
    std::string shaped;
    uint64_t    sentMs = 0;
    uint64_t    keyAfter = 0;
};

std::deque<Item>            g_queue;
Flight                      g_flight;
coop::chat_rate::TalkBudget g_budget;  // this machine's copy of the budget the host keeps for it

uint64_t NewestKey() {
    uint64_t newest = 0;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (r.key > newest) newest = r.key;
    });
    return newest;
}

// The host's commit of the line in flight, as this machine's feed shows it: a row newer than the send whose text is
// the shaped line. The feed's row carries no slot, so the text decides; the drills' lines name their role, and no two
// peers say the same one. The host's own lines are committed through the same feed, so one path serves both roles.
bool EchoSeen() {
    bool seen = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (r.key > g_flight.keyAfter && r.line == g_flight.shaped) seen = true;
    });
    return seen;
}

}  // namespace

void Say(const std::string& line) { g_queue.push_back(Item{line, 0}); }

// The host judges the interval at receive time, and a retransmit can bring two lines together. So a line leaves only
// after the one before it was echoed, and the budget's last talk is the echo's time: the host received it no later, so
// the next line, sent no earlier than echo + 0.66 s, is received after host-receive + 0.66 s. While a line waits, the
// feed is walked on the drill poll (PollDue) and once more when the bound passes; the bound itself counts milliseconds.
void Pump(uint64_t nowMs) {
    if (g_flight.on) {
        const bool waiting = nowMs - g_flight.sentMs < kEchoBoundMs;
        if (waiting && !PollDue()) return;
        if (EchoSeen()) {
            g_budget.lastTalkMs = nowMs;
            g_flight.on = false;
        } else if (waiting) {
            return;
        } else if (g_flight.item.sends >= kMaxSends) {
            UE_LOGW("drill_chat: no echo for \"%s\" after %d sends -- dropped", g_flight.item.line.c_str(),
                    g_flight.item.sends);
            g_flight.on = false;
        } else {
            g_queue.push_front(g_flight.item);  // the host refused or lost it: sent again
            g_flight.on = false;
        }
    }
    if (g_queue.empty() || !coop::chat_rate::Allows(g_budget, nowMs)) return;
    Item item = std::move(g_queue.front());
    g_queue.pop_front();
    std::string shaped;
    if (!coop::chat_sync::ShapeChatLine(item.line, &shaped)) {
        UE_LOGW("drill_chat: \"%s\" shapes to nothing, the host would commit no row -- dropped", item.line.c_str());
        return;
    }
    ++item.sends;
    g_flight.keyAfter = NewestKey();
    g_flight.shaped = std::move(shaped);
    g_flight.sentMs = nowMs;
    g_flight.item = item;
    g_flight.on = true;
    coop::chat_sync::QueueSend(item.line);
    coop::chat_rate::Attempt(g_budget, nowMs);  // the send is recorded as the host will judge it
}

void Reset() {
    g_queue.clear();
    g_flight = Flight{};
    g_budget = coop::chat_rate::TalkBudget{};
}

bool ChatHasLive(const std::string& needle) {
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (!r.retained && r.line.find(needle) != std::string_view::npos) found = true;
    });
    return found;
}

bool LiveRestOf(const std::string& prefix, std::string* rest) {
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (r.retained || r.line.compare(0, prefix.size(), prefix) != 0) return;
        std::string_view tail = r.line.substr(prefix.size());
        while (!tail.empty() && tail.back() == ' ') tail.remove_suffix(1);
        if (tail.empty()) return;
        rest->assign(tail.begin(), tail.end());
        found = true;
    });
    return found;
}

void LiveRestsOf(const std::string& prefix, std::vector<std::string>* rests) {
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (r.retained || r.line.compare(0, prefix.size(), prefix) != 0) return;
        std::string_view tail = r.line.substr(prefix.size());
        while (!tail.empty() && tail.back() == ' ') tail.remove_suffix(1);
        if (!tail.empty()) rests->emplace_back(tail);
    });
}

bool PollDue() {
    static uint32_t sTicks = 0;
    return sTicks++ % 15 == 0;
}

}  // namespace coop::dev::drill_chat
