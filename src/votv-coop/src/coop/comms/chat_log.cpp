// coop/comms/chat_log.cpp -- see coop/comms/chat_log.h.

#include "coop/comms/chat_log.h"

#include "coop/comms/chat_feed.h"

#include "ue_wrap/core/log.h"

#include <deque>

namespace coop::chat_log {
namespace {

// The record is capped at the same figure the feed retains, because the seed is what
// the joiner's retained tier becomes and holding rows we would never deliver is dead
// memory. ~28 KB of store; the seed it implies is ~21 KB against the 8192-message
// reliable inbox cap, i.e. 1.2 %, and it rides world-ready so it is not competing with
// the save transfer's window.
std::deque<Row> g_rows;
uint32_t        g_nextSeq = 1;   // 0 is reserved: it means "no line"

}  // namespace

uint32_t Append(uint8_t slot, const std::string& nick, uint32_t nickArgb,
                const std::string& text) {
    Row r;
    r.lineSeq  = g_nextSeq++;
    r.slot     = slot;
    r.nickArgb = nickArgb;
    r.nick     = nick;
    r.text     = text;
    g_rows.push_back(std::move(r));
    while (g_rows.size() > static_cast<size_t>(coop::chat_feed::kMaxRetained))
        g_rows.pop_front();
    return g_rows.back().lineSeq;
}

void ForEach(const std::function<void(const Row&)>& fn) {
    for (const Row& r : g_rows) fn(r);
}

int Count() { return static_cast<int>(g_rows.size()); }

void Reset() {
    if (!g_rows.empty())
        UE_LOGI("chat_log: reset -- dropping %zu committed line(s)", g_rows.size());
    g_rows.clear();
    // g_nextSeq deliberately keeps counting. A restarted lobby that reused seq 1 would
    // hand a client that never reset its own applied range a "duplicate" it has
    // already seen, and the row would be silently dropped.
    g_nextSeq = g_nextSeq < 1 ? 1 : g_nextSeq;
}

}  // namespace coop::chat_log
