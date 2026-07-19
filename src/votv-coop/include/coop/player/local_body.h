// coop/player/local_body.h -- the LOCAL player's body skin (first-person immersion).
//
// The local pawn's visible body (torso/legs when looking down, the mirror
// reflection) must wear the SAME skin the player's puppet wears on other peers
// (user 2026-07-02: "клиент видит туловище dr kel -- ломает иммерсию"). This
// module is the single owner of the LOCAL skin choice:
//   - persisted in multivoid.ini player_skin= (next to player_guid; default
//     hl_einstein_v1sc for a new identity),
//   - announced on the wire by player_handshake (Join field + SkinChange),
//   - applied to the local pawn here (both body slots via client_model),
//   - re-captured/re-applied across level changes (new pawn generation), with a
//     1 Hz convergence check that detects + logs a game-side re-dress (so we
//     learn the exact seam if the game ever fights the swap).
//
// It also owns the PRISTINE kel mesh capture: the native body asset is latched
// from the local pawn BEFORE any swap, per pawn generation. Puppet spawns use
// NativeBodyMesh() as their kel baseline -- reading the LOCAL pawn's live mesh
// after a swap would contaminate every kel puppet with our custom skin.
//
// Threading: game thread owns the state; the F1 browser (render thread) enters
// via RequestSkin (posts the apply) and LocalSkinNameCopy (mutex snapshot).

#pragma once

#include <string>

namespace coop::net { class Session; }

namespace coop::local_body {

// Boot wiring (harness bring-up thread, before the net pump sends a Join):
// the persisted choice from multivoid.ini player_skin=.
void SetInitialSkin(const std::string& name);

// The local skin name (game thread -- the Join builder / puppet baseline path).
const std::string& LocalSkinName();

// Render-thread-safe snapshot for the F1 browser highlight.
std::string LocalSkinNameCopy();

// The pristine kel USkeletalMesh of the CURRENT pawn generation (captured
// before any swap). Null until the local pawn is dressed. Game thread.
void* NativeBodyMesh();

// F1 browser pick (render thread): validate, persist to ini, post the local
// apply to the game thread, announce SkinChange to the session (if connected).
void RequestSkin(const std::string& name);

// net_pump wiring (subsystems tick site). Install stores the session pointer
// (cheap per-tick re-store, the chat_sync shape); Tick applies/converges.
void Install(coop::net::Session* session);
void Tick();

}  // namespace coop::local_body
