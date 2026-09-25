// coop/dev/class_lifetime_probe.h -- [dev] whether a class, and a function it declares, outlive the
// world that loaded them. Every loaded class is read from the object index with no walk (the Blueprint
// classes are the instances of the Blueprint class meta-classes, the native ones the instances of
// Class) and recorded by name with its pointer, slot and slot serial. Once per world, the menu's
// included, once the index has drained the world's load, each is judged against the identity it had
// when last seen: the same object (same slot, and the serial the record took), the same address
// holding a new object (the serial moved), a new address, or gone. The census repeats every 30 s within
// a world, so a class a world loads late is on record when the next world is judged. The functions its
// watched rows name, those the lanes hooked by pointer, are judged the same way, and the native classes
// are the control. Armed by
// class_lifetime_probe=1; read-only but for the slot serials it assigns, as a weak pointer would. Game
// thread.
#pragma once

namespace coop::dev::class_lifetime_probe {

// The frame tail's entry (harness::pump::TickFrameTail): one latched flag read when off. Game thread.
void Tick();

}  // namespace coop::dev::class_lifetime_probe
