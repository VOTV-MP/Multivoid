// coop/dev/container_intent_drill_put2.cpp -- the container intent drill's put arms that need a red or an interleave,
// the CLIENT's half and the reds of both peers; see container_intent_drill_put.cpp for the arms they share a machine
// with. A red is a dev row beside container_act_no_escrow, read once and armed on the peer it breaks.

#include "container_intent_drill_put.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace AR = coop::props::container_act_request;
namespace AH = coop::props::container_act_host;

}  // namespace

void PutArmClientKnobs() {
    static const bool noHover = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_hover_guard);
    static const bool noTable = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_hand_table);
    if (noHover) AR::dev::NoHoverGuard(true);
    if (noTable) AR::dev::NoHandTable(true);
}

void PutArmHostKnobs() {
    static const bool noReservation = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_reservation);
    static const bool noLapse = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_lapse);
    static const bool notReady = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_insert_notready);
    if (noReservation) AH::dev::NoReservation(true);
    if (noLapse) AH::dev::NoLapse(true);
    if (notReady) AH::dev::InsertNotReady(true);
}

}  // namespace coop::dev::container_intent_drill::detail
