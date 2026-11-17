// coop/dev/cheat_drill_shared.h -- what the cheat drill's two files share: coop/dev/cheat_drill (the panes and arms A..F,
// the ticks) and coop/dev/cheat_drill_arms (the hostile arms G..L). The drill's state, the verdict calls, and the one
// way a client sends a request and waits for its answer. Private to the two; nothing else includes it.

#pragma once

#include "coop/cheats/cheat_request.h"
#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::cheat_drill::detail {

constexpr const char* kTag = "[CHEAT-DRILL]";
constexpr uint64_t kSyncBoundMs = 180000;   // a peer waiting on the other's line
constexpr uint64_t kEffectBoundMs = 30000;  // a cheat's effect reaching a count

struct State {
    int step = 0;
    uint64_t stepAt = 0;
    bool done = false;
    // the pane opener
    size_t paneIdx = 0;
    bool paneAsked = false;
    uint64_t askedAt = 0;
    bool menuOpen = false;
    // baselines of the arm in flight
    int crowbars = 0;
    int32_t points = 0;
    unsigned fires = 0;
    unsigned replays = 0;
    float liftStartZ = 0.f, liftedZ = 0.f;   // arm L: the body's height before the lift, and read back after it
    uint32_t answered[4] = {};
    bool sent = false;
    int resultIdx = 0;
    // the hostile arms (G..L): which, and where in it
    int arm = 0;
    int sub = 0;
    int stage = 0;
};
extern State g;

// The client's results, in the order they arrived, with the status line as it stood when each arrived.
struct Result { uint8_t op; uint8_t reason; uint16_t done; uint32_t id; bool pending; uint8_t statusReason; };
extern Result g_results[16];
extern int g_nResults;

uint64_t Now();
bool Timed(uint64_t bound);   // the step has lasted longer than `bound`
void Next();
bool Seen(const char* line);  // a drill line from the other peer
int Crowbars();
bool Points(int32_t* out);

// The run's ending. A FAIL is decided by an event and said at once; a time bound only says INVALID.
void Verdict(const char* role, bool pass, const std::string& why);
void Invalid(const char* role, const std::string& why);
void ClientFail(const char* arm, const std::string& why);
void ClientInvalid(const char* arm, const std::string& why);

// Send one request through `send` and wait for its answer: 1 when answered, `r` the answer; 0 while waiting.
template <class SendFn>
int Ask(SendFn send, Result* r, const char* arm) {
    if (!g.sent) {
        g.resultIdx = g_nResults;
        send();
        g.sent = true;
        g.stepAt = Now();
        return 0;
    }
    if (g_nResults > g.resultIdx) { *r = g_results[g.resultIdx]; return 1; }
    if (Timed(kEffectBoundMs)) ClientInvalid(arm, "the host never answered the request");
    return 0;
}

// The hostile arms G..L, one tick each. True once every arm has passed on this peer.
bool HostArms(coop::net::Session& s);
bool ClientArms(coop::net::Session& s);

}  // namespace coop::dev::cheat_drill::detail
