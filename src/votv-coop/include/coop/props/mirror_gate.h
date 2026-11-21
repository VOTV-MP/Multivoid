// coop/props/mirror_gate.h -- whether a prop actor is a peer's copy, whose own authored state does not run here.
//
// On an active client session every world prop is the host's copy except the one in the LOCAL player's hand, which is its
// holder's own. On any other session the copy shown in another slot's hand (hand_item's mirror) is a peer's. A gate that
// refuses a Blueprint body on such a copy asks this, so one rule says which actors those are. Game thread.

#pragma once

namespace coop::mirror_gate {

// True when `actor` is a copy of a peer's prop: on an active client session any actor but the local hand-view one; on the
// host or in single player, a remote hand mirror.
bool IsPeerCopy(void* actor);

}  // namespace coop::mirror_gate
