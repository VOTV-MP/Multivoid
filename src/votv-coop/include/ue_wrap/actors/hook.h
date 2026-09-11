// ue_wrap/actors/hook.h -- Ahook_C, the deployed grappling hook, and the rope, which is a SUBCLASS
// of it rather than a sibling. Engine-wrapper layer: class and member resolution, the state a lane
// reads, a display mirror, and the actor's own save record. No network, no coop state.
//
// Three measured facts shape the file, each stated where it bites: the hook's tick reels the LOCAL
// player whoever threw it (ReceiveTickFunction); hook_C is an Aactor_save_C, so an unmarked mirror
// lands in the host's save (WriteSkipSave); and the actor's own getData already carries the anchor
// identity (CaptureRecord / AdoptRecord).
//
// Game thread only: every path here reads or calls engine object state.

#pragma once

#include <cstdint>

#include "ue_wrap/actors/save_record.h"  // SaveRecord -- the hook's own getData payload
#include "ue_wrap/core/types.h"          // FVector, FRotator

namespace ue_wrap::hook {

// The deployable hook classes, in wire order: the index IS the wire's classId, so a row may be
// appended but never reordered or removed.
//
// hook_Child_C is deliberately absent. It is placed by the level and attaches itself to its own
// manual_A / manual_B fields in its own BeginPlay, so it already exists on every peer; mirroring it
// would put two hooks where the world has one.
enum class Kind : uint8_t {
    Hook  = 0,  // hook_C, the standard grapple
    Flesh = 1,  // hook_flesh_C, the long-range variant
    Rope  = 2,  // rope_C, which spawns broken halves when its constraint breaks
    Count = 3,
};

// Resolve the classes and members once. False while the world has not loaded them, and false
// FOREVER once a capped number of passes has run with the class in hand but a member missing: no
// offset fallbacks, because an unresolved member means this is not the class the wrapper was
// written against and a guessed offset would write into whatever now lives there.
bool EnsureResolved();

// `actor`'s Kind, or Count when it is not a deployable hook. An EXACT class compare, not a descent
// test: hook_Child_C descends from hook_C and must never match.
Kind KindOf(void* actor);

// The UClass* for a Kind, or null when unresolved / out of range.
void* ClassForKind(Kind k);

// The Blueprint class NAME for a Kind, empty past the table. An arbiter writes this into a record
// it adopts in place of the class name a sender sent, because a class name reaching a spawn is a
// spawn primitive.
const wchar_t* ClassNameOf(Kind k);

// The one pointer that discovers a player's own hook: mainPlayer_C's `activeHook` field. Null when
// that player has no hook in hand, and reading it costs one load.
//
// It goes null for TWO different reasons and a caller must tell them apart by the actor's liveness:
// the hook was destroyed (a cancel, an interrupt, one of attach_a/attach_b's reject cases), or it
// anchored its second end and the game let go of a hook that is still standing there. The field is
// also left DANGLING -- prop_hook_C returns without clearing it when the hook is already invalid --
// so the liveness check is not optional.
void* ActiveHookOf(void* mainPlayer);

// Everything a lane needs off a live hook, in one read.
//
// The A end's pose is the HEAD's and is taken from the `A` component rather than from the actor:
// during flight `throw` re-parents `A` onto the thrown physics sphere while the actor root stays
// behind at the camera, so the actor transform is not the head.
struct State {
    bool     attachedA    = false;  // the head has bitten something
    bool     attachedB    = false;  // the second end is anchored -- with attachedA, the hook is
                                    // world furniture and has left its owner
    bool     thrown       = false;  // isThrown: in the air
    bool     playerHooked = false;  // the cable is reeling its owner
    float    dist         = 0.f;    // the cable length the reel sets
    FVector  aLoc{};                // the head, world space
    FRotator aRot{};                // the head, world space
};
bool ReadState(void* hookActor, State& out);

// The two actors a hook's ends are tied to: `actor_a` is what the head bit, `actor_b` the tail's
// anchor -- the thrower while the hook is in hand (attach_a writes the player there), the second
// anchor once attach_b has run. Raw field reads; either can be null, and a caller that keeps one
// past this call checks its liveness first. False when unresolved.
bool AttachedActors(void* hookActor, void*& outA, void*& outB);

// mainPlayer_C::activeHook, written: the field the game writes when the item fires a hook, so a
// driver that plants one by hand hands it to the owner lane the same way. Null clears it.
bool WriteActiveHook(void* mainPlayer, void* hookActor);

// attach_a with its replace parameters, the game's own programmatic attach: the head binds to
// `component` of `actor` at `location` with `normal`, the tail to `actorAttach`'s root, and the
// constraint is built between them. No hit result is needed, since the verb reads the replace set
// whenever `actor` is valid; `unfreezeFrozen` stays false, the item's own plant. Game thread.
bool AttachHead(void* hookActor, void* actor, void* component, const FVector& location,
                const FVector& normal, void* actorAttach, bool checkLen);

// Aactor_save_C's `skipSave`, which its ignoreSave returns and the game's save walk asks of every
// object implementing int_save_C before serializing it. hook_C overrides neither, so a hook is
// saved by default -- and a mirror of someone else's hook standing on the HOST would be written
// into the one save in the session, to come back as an unattached hook lying where that one
// happened to be. Write it in the deferred window, before the finish, so no autosave can see the
// actor unmarked.
bool WriteSkipSave(void* hookActor, bool on);

// hook_C::ReceiveTick's UFunction, the seam a lane cancels a mirror's Blueprint body at. Null until
// EnsureResolved succeeds. ONE registration covers every Kind: hook_C is the only one of the four
// classes that declares ReceiveTick, so they share its UFunction -- and it must be resolved on
// hook_C, because FindFunction is exact-owner and a resolve against a subclass returns null.
//
// Why a cancel and not a tick disable alone: the CDO can ever tick and nothing on the Blueprint
// chain serializes whether it STARTS ticking, so the tick function registers inside
// FinishSpawningActor and a disable can only land after it. What one frame of that body does on a
// mirror is write the VIEWING peer's own movement velocity toward someone else's hook and flip them
// into a fall -- the tick reads the local player, never the actor that threw the hook. The tick is
// also not cheap: a quiet hook costs eight component-location dispatches a frame and a reeling one
// fourteen, so the cancel is a saving as well as the fix.
void* ReceiveTickFunction();

// Spawn a display mirror of `kind` at a head pose, through the deferred window. Inside it: the
// skipSave mark. After the finish: tick disabled, collision off. Null on any failure, and nothing
// is left half-built. The caller owns the actor and must destroy it.
void* SpawnMirror(Kind kind, const FVector& aLoc, const FRotator& aRot);

// Spawn the CANONICAL hook a host owns after an anchor handoff. The same deferred window and the
// opposite settings: skipSave stays FALSE (this one belongs in the save), the tick runs, and
// collision is whatever loadData leaves. Two functions rather than a flag, because they are
// opposite intents and a boolean at the call site is how one becomes the other by accident.
void* SpawnCanonical(Kind kind, const FVector& aLoc, const FRotator& aRot);

// Drive a mirror from a wire state: the head pose, and the cable length through the game's own
// setLength verb, which writes the three constraint limits and the cable together. The cable is not
// `dist` -- the game divides it -- and that ratio is the thing a hand-written mirror gets wrong.
bool DriveMirror(void* mirror, const FVector& aLoc, const FRotator& aRot, float dist);

// Attach a mirror's `B` end to `ownerRootActor`'s root component, which is what `throw` does with
// the thrower's own body. The tail then follows that peer's puppet for free and never rides the
// wire. Passing null detaches.
bool AttachTailTo(void* mirror, void* ownerRootActor);

// ---- the actor's own save record --------------------------------------------------------------
//
// NOT save_record's CaptureRecord / ApplyRecord: those are gated to the Aprop_C lineage, and their
// apply deliberately splices the receiver's own base half over the sender's, because for a prop the
// base fields already ride the spawn row. A hook is the opposite case -- an Aactor_save_C with no
// spawn row, where the base half IS the thing being moved. Same codec, different operation.

// Capture `hookActor`'s record as the actor itself serializes it. One dispatch.
bool CaptureRecord(void* hookActor, ue_wrap::save_record::SaveRecord& out);

// Hand a record to `hookActor`'s own loadData, then run processKeys so the game re-resolves both
// attach keys in THIS world and re-attaches. processKeys tails unconditionally into a one-frame
// delay that runs makeAttachments and assign, which is the game's own retry for an anchor that has
// not loaded yet. Two dispatches.
//
// APPLIES THE RECORD AS GIVEN. loadData writes the actor's save key and both attach keys, so a
// caller handing this something off the wire has already decided, field by field, what it will take
// from the sender. The engine wrapper does not know what a sender is; that judgement is the lane's.
// loadData also re-enables both collision spheres, so a mirror's caller turns collision off again.
bool AdoptRecord(void* hookActor, const ue_wrap::save_record::SaveRecord& r);

// THE RECORD DOES NOT CARRY THE PHASE. getData writes an empty bool group, and attached_a and
// attached_b are written only inside attach_a and attach_b, neither of which processKeys calls. So
// a hook rebuilt from its own record has both false, and unhook_ then fails its
// attached_a && attached_b gate and destroys the hook instead of dropping a pickup -- the player
// can never get that hook back.
//
// The canonical copy therefore has both restored after the adopt. A MIRROR deliberately does not:
// with the flags clear, its OnDestroyed bindings destroy it rather than minting a second pickup,
// and assign's collision re-enable, gated on the same pair, cannot fire two frames later and undo
// the mirror's collision-off.
bool WriteAttachedFlags(void* hookActor, bool attachedA, bool attachedB);

// hook_C's `maxDist`, the cable ceiling, which the flesh variant raises twentyfold. An arbiter
// clamps a wire `dist` to it, the clamp the game itself applies when it sets the length.
float MaxDistOf(void* hookActor);

// Drop the cached class and member lookups (level change / disconnect).
void ResetCache();

}  // namespace ue_wrap::hook
