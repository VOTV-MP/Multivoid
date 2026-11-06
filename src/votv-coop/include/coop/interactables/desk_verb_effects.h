// coop/interactables/desk_verb_effects.h -- what a desk press the host runs for a client makes for that client
// alone: the glossary entry its SAVE adds, the sounds its branches play, and what a decode its start began adds.
//
// The SAVE branch's lib_C::addGloss(name, 0, desk) writes getMainSave(), the save_main of the machine that runs
// it (lib.cpp:3472-3477, :4143), so run on the host for a client's press the entry would land in the host's
// profile. The branches' PlaySound2D clicks are 2D, heard at one level anywhere (analogDScreenTest.cpp:2611,
// :2696), so the host would hear a client's press and the client nothing, its own press refused. While a
// Replay is open the host refuses both, addGloss at the script-body gate and PlaySound2D at a pre hook armed
// only inside it (ue_wrap/core/ufunction_hook), and sends each to the presser, whose machine makes the call for
// itself. The scope is the replay's own rather than the gate's IsBodyActive(actionOptionIndex): the host's own
// press runs the same body and keeps its gloss and its clicks (drive_sync.cpp:108-132). A client's refiner
// decode completes long after its press, in calculate_comp's body: its gloss is forwarded on the gate's scope
// of that body (ForwardGlossesInBody), and its signals_processed point comp_sync sends as a stat (desk_verb_intent). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DeskVerbPayload;
}  // namespace coop::net

namespace coop::desk_verb_effects {

// Watch addGloss and install the PlaySound2D pre hook disarmed. Idempotent; retried until both settle. Game
// thread.
void Install(coop::net::Session* session);

// HOST: both seams stand, so a client's press can run here without its gloss or sounds landing on the host.
// Refused names a seam this process will never have. Game thread.
bool Ready();
bool Refused();

// HOST: while one lives, the press running is `slot`'s, press number `seq`: its gloss and its 2D sounds go
// there. Restores the scope it found, so a nested one cannot close an outer. Game thread.
class Replay {
  public:
    Replay(uint8_t slot, uint32_t seq);
    ~Replay();
    Replay(const Replay&) = delete;
    Replay& operator=(const Replay&) = delete;

  private:
    uint8_t  outerSlot_;
    uint32_t outerSeq_;
    bool     outerOpen_;
};

// HOST: the slot and press number of the replay running, 0xFF and 0 outside one. Game thread.
uint8_t  ReplaySlot();
uint32_t ReplaySeq();

// HOST: while `bodyFn`'s body runs (script_gate::IsBodyActive), a gloss made in it is `slot`'s, as a replayed
// press's is: the refiner's decode a client began, whose completion runs in a tick of the desk's long after
// the press (comp_sync). A slot of 0xFF ends it. Game thread.
void ForwardGlossesInBody(void* bodyFn, uint8_t slot, uint32_t seq);

// CLIENT: the host's gloss or sound for this machine's press (DeskVerb ops 2 and 3): made here, as
// the press would have made it. Game thread.
void OnEffect(const coop::net::DeskVerbPayload& p);

// Session end: the counts are said and the scope is closed.
void OnDisconnect();

// This session's counts, for a drill: the glosses and 2D sounds this host sent to their presser, and the
// ones this presser made from the host's word; `lost`, the ones that did not cross or apply. Game thread.
struct Counts {
    uint64_t glossesSent = 0, soundsSent = 0, glossesMade = 0, soundsMade = 0;
    uint64_t lost = 0;
};
Counts CountsNow();

}  // namespace coop::desk_verb_effects
