// coop/dev/director/input_stub.h -- a key at the human-input seam: mainPlayer's input-action event stub, resolved once
// by its generated name and called with its zeroed frame, or an AnyKey event stub called with the key it names. The
// director's processes and the drills that press a key themselves share this one. DEV-ONLY, like the rest of the
// director; game thread.

#pragma once

#include "ue_wrap/core/reflection.h"

namespace coop::director {

// The stub's generated name (a game update renames the K2Node stubs), its function once resolved, and whether the
// resolution was tried.
struct InputStub {
    const wchar_t* name;
    void* fn = nullptr;
    bool  tried = false;
};

// Resolved once (said once when it does not resolve) and called with a zeroed frame (the Key it is given is not
// read). False when it did not resolve or the call failed.
bool PressInputEvent(void* player, InputStub& stub);

// A key event of mainPlayer's AnyKey pair (the stub is the press or the release one, by its generated name): the
// stub resolved as above and called with a frame whose FKey is `key`, a name the game's own input settings gave (an
// FKey is its name first; the engine looks its details up from the name). False when the stub did not resolve, its
// frame has no `Key`, or the call failed.
bool PressKeyEvent(void* player, InputStub& stub, const ue_wrap::reflection::FName& key);

}  // namespace coop::director
