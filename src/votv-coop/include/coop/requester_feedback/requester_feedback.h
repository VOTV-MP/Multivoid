// coop/requester_feedback/requester_feedback.h -- what a verb the host runs for a client shows that client, not the
// host: the hint it raises, the glossary entry it adds, the 2D sounds it plays.
//
// A verb a client asked the host to run (a desk press, a kerfur command) runs on the host's copy, and the game's own
// feedback for it is made on the host's machine: lib_C::addHint writes the host's HUD, lib_C::addGloss the host's
// profile (getMainSave, lib.cpp:3472-3477, :4143), PlaySound2D plays at one level for the host's player alone. While
// a Scope is open the host refuses each at the seam it passes (the script gate's watch on addHint and addGloss, a
// pre hook on PlaySound2D armed only inside a scope, ue_wrap/core/ufunction_hook) and sends it to the requester as
// RequesterFeedback, whose machine makes the call for itself, its own settings deciding (a hint's enablePopups).
// Source sends a one-player sound to a single recipient (basecombatweapon_shared.cpp:1920-1930), MTA's
// playSoundFrontEnd goes to one player (CStaticFunctionDefinitions.cpp:9030); the shape is theirs, the seams ours.

#pragma once

// The scope is opened by the code that runs the verb on the requester's behalf and never for the host's own verb
// (slot 0): the host's own press runs the same body and keeps its hints, glosses and sounds. A body-scoped form
// (ForwardInBody) carries a gloss that completes long after its press, in one script body's span (the refiner's
// decode, comp_sync). Game thread; the state is plain globals read by the hooks on that thread.

#include <cstdint>

namespace coop::net {
class Session;
struct RequesterFeedbackPayload;
}  // namespace coop::net

namespace coop::requester_feedback {

// Watch addHint and addGloss and install the PlaySound2D pre hook disarmed. Idempotent; retried until each settles.
// Game thread.
void Install(coop::net::Session* session);

// HOST: every seam stands, so a verb run for a client can run here without its feedback landing on the host.
// Refused names a seam this process will never have. A caller that opens a Scope holds its verb while !Ready().
// Game thread.
bool Ready();
bool Refused();

// HOST: while one lives, the verb running is `slot`'s, request number `seq` (0 for a verb with none): its hints, its
// glosses and its 2D sounds go there. Restores the scope it found, so a nested one cannot close an outer: the
// innermost holds. A scope for slot 0 (the host's own verb) or a slot no peer holds opens nothing: counted and said
// once. Game thread.
class Scope {
  public:
    Scope(uint8_t slot, uint32_t seq);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    uint8_t  outerSlot_;
    uint32_t outerSeq_;
    bool     outerOpen_;
    bool     active_;
};

// HOST: the slot and request number of the scope open, 0xFF and 0 outside one. Game thread.
uint8_t  CurrentSlot();
uint32_t CurrentSeq();

// HOST: while `bodyFn`'s body runs (script_gate::IsBodyActive), a gloss made in it is `slot`'s, as a scope's is: the
// refiner's decode a client began, whose completion runs in a tick of the desk's long after the press (comp_sync).
// Its hints and sounds are not forwarded (only a Scope's are). A slot of 0xFF ends it. Game thread.
void ForwardInBody(void* bodyFn, uint8_t slot, uint32_t seq);

// CLIENT: the host's feedback for a verb this machine asked for: made here, as the verb would have made it. Game
// thread.
void OnFeedback(const coop::net::RequesterFeedbackPayload& p);

// Session end: the counts are said and the scope is closed.
void OnDisconnect();

// This session's counts, for a drill: what this host routed to a requester and what this requester showed from the
// host's word; `lost`, what did not cross or apply. Game thread.
struct Counts {
    uint64_t hintsSent = 0, glossesSent = 0, soundsSent = 0;
    uint64_t hintsShown = 0, glossesMade = 0, soundsMade = 0;
    uint64_t lost = 0, badOps = 0, scopesRefused = 0;  // badOps: an unknown op or hint type received
};
Counts CountsNow();

}  // namespace coop::requester_feedback
