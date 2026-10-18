// coop/permissions/gameplay_nodes.h -- the `multivoid.gameplay.*` nodes: what a player's own acts in the shared
// world may do, each a right the host can withhold from a player. One home for the domain, as grants_core.h is for
// `multivoid.dev.local.*`: the table names the node, its default and its description, and Install declares them
// in the command registry, where `/mv permission` and the info listing find a node. The default is the maintainer's
// preset, GRANTED: a player may do what the game lets them until the host says otherwise. The checks are
// permission_host::Allows, asked by the lane the node guards. Engine-free.

#pragma once

#include <cstddef>

namespace coop::commands { class Registry; }

namespace coop::permissions::gameplay_nodes {

struct Entry {
    const char* node;
    const char* description;
    bool        defaultGranted;
};

// The tool gun's remover: a player may remove a prop with it. Guards RemoverRequest on the host.
inline constexpr Entry kToolgunRemove{"multivoid.gameplay.toolgun.remove", "Remove props with the tool gun's remover.",
                                      true};

// Declares every node of the domain. False when a declaration is refused (one already declared).
bool Install(coop::commands::Registry& reg);

}  // namespace coop::permissions::gameplay_nodes
