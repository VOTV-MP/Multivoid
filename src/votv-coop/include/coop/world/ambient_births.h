// coop/world/ambient_births.h -- the births and deaths of the ambient actors, named once for every lane that needs them.
//
// The walking tree, the sus hole, the beehive branch and the wasp nest are each made and unmade by the engine, which
// announces both through the object index's listener list. One listener here recognises the four classes and keeps one
// ordered queue of (object, slot, birth or death, class); the creature lane and the fixture lane drain their classes in
// their own ticks, so neither scans the world for what the engine already says.
//
// The callback only queues. A birth is handed over once its object reads (a class default object and an object still
// loading are held back), a death by the object's slot alone. An object that stood when the session began is seeded as
// a birth at Install; the engine may announce one of those again, so a consumer keys by object and takes a birth of one
// it holds as a repeat. A consumer checks that the slot still holds a born object before it reads it: it may have died.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::ambient_births {

enum class Class : uint8_t { Tree, Hole, Branch, Nest };  // walkingTree_C, susDirtHole_C, beehiveBranch_C, prop_waspnest_bee_C
enum class Kind : uint8_t { Birth, Death };

using Mask = uint8_t;
constexpr Mask Bit(Class c) { return static_cast<Mask>(1u << static_cast<unsigned>(c)); }

struct Event {
    void*   obj;    // a death's object may be freed: compare it, never read through it
    int32_t index;  // its slot in the object array
    Kind    kind;
    Class   cls;
};

// Session start: begin recording and seed what stands now. Game thread. Idempotent for a session already begun.
void Install();

// Append the ready events of the classes in `mask` to `out`, in the order they happened, and remove them from the
// queue. Call it every tick, on either role: an event nobody takes waits, up to the queue's cap. Game thread.
void Drain(Mask mask, std::vector<Event>& out);

// The session ended: recording stops and both queues clear.
void OnDisconnect();

}  // namespace coop::ambient_births
