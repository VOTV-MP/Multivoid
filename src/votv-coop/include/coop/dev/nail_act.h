// coop/dev/nail_act.h -- [dev] the nailgun projectile's act, for a probe or a drill: two props stand in a line, a nail spawns
// before the first one's outer face, aimed at the second, with a length that reaches past it, and its own nailNail ties them. The held nail's two
// pieces (a deferred spawn with a length, a verb call) are here too. What a caller wants said as the act goes, and what it
// wants a spawned nail labelled, it gives through a Voice. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <string>

namespace coop::dev::nail_act {

// How the act speaks: a probe narrates every step under its own tag and labels what it spawns before the spawn finishes (its
// watches name a nail by the label); a drill logs plainly and labels nothing. Every line arrives formatted.
class Voice {
public:
    virtual ~Voice() = default;
    virtual void Label(void* actor, const char* name) = 0;
    virtual void Info(const char* line) = 0;
    virtual void Warn(const char* line) = 0;
    virtual void Event(const char* line) = 0;
    // The act could not run: a probe counts and warns, a drill ends INVALID.
    virtual void Unmeasured(const char* act, const char* line) = 0;
    // "loc=(x,y,z) rot=(p,y,r) scale=(x,y,z)" of an actor, the name a Voice gave an actor, and the state line of a nail.
    virtual std::string TransformOf(void* actor) = 0;
    virtual std::string LabelOf(void* actor) = 0;
    virtual std::string NailState(void* nail) = 0;
};

// A deferred spawn of the nail class at `loc`, a `length` written before the finish when positive, labelled before the finish
// so the watches name it. Null when it did not spawn.
void* SpawnNail(Voice& v, void* nailCls, const char* label, const ue_wrap::FVector& loc, float length,
                const ue_wrap::FRotator& rot = {});

// nailNail and attach take `a` and answer `fail`. False when the verb did not run.
bool CallVerb(void* nail, const wchar_t* name, bool& fail);

// The nail's own n(): where its segment runs, said as an Info line.
void SaySegment(Voice& v, void* nail);

// A nail the two-prop way, as the projectile puts one in: aimed along the flat line from `a`'s centre to `b`'s, its tip a
// centimetre before `a`'s outer face on that line (the face away from `b`), a length that reaches past `b`, read from the
// bounds of both as they stand now, then nailNail. Says its own Unmeasured line under `act` when it cannot.
bool NailThePair(Voice& v, const char* act, const char* label, void* nailCls, void* a, void* b, void*& nail);

}  // namespace coop::dev::nail_act
