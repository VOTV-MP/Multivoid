// coop/props/glowstick_sync.h -- a glowstick stuck to a surface is stuck on every peer.
//
// A held glowstick (prop_glowstick_C) ticks a short ray from its tip; when the ray strikes, the holder drops it and
// its stickTo hangs it from a hook at the struck point by a physics constraint. A grab's prelude runs its
// releaseGlowstick, which breaks the constraint. Both run on the holder's game only, and the constraint is no field a
// prop's pose or record moves: another peer's copy stayed free, fell or stood where it was, and its pose then pulled
// the stuck copy off its point ("snaps back"). The stick is save state, though -- the glowstick's own getData carries
// `sticked`, the point, the frames and the stuck surface's transform -- so the prop record lane (coop/props/
// prop_save_data) carries it: a player's own stickTo or releaseGlowstick publishes the glowstick's record (a client's
// as an intent the host takes and re-publishes), and on every peer the record's landing, the glowstick's own loadData,
// is followed by the same constraint made or broken on that copy: releaseGlowstick, then the stick made from the
// landed point (stickTo's re-stick form, then hookDelay). The author's own copy keeps its constraint through the echo.
#pragma once

namespace coop::net { class Session; }

namespace coop::glowstick_sync {

// Caches the session and registers the three watches (stickTo, releaseGlowstick, loadData). Idempotent. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: publishes the record of each glowstick a player's own act stuck or freed since the last pass.
// Game thread.
void Tick();

// Session teardown: the pending publishes and the landing marks go.
void OnDisconnect();

}  // namespace coop::glowstick_sync
