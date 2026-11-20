// coop/dev/nail_act.h -- [dev] the nailgun projectile's act, for the constraint-join drill: two props stand in a line, a nail
// spawns before the first one's outer face, aimed at the second, with a length that reaches past it, and its own nailNail ties
// them. What the drill wants said as the act goes, and what to do when the act cannot run, it gives through a Voice. Game thread.

#pragma once

#include <string>

namespace coop::dev::nail_act {

// How the act speaks: the drill logs every line plainly under its own tag. Every line arrives formatted.
class Voice {
public:
    virtual ~Voice() = default;
    virtual void Info(const char* line) = 0;
    virtual void Warn(const char* line) = 0;
    virtual void Event(const char* line) = 0;
    // The act could not run: the drill ends INVALID.
    virtual void Unmeasured(const char* act, const char* line) = 0;
    // "(x,y,z)" of an actor, the name the drill gave an actor, and the state line of a nail.
    virtual std::string TransformOf(void* actor) = 0;
    virtual std::string LabelOf(void* actor) = 0;
    virtual std::string NailState(void* nail) = 0;
};

// A nail the two-prop way, as the projectile puts one in: aimed along the flat line from `a`'s centre to `b`'s, its tip a
// centimetre before `a`'s outer face on that line (the face away from `b`), a length that reaches past `b`, read from the
// bounds of both as they stand now, then nailNail. Says its own Unmeasured line under `act` when it cannot.
bool NailThePair(Voice& v, const char* act, const char* label, void* nailCls, void* a, void* b, void*& nail);

}  // namespace coop::dev::nail_act
