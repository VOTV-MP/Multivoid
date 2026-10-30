// coop/dev/director/aimed_grab.h -- the director's grab of a loose prop, as a player's use key takes it. The camera
// turns through a fan round the prop until the player's own interaction trace strikes it (the trace runs on the
// player's tick, so each pose is held a few ticks), then the use key's grab chain runs in its order -- the prop's
// playerGrabbed_pre, the player's useAction, the prop's playerGrabbed -- and the grab is verified on the player's
// grabbing actor. DEV-ONLY, like the rest of the director; game thread.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"

namespace coop::director {

// The player's physics-grabbed actor, or null.
void* Grabbing(void* player);

// A reflected call of `fnName` on `obj` with the one parameter `player`, or with none; false when the function
// does not resolve or the call did not run.
bool CallWithPlayer(void* obj, const wchar_t* fnName, void* player);
bool CallOnPlayer(void* player, const wchar_t* fnName);

enum class GrabState { Working, Grabbed, Failed };

// One grab, ticked once per game-thread tick from a drill's step until it says Grabbed or Failed. The player must
// already stand within the use key's reach of the prop (a director walk takes it there). Both are held by slot and
// serial, so one that dies between ticks fails the grab rather than being read.
class AimedGrab {
public:
    AimedGrab(void* player, void* prop) { player_.Set(player); prop_.Set(prop); }
    GrabState Tick();
    // Hold at the aim: once the trace takes the prop, Tick keeps answering Working without running the chain
    // until Fire. A race aims both peers first and fires them at one instant.
    void HoldAtAim() { holdAtAim_ = true; }
    void Fire() { holdAtAim_ = false; }
    bool Aimed() const { return aimed_; }
    // The trace left the prop while held: aim again from the fan's first pose.
    void Reaim() { aimed_ = false; poses_ = 0; ticks_ = 0; }
    const char* Why() const { return why_; }  // the failure, once Failed
    int AimPoses() const { return poses_; }   // the fan poses tried before the trace took the prop
    // Whether each call of the grab chain ran: the prop's playerGrabbed_pre, the player's useAction, the prop's
    // playerGrabbed. All false until the aim is taken.
    bool ChainPre() const { return chainPre_; }
    bool ChainUse() const { return chainUse_; }
    bool ChainPost() const { return chainPost_; }
private:
    ue_wrap::CachedObjRef player_;
    ue_wrap::CachedObjRef prop_;
    int ticks_ = 0;
    int poses_ = 0;
    bool aimed_ = false;
    int grabTicks_ = 0;
    bool chainPre_ = false, chainUse_ = false, chainPost_ = false;
    bool holdAtAim_ = false;
    const char* why_ = "";
};

}  // namespace coop::director
