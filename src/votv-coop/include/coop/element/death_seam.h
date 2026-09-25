// coop/element/death_seam.h -- an element's end of play, announced once, at the engine's own moment.
//
// A lane that syncs an actor has to learn when that actor stops existing: a prop broken or sold, a pile
// emptied, an NPC killed, a pinecone's lifespan run out. Sixteen death-watches learned it by polling its
// liveness, two of them behind a camera-distance proxy, beside patches on single destroy routes: a poll
// hears a death late, or never where a pass erased the entry first or the proxy judged it far, and a
// patch misses the routes it does not watch. The engine ends every actor's play in one function,
// AActor::EndPlay, for a destroy of any kind and for a stream-out (ue_wrap::actor_end_play); this module
// makes that one announcement per element. The sink, inside EndPlay, records which element ended and
// why, and does nothing else: a destroy or a spawn there would run inside the engine's own. The drain,
// on the game thread outside it, hands each record to the handlers subscribed for the element's type.
// MTA's shape is its element deleter: one announcement, the consumers subscribed, the free at one
// controlled point (reference/mtasa-blue/Client/mods/deathmatch/logic/CElementDeleter.cpp:22-50). A
// level transition and the quit are not announced, and an end recorded in another world than the one
// the drain runs in is dropped there (ue_wrap::world_identity). Coop layer; game thread.

#pragma once

#include "coop/element/element.h"

#include <cstdint>

namespace coop::element::death_seam {

// `actor` is the actor's identity: by the drain it has ended its play and may be marked for death, so
// it is read only while its slot still names it, `actorIndex` and `actorSerial` (the slot's serial at
// the end, 0 when none was ever drawn) being what a handler checks that by.
struct Death {
    ElementId   eid;
    ElementType type;
    bool        mirror;       // the element mirrors an entity another peer owns
    bool        streamedOut;  // its level was streamed out: the entity itself may come back
    void*       actor;
    int32_t     actorIndex;
    int32_t     actorSerial;
};

// Called from the drain, game thread, once per element end, in the order the engine ended them; a
// drain hands out its element ends before its class ends.
using Handler = void (*)(const Death& death);

// Subscribe `handler` to the ends of elements of `type`. Idempotent per pair; false when the type's
// table is full. Game thread.
inline constexpr int kMaxHandlersPerType = 8;
bool Subscribe(ElementType type, Handler handler);

// A lane that follows actors by their class rather than as elements -- a keyed channel's decals, its
// piles -- hears their ends by class: an actor whose class, or a class above it, is named `className`,
// compared by name, so a class a world change reloads at a new address still matches.
struct ActorEnd {
    void*   actor;        // as in Death
    int32_t actorIndex;
    int32_t actorSerial;
    bool    streamedOut;  // its level was streamed out: the actor itself may come back
};
using ClassHandler = void (*)(const ActorEnd& end);

// Subscribe `handler` to the ends of actors of the class named `className` and its subclasses. The name
// resolves through the engine on the game thread; false while it does not resolve yet (ask again) or
// when the table is full. Idempotent per pair. Game thread.
inline constexpr int kMaxClassHandlers = 16;
bool SubscribeClass(const wchar_t* className, ClassHandler handler);

// Add the sink to actor_end_play. Idempotent; false when the seam did not install. Game thread.
bool Install();

// Ends are recorded only while a holder has the seam, as the script gate is held: the session takes it
// while it runs and gives it back when it stops -- and when a host's last peer leaves, taking it again
// the next tick -- which drops what was recorded and not handed out, so a solo stretch between sessions
// leaves nothing for the next one. Counted; game thread.
void Acquire(const char* who);
void Release(const char* who);

// Hand the recorded ends to their handlers. The pump, game thread, outside any engine destroy. Both
// queues are taken before any handler runs: an actor a handler ends is recorded for the next drain, and
// a handler that faults costs its own batch, which is never handed out twice.
void Drain();

}  // namespace coop::element::death_seam
