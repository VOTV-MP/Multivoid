// coop/dev/badsun_drill_shared.h -- what the Bad Sun drill's files share: the arm the config names, the one verdict
// line, the join's end, and each arm file's entry points. Dev only; game thread. A private header of
// coop/dev/badsun_drill*.cpp (src tree, not include/).

#pragma once

#include <chrono>
#include <cstdarg>

namespace coop::net { class Session; }

namespace coop::dev::badsun_drill::detail {

using Steady = std::chrono::steady_clock;

// The arms. Each is judged on both peers once a client's join is over.
enum class Arm {
    Off,
    // clock, on the rig's save: a client's own loaded copy goes active at 07:00 by its own clock. The host parks the
    // clock at 05:00 at hosting and sets 06:30 after the join; each peer samples the count (1) and the copy's two tests
    // (widget set; in activeEvents_senders) from hour 6. Inactive until 07:00, active by both within 15 s after.
    // INCONCLUSIVE when the save's copy was active at hosting or a peer did not see its clock cross 07:00.
    ClockArm,
    // midnight, fresh world, rollover_watch=1 on the host: the host writes game mode 7 (put back after), sets 23:58
    // and waits for the day edge. Host PASS: the day cycle's in-graph Spawn Bad Sun counted by the watch and
    // gamemode.badSun present. Client PASS: exactly one inactive copy within 10 s of the host's day number moving.
    // Until its verdict (6 to 190 s) the host's GameInstance holds game mode 7, so a save written in that window
    // carries it; the mode is put back on the verdict and on the session's end.
    Midnight,
    // spawn, fresh: the host sets 08:00 and calls Spawn Bad Sun. Client PASS: exactly one copy, active, and the
    // lane's own spawn count moved.
    Spawn,
    // cheat, fresh: the client calls its own Spawn Bad Sun at 08:00 by the reflected verb; the cheat menu's own handler
    // needs hasWeapon (Sandbox or flying) and a New Game is Story, so that route stays unproven. Client PASS: no copy
    // 10 s later and the lane's refusal count moved. The host sets 08:00 and judges nothing: a client's own spawn
    // never reaches it.
    Cheat,
    // join, rig save, a rejoin on the client's first verdict: the host sets 08:00. Joiner PASS: the seed's apply
    // counted one copy (the blob's) and spawned none, and that copy goes active.
    Join,
    // end, rig save: the host sets 08:00, waits for its widget's alpha to read 1.0 for 15 s (both copies' ramps then
    // clamp, so their fades are the same length), sets 18:59. Host PASS: its own remove() ran and the lane sent absent.
    // Client PASS: absent received while its copy was active, its own remove() after, and the sky's sunIntensityMult
    // back at 1. FAIL when the absent apply neither left an active copy nor destroyed an inactive one.
    // INCONCLUSIVE when the client's own remove() ran first.
    End,
    // absent_stale, fresh, a rejoin on the life-1 client's ready line: the host sets 02:00 and spawns one inactive
    // copy; when a rejoining slot's world is taken and not yet ready it destroys that copy, so the blob holds one the
    // host no longer has. Joiner PASS: the seed applied absent, the lane's destroyed count 1, no copy 10 s later.
    // INCONCLUSIVE when the seed said present, the blob carried no copy, or its copy was active. FAIL when the seed
    // applied absent to a live copy and destroyed none. The host's session ends when the life-1 client leaves, so its
    // drill re-arms and resumes on the copy this process made.
    AbsentStale,
};

// The configured arm, read once.
Arm ArmOf();

bool IsHost(coop::net::Session* s);
const char* RoleName(bool host);

// The one verdict line: the drill tag, the arm, the role, the word, then the reason; PASS at info, the rest at warning.
void VerdictV(const char* arm, bool host, const char* word, const char* fmt, va_list ap);

// Whether the join is over on this peer. The host: a client's slot is world-ready and its snapshot bracket closed
// (that slot is kept in `slot`). A client: its world-ready announced and its join phases over.
bool JoinOver(coop::net::Session* s, bool host, int& slot);

// The `clock` arm (coop/dev/badsun_drill_clock.cpp): one tick of it, and its state back to the first phase.
void ClockTick(coop::net::Session* s, bool host);
void ClockReset();

// The six lane arms (coop/dev/badsun_drill_lane.cpp): one tick of the configured one, and its state back to the
// first phase (a game mode the midnight arm wrote is put back).
void LaneTick(coop::net::Session* s, bool host);
void LaneReset();

}  // namespace coop::dev::badsun_drill::detail
