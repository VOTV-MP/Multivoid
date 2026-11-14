// coop/gamefix/body_guard.h -- is a game function still the body a fix was written for?
//
// A fix under coop/gamefix corrects one function of one game build. It names that function's body
// by the hash the fingerprint census keeps for it (ue_wrap/core/script_fingerprint, bodyHash), and
// acts only while the running game's function hashes the same: a build whose developer rewrote the
// function, fixed or not, is left alone. The answer is kept per function object and asked again
// when another comes (a new world reloads a Blueprint class). Game thread.

#pragma once

#include <cstdint>

namespace coop::gamefix {

class BodyGuard {
public:
    constexpr BodyGuard(const char* what, uint64_t bodyHash) : what_(what), want_(bodyHash) {}

    // True when `function`'s statements hash to the body the fix corrects. The verdict for a new
    // function object is logged once.
    bool Holds(void* function);

private:
    const char* what_;
    uint64_t    want_;
    void*       fn_ = nullptr;
    int32_t     fnIdx_ = -1;
    int32_t     fnSerial_ = 0;
    bool        holds_ = false;
};

}  // namespace coop::gamefix
