// coop/dev/host_settle_drill.h -- [dev] a join that meets the host's world start-up waits for it.
// The host judges (row host_settle_drill, env VOTVCOOP_HOST_SETTLE_DRILL).
//
// It reads the REAL conditions at the join's capture (host_world_settle::TrueSettledNow, which the
// red row never overrides), so a capture that ran before the world settled FAILs by name. It also
// requires that the window was exercised: at least one join request must have waited in the hold
// because the world had not settled, else the verdict is INCONCLUSIVE, never PASS. One more
// reading: births the watcher's queue cap dropped. The verdict prints once a slot is world-ready and
// the capture was judged.
// Lines tagged [HOST-SETTLE-DRILL]. Game thread.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::host_settle_drill {

bool IsEnabled();

// A join request was held in the transfer's wait. Counted only when the world had a gamemode and had
// not settled. A no-op when off.
void NoteHold();

// The join's live capture is starting now. Judges the real conditions. A no-op when off.
void NoteCapture();

// The verdict pass (a single bool read when off). Host only.
void Tick(coop::net::Session& session);

}  // namespace coop::dev::host_settle_drill
