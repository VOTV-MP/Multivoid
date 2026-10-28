// coop/dev/container_intent_drill_put.h -- what the container intent drill's put arms share between their three files,
// container_intent_drill_put.cpp (the client's machine, and what both halves read of an arm),
// container_intent_drill_put2.cpp (the client's interleaves and verdicts, and the reds) and
// container_intent_drill_put_host.cpp (the host). A private header of coop/dev/container_intent_drill_put*.cpp (src
// tree, not include/).

#pragma once

#include "container_intent_drill_detail.h"

#include "coop/props/container_act_request.h"

#include <cstddef>

namespace coop::dev::container_intent_drill::detail {

// The lines the two peers say to each other on drill chat.
inline constexpr const char* kPutFixtureLine = "ci-put host fixture ";   // + "pack=<eid> item=<eid|0> item2=<eid|0> count=<n>"
inline constexpr const char* kPutDoneLine    = "ci-put client done ";    // + "ok=<0 failed|1 passed|2 not measured> stray=<n|-1>"
// putjoin: the host tells client 1 to ask (go) and to release its parked replay (release); the joiner is told its
// fixture, and says the contents its seed showed and its own end.
inline constexpr const char* kPutGoLine       = "ci-put host go";
inline constexpr const char* kPutReleaseLine  = "ci-put host release";
inline constexpr const char* kPutJoinLine     = "ci-put host joiner ";    // + "pack=<eid> item=<eid> item2=0 count=<n>"
inline constexpr const char* kPutSeedLine     = "ci-put joiner seed ";    // + "n=<records the seeded mirror showed>"
inline constexpr const char* kPutJoinDoneLine = "ci-put joiner done ";    // + the done line's fields
inline constexpr const char* kPutNotMeasuredLine = "ci-put host notmeasured";   // putjoin could not run: say so, do not fail

// The container the nested and empty arms pocket: a gembox, whose volume fits the player's pocket.
inline constexpr const wchar_t* kBoxPath  = L"/Game/objects/prop_container_gembox.prop_container_gembox_C";
inline constexpr const wchar_t* kBoxClass = L"prop_container_gembox_C";

// The arms in which no put may be asked: puthidden (the game only clears the hover) and putnested (the seat refuses).
inline bool PutIsNoPutArm(Arm a) { return a == Arm::PutHidden || a == Arm::PutNested; }

// The arms whose target the host fills first: with one record (puthidden, puthover, putjoin, putowed) or to its volume
// (putfull*).
inline bool PutTargetFilled(Arm a) {
    return a == Arm::PutHidden || a == Arm::PutHover || a == Arm::PutFull || a == Arm::PutFullHand || a == Arm::PutJoin ||
           a == Arm::PutOwed;
}
inline bool PutFillsToBrim(Arm a) { return a == Arm::PutFull || a == Arm::PutFullHand; }

// The arms whose host spawns an item for the client to take (the hand arms take a drive from the player's own verb).
inline bool PutHasItem(Arm a) { return a != Arm::PutHand && a != Arm::PutHidden && a != Arm::PutFullHand; }

// How many records the target gains in the arm's green run: the puts that land.
inline int PutHostDelta(Arm a) {
    switch (a) {
    case Arm::PutRecord:
    case Arm::PutHand:
    case Arm::PutFood:
    case Arm::PutEmpty:
    case Arm::PutReserved:
    case Arm::PutDeadline: return 1;
    case Arm::PutJoin:     return 2;
    default:               return 0;
    }
}

// The arms whose host does something between the fixture and the client's end.
inline bool PutHostInterleaves(Arm a) {
    return a == Arm::PutReserved || a == Arm::PutDepart || a == Arm::PutDeadline || a == Arm::PutOwed || a == Arm::PutJoin;
}

// The class the arm puts (null for puthidden), which the stray count looks for.
const wchar_t* PutItemClass(Arm a);

// The live prop elements of class `cls` within 500 cm of `target`, the target itself not counted; -1 when the target's
// place does not read. `skip` is an actor never counted: the drive still in a player's hand. One snapshot per call: once per
// arm, never per tick.
int PutStrayCount(const wchar_t* cls, void* target, void* skip = nullptr);

// The own inventory's last record of the arm's item class selected on the open screen with the hover cleared, as the
// hotbar key's put reads it; false, with `why` filled, when the screen does not read as a put of it. Both halves use it.
bool PutSelectOwnItem(void* ui, int32_t& sel, char* why, size_t whyLen);

// The host's half: its steps, and its state back to the start.
void PutHostReset();

// The reds of the put arms (container_intent_drill_put2.cpp): each dev row is read once and its knob set on the peer it
// breaks, every tick (a session's end puts the knobs back). Inert when no row is on.
void PutArmClientKnobs();
void PutArmHostKnobs();

// ---- the client's machine (put.cpp), for the arms that interleave (put2.cpp) -------------------------------------

// What the press was judged against, taken just before it.
struct PutBase {
    coop::props::container_act_request::dev::Put put0;
    int own0 = -1;
    int count0 = -1;
    uint32_t packEid = 0;
    void* pack = nullptr;
};
PutBase PutBaseOf();

// What the settle read, after the act's end and kSettleMs.
struct PutSettle {
    coop::props::container_act_request::dev::Put p;
    int own = -1;
    bool held = false;   // the hand holds an actor
    int stray = -1;
};

void PutFail(const char* fmt, ...);     // the client's end with the FAIL line (the host is told)
void PutNotMeasured(const char* why);   // the client's end with NOT MEASURED (the host is told)
int  PutPhase2();                       // the host's insert_notready mode, said on the fixture line (2: putowed runs its second phase)
void PutPass(int stray);                // the client's end with the PASS line
bool PutActEnded();                     // the last put names a new id and is no longer only asked
void PutToSettle();                     // the act is over: the machine reads the counts and judges
void PutEnterHook();                    // the machine hands its tick to Put2Hook
bool PutAskR2(void* ui);                // a hotbar-key put of the own inventory's item, its baseline taken; false: FAIL printed
bool PutHoverSlot(void* ui, void* slot);

// put2.cpp: what an arm does around the ask and at the settle.
bool Put2Idle();                        // a relaunched putdepart client has nothing to do
void Put2BeforeAsk();                   // holds the replay for the arms that interleave
bool Put2Hooked();                      // after the ask the arm runs Put2Hook, not the plain act wait
int  Put2Pockets();                     // how many items the arm's client pockets
void Put2Hook();
bool Put2Judge(const PutSettle& s);     // the verdict of an arm that is not putrecord's; false: the plain verdict applies
void Put2Reset();
void Put2Poll();                        // every client tick: the host's go and release lines of putjoin are latched
bool Put2AskGated();                    // putjoin's first client waits for the host's go before it asks; true: the hook holds the ask
const char* Put2WaitPrefix();           // the fixture line this client waits for
const char* Put2DonePrefix();           // and the line it ends with
void Put2OnMirrors();                   // the fixture's mirrors are here: a putjoin joiner says what its seed showed
int  PutNotReadyMode();                 // container_act_insert_notready, as the row says
void PutKnobRelease();                  // the drill has put insert_notready back to 0: the knob is not armed again

// ---- the host's putjoin (container_intent_drill_put_join.cpp), and what it reads of the host machine -------------

struct PutHostView {
    void* target = nullptr;
    int count0 = -1;   // the target's record count at the fixture line
    void* item2 = nullptr;   // the second item (putdeadline, putjoin)
};
PutHostView PutHostViewOf();
void PutJoinHost(const coop::net::Session& s);
void PutJoinNoteSlot1(int slot);   // the slot the fixture was spawned for
void PutJoinReset();

}  // namespace coop::dev::container_intent_drill::detail
