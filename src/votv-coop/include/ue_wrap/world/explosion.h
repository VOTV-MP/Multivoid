// ue_wrap/world/explosion.h -- the explosion classes of the game: explosion_C (an AActor; the blast, its
// radial force and its damage verbs), and its two Blueprint children explosion_pc_C and
// explosion_saltrocket_C. A class table: which UClass each is, resolved by name, and which of the three a
// class is. Engine access only. Game thread.
#pragma once

#include <cstdint>

namespace ue_wrap::explosion {

// The three classes, in the order a wire's classId numbers them.
enum class Cls : uint8_t { Explosion = 0, Pc = 1, SaltRocket = 2, Count = 3 };

// Resolve each unresolved class, when the object index has no backlog: by name from the index, else (in the
// gameplay world only) ONE load by its asset path per class per world. A failed load is logged once and not
// repeated until Reset(). Each class logs once when it resolves. True when all three are resolved.
bool Resolve();

// The class at once: from the index, else the one load by path this world allows (no backlog gate). Null when
// neither answers. Counts as that class's one load attempt for this world.
void* ResolveNow(Cls c);

// Forget the pointers and the load attempts: a new world resolves again.
void Reset();

bool Resolved(Cls c);

// The class object, null while unresolved.
void* ClassPtr(Cls c);

// Which of the three `cls` is, or -1. If the index unlisted a class since the last call, the pointers are
// cleared first. Then each resolved class is compared by pointer and each unresolved one by name; only without
// an exact match, `cls`'s super chain is compared by pointer for two steps. `byName` is set when a name
// matched.
int ClassIdOf(void* cls, bool& byName);

// The class's short name, as the game spells it.
const wchar_t* ClassName(Cls c);

// Narrow an explosion's radial force to pawns: its RadialForce member's own RemoveObjectTypeToAffect, called twice with
// the enum bytes 1 and 3 (WorldDynamic, PhysicsBody; byte 2, Pawn, stays). The component's array is never written
// raw: that does not narrow. False when the member or the function does not resolve, or a call fails.
bool NarrowForceToPawn(void* explosion);

// The parameters a blast reads, as the host captured them. objType is explosion_C's Int.
struct Params {
    float   damage, radius, force, fireStrength, fireFuel, innerShake, outerShake;
    int32_t objType;
    bool    ignoreWalls, skipSetting;
};

// A new explosion of class `c` at a full transform (rotation = pitch, yaw, roll in degrees; the scale included, which
// engine::BeginDeferredSpawn takes no argument for), its parameters written between the deferred spawn and its finish,
// and, when `ignoreActor` is non-null, its `ignores` set to that one actor. The class must be resolved. Game thread.
// Null on failure: a property that does not resolve is said once per class, before anything is spawned.
void* SpawnDeferred(Cls c, const float loc[3], const float rot[3], const float scale[3], const Params& p,
                    void* ignoreActor);

}  // namespace ue_wrap::explosion
