// coop/dev/container_poke.cpp -- see coop/dev/container_poke.h.

#include "coop/dev/container_poke.h"

#include "coop/props/container_contents_sync.h"

#include "ue_wrap/core/reflection.h"

namespace coop::dev::container_poke {

namespace R  = ue_wrap::reflection;
namespace CC = coop::props::container_contents_sync;

// The organic mutation: `Aprop_container_C::extract(int32 Index)`, the very verb a player's UI
// extraction runs. extract's first act is `propInventory->takeObj(index, false, ...)`, dispatched
// through EX_LocalVirtualFunction, so the call WE make is the outer one and the mutation the lane
// must catch is the game's own inner dispatch. Calling `takeObj` ourselves would arrive by
// ProcessEvent -- the one path the lane does NOT rely on -- and would prove nothing, since a probe
// must never resolve through the mechanism under test.
bool Extract(void* actor, int32_t& before, int32_t& after, uint32_t eid) {
    // The DISPATCH function for THIS actor's own class, resolved per fire and not cached in one
    // slot: FindFunction is exact-owner, so ClassOf(actor) -- always a subclass here -- yields
    // null through it, while resolving the base instead would run the base body on a player
    // inventory container, which overrides extract. The targets differ between fires, so one
    // cached pointer would carry the first target's body to every later one.
    void* extractFn = R::FindDispatchFunction(R::ClassOf(actor), L"extract", nullptr);
    if (!extractFn) return false;
    float vol = 0.f;
    before = -1;
    CC::ContentsDigest(eid, before, vol);
    struct { int32_t Index; } params{0};   // always the first record
    R::CallFunction(actor, extractFn, &params);
    after = -1;
    CC::ContentsDigest(eid, after, vol);
    return true;
}

}  // namespace coop::dev::container_poke
