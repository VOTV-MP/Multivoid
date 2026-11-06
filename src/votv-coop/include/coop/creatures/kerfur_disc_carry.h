// coop/creatures/kerfur_disc_carry.h -- a client's get_reports with the floppy disc in its hand.
//
// The Omega's get_reports reads the requester's hand and destroys the disc it takes. The host runs the game's own verb for
// a client, but the client's hand is on the client's machine, so the disc travels: the press captures the held disc's record
// into the command (CLIENT), and the host builds a real copy of it for the verb's span, in the puppet's hand
// (HOST). The copy never leaves the host: it is marked so neither its birth nor its destroy is published. When the verb
// destroys it, the host answers `consumed-held`, and the client's own machine destroys its disc through the game's
// destroy, if it is still the disc it pledged. Nothing imitates the verb's effect. A disc of the Wh class is not carried:
// the game refuses it itself, with its own hint.
//
// Principle 7: gameplay/network module; engine access through ue_wrap only. Page: docs/npcs-and-kerfur.md. Game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"

#include <cstdint>
#include <optional>
#include <string>

namespace coop::kerfur_disc_carry {

// ---- CLIENT ---------------------------------------------------------------------------------------------------

// What a client's press pledged: the disc in its hand, by slot and serial, so the host's word later destroys that
// disc and no other. The class and the name are for the log.
struct Pledge {
    ue_wrap::CachedObjRef actor;
    std::wstring cls;
    std::wstring name;
};

// What a press carries: the disc's record as the game itself serializes it, and the pledge.
struct Capture {
    ue_wrap::save_record::SaveRecord record;
    Pledge pledge;
};

// At the seat (a script-gate PRE on the game thread, where an engine call is allowed): the local player's held
// actor when it is a floppy disc and not a Wh disc, with its record captured. Nothing when the hand is empty or holds another
// thing (the game's own refusal hint then comes from the host's run) and when the capture failed or the record is past
// the wire's bounds (said).
std::optional<Capture> CaptureHeld();

// The host consumed the disc this pledge names: destroyed through the game's own destroy when the hand still holds that
// disc and no other, else a miss (the player swapped or stowed it within the round trip), said either way. True when
// the disc was destroyed here.
bool OnConsumed(const Pledge& pledge);

// ---- HOST -----------------------------------------------------------------------------------------------------

// What the host makes of a tail at its arrival. Malformed: a hostile client's word (a name past its bound, a class that
// is no floppy disc), dropped and counted, unanswered. Unavailable: an honest client the host cannot serve yet (the
// disc's class is not loaded here, or the hand lane has not shown the disc), answered when the command is served.
enum class TailVerdict : uint8_t { Ok, Malformed, Unavailable };

// The tail's class must be of the floppy family by ClassByName (hostile input) and must EQUAL the class `slot`'s hand
// lane announced, judged by hand_history at the command's arrival `arrivalMs`. `why` names a Malformed.
TailVerdict JudgeTail(const ue_wrap::save_record::SaveRecord& tail, uint8_t slot, uint64_t arrivalMs, const char** why);

// The Omega's own gates of get_reports, read before anything is spawned. HasDisc: it already holds one, so the verb
// skips the read and runs as the game runs it, with no carry. Busy: `animation`, `server` or `remoteControl` is set, so
// the read would come on a later move, outside the span. (A job is the kerfur's state, judged by kerfur_command.)
enum class Gate : uint8_t { Open, HasDisc, Busy };
Gate ReadGate(void* omega);

// The span of one get_reports on a client's disc: a copy of the record is built at `slot`'s puppet, in the puppet's
// hand, and the puppet's own hand is put back on every exit, with the copy destroyed if it is still alive. The copy is
// never published (not its birth, not its destroy), its collision and physics are off, and it lives no longer than the
// verb's synchronous call.
class HandScope {
public:
    HandScope(uint8_t slot, const ue_wrap::save_record::SaveRecord& record);
    ~HandScope();
    HandScope(const HandScope&) = delete;
    HandScope& operator=(const HandScope&) = delete;

    // Whether the copy stands in the puppet's hand: false when it could not be built, and the verb is then not run.
    bool ok() const { return ok_; }
    // After the verb: whether the copy is gone, which only the verb's own destroy does.
    bool Consumed() const;

private:
    void* puppet_ = nullptr;
    void* previous_ = nullptr;  // the puppet's holding_actor before the write
    bool wrote_ = false;
    bool ok_ = false;
    uint8_t slot_ = 0;
    ue_wrap::CachedObjRef copy_;
};

}  // namespace coop::kerfur_disc_carry
