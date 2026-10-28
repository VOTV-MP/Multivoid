// coop/dev/director/container_take_judge.h -- the two-peer take race's cross-peer judge: the counts one peer
// writes beside the GO file, the other's read of them, and the verdict both peers print from the same four counts and
// the same act outcome. Dev only (RULE 3).

#pragma once

#include <cstdint>
#include <string>

namespace coop::director {

// System wall-clock (shared across peers on one box) as Unix ms.
uint64_t NowUnixMs();
// A leaf file beside the orchestrator's GO file; empty when either name is empty.
std::string BesideGo(const std::string& goFile, const std::string& leaf);
// Writes `text` to a temp file and renames it over `file`, so a reader never sees half of it.
bool WriteSentinel(const std::string& file, const std::string& text);

// How the client's act ended, as its own probe read it; idle for a peer that runs no act (the host takes natively).
enum ActCode : int32_t { kActIdle = 0, kActDone, kActAborted, kActRefused, kActNone, kActPending };

// What one peer measured, handed to the other through a file beside the GO file (the target sentinel's shape): both
// peers then judge from the same four counts and the same act outcome.
struct PeerCounts { int32_t contBefore = -1, contAfter = -1, pockBefore = -1, pockAfter = -1, act = kActIdle; };
std::string CountsFile(const std::string& goFile, const std::string& role);
bool WriteCounts(const std::string& file, const PeerCounts& c);
// The other peer's counts, polled until the file reads whole.
bool WaitForCounts(const std::string& file, uint64_t timeoutMs, PeerCounts& out);

// The race's judged result: `total` is the copies of X alive after the take, the container's record still there
// (1 - its loss) plus each pocket's gain. 1 correct, 2 a duplicate, 0 vanished. A client act that did not replay, never
// ran or is still open is never read as a pass.
struct Judged { int32_t total; const char* verdict; const char* why; };
Judged Judge(const PeerCounts& host, const PeerCounts& client);

}  // namespace coop::director
