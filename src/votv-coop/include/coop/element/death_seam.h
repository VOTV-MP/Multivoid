// coop/element/death_seam.h -- an element's end of play, announced once, at the engine's own moment.
//
// A lane that mirrors an actor has to learn when that actor stops existing: a prop broken or sold, a
// pile emptied, an NPC killed, a pinecone's lifespan run out. Sixteen lanes learned it by polling -- a
// liveness sweep, a camera-distance proxy, a patch on one destroy route -- and each missed the routes it
// did not watch (POLL arc §2.4). The engine ends every actor's play in one function, AActor::EndPlay, for
// a destroy of any kind and for a stream-out (ue_wrap::actor_end_play); this module makes that one
// announcement per element. The sink, inside EndPlay, records which element ended and why, and does
// nothing else: a destroy or a spawn there would run inside the engine's own. The drain, on the game
// thread outside it, hands each record to the handlers subscribed for the element's type. MTA's shape is
// CElementDeleter with onClientElementDestroy: one announcement, the consumers subscribed, the free at one
// controlled point. A level transition and the quit are not announced: the world's generation covers
// them (ue_wrap::world_identity). Coop layer; game thread.

#pragma once

#include "coop/element/element.h"

#include <cstdint>

namespace coop::element::death_seam {

struct Death {
    ElementId   eid;
    ElementType type;
    bool        mirror;       // the element mirrors an entity another peer owns
    bool        streamedOut;  // its level was streamed out: the entity itself may come back
    void*       actor;        // identity only, never dereferenced: the actor is being torn down
    int32_t     actorIndex;   // its object-array slot, for reflection::IsLiveByIndex
};

// Called from the drain, game thread, once per element end, in the order the engine ended them.
using Handler = void (*)(const Death& death);

// Subscribe `handler` to the ends of elements of `type`. Idempotent per pair; false when the type's
// table is full. Game thread.
inline constexpr int kMaxHandlersPerType = 8;
bool Subscribe(ElementType type, Handler handler);

// A lane that follows actors by their class rather than as elements -- a keyed channel's decals, its
// piles -- hears their ends by class: an actor whose class, or a class above it, is named `className`,
// compared by name, so a class a world change reloads at a new address still matches.
struct ActorEnd {
    void*   actor;        // identity only, never dereferenced: the actor is being torn down
    int32_t actorIndex;   // its object-array slot
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

// Hand the recorded ends to their handlers. The pump, game thread, outside any engine destroy.
void Drain();

}  // namespace coop::element::death_seam
