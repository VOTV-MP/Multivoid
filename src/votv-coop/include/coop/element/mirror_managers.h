// coop/element/mirror_managers.h -- the ONE canonical home for the entity mirror managers.
// There are exactly THREE streamed-mirror kinds, each a process-wide MirrorManager<T> singleton
// (the MTA per-type CClient*Manager analog):
//
//   PropMirrors() -> MirrorManager<Prop>        chipPiles, trash, kerfur OFF-props, grabbables
//   NpcMirrors()  -> MirrorManager<Npc>         kerfur NPC form, zombies, ariral, wisps, AI chars
//   WaMirrors()   -> MirrorManager<WorldActor>  UFOs, ships, jellyfish (allowlisted event actors)
//
// (Player is NOT here -- it has its own coop::players::Registry. Kerfur is a host-only logical
// id record with no manager; its rendered form is a Prop or Npc mirror above.)
//
// RULE 2, one concept and one implementation: include this header and
// `using coop::element::{Prop,Npc,Wa}Mirrors;`, or call fully qualified, rather than declaring a
// local inline wrapper of your own.

#pragma once

#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/element/world_actor.h"

namespace coop::element {

inline MirrorManager<Prop>& PropMirrors() { return MirrorManager<Prop>::Instance(); }
inline MirrorManager<Npc>& NpcMirrors() { return MirrorManager<Npc>::Instance(); }
inline MirrorManager<WorldActor>& WaMirrors() { return MirrorManager<WorldActor>::Instance(); }

}  // namespace coop::element
