// coop/creatures/kerfur_disc_carry.h -- a client's get_reports with the floppy disc in its hand.
//
// The Omega's get_reports reads the requester's hand and destroys the disc it takes. The host runs the game's own verb for
// a client, but the client's hand is on the client's machine, so the disc travels: the press captures the held disc's record
// into the command (CLIENT), and the host builds a real copy of it for the verb's span, in the puppet's hand
// (HOST). The copy never leaves the host; when the verb destroys it, the host answers `consumed-held`, and the client's
// own machine destroys its disc through the game's destroy, if it is still the disc it pledged. Nothing imitates the
// verb's effect. A disc of the Wh class is not carried: the game refuses it itself, with its own hint.
//
// Principle 7: gameplay/network module; engine access through ue_wrap only. Page: docs/npcs-and-kerfur.md. Game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"

#include <optional>
#include <string>

namespace coop::kerfur_disc_carry {

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

// CLIENT, at the seat (a script-gate PRE on the game thread, where an engine call is allowed): the local player's held
// actor when it is a floppy disc and not a Wh disc, with its record captured. Nothing when the hand is empty or holds another
// thing (the game's own refusal hint then comes from the host's run) and when the capture failed or the record is past
// the wire's bounds (said).
std::optional<Capture> CaptureHeld();

}  // namespace coop::kerfur_disc_carry
