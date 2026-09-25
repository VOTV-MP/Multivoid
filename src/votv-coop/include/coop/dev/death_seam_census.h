// coop/dev/death_seam_census.h -- [dev] every element end the death seam announces, one line each: the
// element's type and id, whether it mirrors another peer's entity, a destroy or a stream-out, and the
// actor's class, read before the drain hands it on. The lanes' own death detections log their eids, so a
// run pairs the two: which ends a lane saw, how much later, and which it never saw. Armed by
// death_seam_census=1; read-only. Lines tagged [DEATH].
#pragma once

namespace coop::dev::death_seam_census {

// Subscribes to every element type once the seam is installed; a latched flag read when off. Game thread.
void Tick();

}  // namespace coop::dev::death_seam_census
