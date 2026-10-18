// coop/net/fake_link_steps.h -- the fake-link knobs a run can change while it is running.
//
// The transport simulates a bad link with process-global settings (lag, loss, jitter, a send
// policer). The rows `net.fakelag_ms`, `net.fakeloss_*_pct`, `net.fakejitter_ms` and
// `net.fakelink_kbs` write them at every session start; `net.fakelink_steps` changes them
// later, at a stated time after an event of the transfer itself, so a drill can degrade a link at
// the moment the join starts instead of at a wall-clock guess. MTA's `sfakelag` is the precedent
// for gating loss, latency, variance and a bandwidth cap as one instrument. Each peer runs only
// the steps of its own config, against its own origins, the first time each happens in a session.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net::fake_link_steps {

// A schedule is `;`-separated steps `<origin>+<sec>:<key>=<value>[&<key>=<value>]` (the keys of a
// step join with `&` because the rig's `--config` splits its specs on commas): origin `begin`
// (the world blob's announce: the joiner's Begin, the host's first chunk pass) or `wr` (the
// joiner's world-ready: the joiner's announce, the host's receipt of it), keys `lag`, `jitter`,
// `loss_send`, `loss_recv`, `kbs`. A schedule that does not parse is ignored whole.
enum class Origin : uint8_t { Begin = 0, WorldReady = 1 };
inline constexpr int kOriginCount = 2;

// The ranges of the rows beside `net.fakelink_kbs`, which a step's values obey as well.
inline constexpr int kLagMaxMs = 2000;
inline constexpr int kJitterMaxMs = 1000;
inline constexpr int kPctMax = 100;
inline constexpr int kKbsMax = 1048576;
inline constexpr uint32_t kMaxAfterSec = 86400;

struct Step {
    Origin origin = Origin::Begin;
    uint32_t afterSec = 0;
    // -1 = the key is not in this step.
    int32_t lagMs = -1;
    int32_t jitterMs = -1;
    int32_t lossSendPct = -1;
    int32_t lossRecvPct = -1;
    int32_t kbs = -1;
};

inline constexpr int kMaxSteps = 16;
struct Schedule {
    Step steps[kMaxSteps];
    int count = 0;
};

// The pure parser. An empty `text` is a valid, empty schedule. On failure `out` is left empty
// and `badStep` holds the text of the step that did not parse.
bool ParseSteps(const std::string& text, Schedule& out, std::string& badStep);

// The global writers, shared by the session-start rows and the steps: one writer per quantity.
// 0 turns the simulation of that quantity off.
void WriteLagMs(int ms);
void WriteLossSendPct(int pct);
void WriteLossRecvPct(int pct);
void WriteJitterMs(int ms);
void WriteLinkKbs(int kbs);

// Threading: Arm runs on the thread that starts the session, NoteOrigin on whichever thread
// reaches the origin (the net thread for the joiner's Begin), Tick on the game thread. The
// schedule is written before it is published and read-only after, the origins are atomics. Arm
// runs again on every start and rewrites the schedule, so what keeps the game thread out of it is
// the order inside Session::Start: Arm precedes `running_`, and net_pump::Tick returns before
// reaching this module while the session is not running.
//
// Session start: writes every fake-link global back to its row's value (0 where unset), so a step
// of a previous session in this process never opens this one; then reads `net.fakelink_steps`,
// forgets the previous session's origins and applied steps, and logs.
void Arm();
// An origin happened; only the first one per session is kept. Any thread.
void NoteOrigin(Origin origin);
// Game thread, every tick: applies the steps that came due. One load when nothing is armed, and no
// clock read while no origin has happened.
void Tick();

// The parser's cases, run once per process at the first Session::Start; logs one PASS or FAIL line.
bool RunSelftest();

}  // namespace coop::net::fake_link_steps
