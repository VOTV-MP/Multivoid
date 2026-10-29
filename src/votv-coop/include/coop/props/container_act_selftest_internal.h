// coop/props/container_act_selftest_internal.h -- what the container act selftest's two files share: the check counter
// and the builders of the records, inputs and holds the checks start from. Not an API. Pure: no engine call.

#pragma once

#include "coop/props/container_act_host.h"
#include "coop/props/container_act_state.h"

#include "ue_wrap/actors/save_record.h"

#include <cstdint>

namespace coop::props::container_act_host::selftest {

// One check: counted, and its failure said.
struct Checks {
    int pass = 0;
    int total = 0;
    void operator()(bool ok, const char* what);
};

ue_wrap::save_record::SaveRecord Rec(const wchar_t* cls, int32_t n, const wchar_t* text);

// Every read passes: the grant. A check breaks one field to reach one branch.
internal::Inputs Ready();

container_act_state::Hold MakeHold(container_act_state::HoldKind kind, uint8_t slot, uint16_t id, uint32_t eid,
                                   uint64_t grantedMs);

// The put's checks (container_act_selftest_put.cpp).
void RunPutSelftest(Checks& check);

// The overlap put's checks (container_act_selftest_overlap.cpp).
void RunOverlapSelftest(Checks& check);

}  // namespace coop::props::container_act_host::selftest
