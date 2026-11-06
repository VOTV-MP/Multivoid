// coop/dev/kerfur_serve_drill_disc_judge.h -- the host's judge of leg D (kerfur_serve_drill_disc.cpp): what a variant's press
// must have done, from the client's after line and the host's own reads. Pure: strings and numbers in, the failing terms
// out. Not for any other file.

#pragma once

#include <string>
#include <vector>

namespace coop::dev::kerfur_serve_drill::disc {

// The knob's values: Base (on) is the plain measurement; Swap takes a second disc into the hand right after the press (the
// host's answer then finds another disc); Wh holds the class the game refuses; Busy presses again after the consume.
enum class Variant { Base, Swap, Wh, Busy };

// The host's reads of its own side of the press.
struct Side {
    bool hasRead = false, has = false;
    bool laneHolds = false;
    std::string laneItem;
    bool keyHeld = false;
    int discs = 0;
    std::string per;
};

// The first press: empty when every term of the variant holds, else the failing terms with their values. `f` is the
// client's after line (gone|held, discs, local body, state, consumed, consume-miss, hint); `carries` the spans that built a
// copy since the snapshot and `published` the PropSpawn + PropDestroy the host's session accepted inside them.
std::string JudgeFirst(Variant v, const std::vector<std::string>& f, const Side& after, long long carries,
                       long long published);

// The busy variant's second press: refused while the Omega is on its job, the second disc kept, no new copy. `f` is the
// client's second line (held|gone, refusals shown, discs consumed).
std::string JudgeSecond(const std::vector<std::string>& f, const Side& after, long long carries, long long published);

}  // namespace coop::dev::kerfur_serve_drill::disc
