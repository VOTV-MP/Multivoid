// coop/permissions/gameplay_nodes.cpp -- see coop/permissions/gameplay_nodes.h.

#include "coop/permissions/gameplay_nodes.h"

#include "coop/commands/command_registry.h"

namespace coop::permissions::gameplay_nodes {

bool Install(coop::commands::Registry& reg) {
    constexpr const Entry* kAll[] = {&kToolgunRemove};
    for (const Entry* e : kAll) {
        if (!reg.DeclareNode({e->node, e->defaultGranted, e->description}, nullptr)) return false;
    }
    return true;
}

}  // namespace coop::permissions::gameplay_nodes
