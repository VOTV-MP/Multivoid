// coop/dev/recycled_slot_drill.h -- [dev] an object slot the engine recycled, as the object index and
// a reflected read meet it.
//
// The engine hands a dead object's array slot, and often its address, to the next object it
// allocates, and the object index applies the births and deaths it is told of a drain later. The
// drill stages both on the local pawn's root component, which is not an actor: a birth that names
// the component under the pawn's class, judged by what the slot then lists, and a listing under that
// class with no check at all, judged by what the index hands out; each is followed by the
// component's own listing restored, which the index's 60 s summary then shows as births and one
// recycled. Then it dispatches the actor location read on the component, whose read faults inside
// the engine, and asks whether the call failed and the firewall absorbed exactly one fault. One
// verdict per half, then DONE; on whichever peer enables it, once the index lists the component.

#pragma once

namespace coop::dev::recycled_slot_drill {

bool IsEnabled();

// The drill once it is ready, then nothing. A single read when off. Game thread.
void Tick();

}  // namespace coop::dev::recycled_slot_drill
