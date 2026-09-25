// coop/dev/world_singleton_parity.cpp -- [dev] the index's lookups against the walks. See the header.
#include "coop/dev/world_singleton_parity.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace coop::dev::world_singleton_parity {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace WI = ue_wrap::world_identity;
namespace WS = ue_wrap::world_singleton;

// The classes the tree keeps one of: the class and the instance are both compared.
const wchar_t* const kSingletons[] = {
    P::name::GamemodeClass, P::name::GameInstanceClass, P::name::DaynightCycleClass,
    P::name::DirectionalWindClass, P::name::PlayerCameraManagerClass, P::name::WorldClass,
    L"drone_C", L"laptop_C", L"wallunit_tapes_C",
};
// Classes with many instances, or none: only the class is compared, since "the first" differs by
// design between the index's list and the array's order.
const wchar_t* const kClasses[] = {
    L"Actor", L"PrimitiveComponent", L"saveSlot_C", L"kerfurOmega_C", L"prop_kerfurOmega_C",
    L"mainPlayer_C", L"actorChipPile_C", L"no_such_class_C",
};

// What the walk would hand out under the singleton's own rule: the first instance in array order
// that is live and readable, not a default object and, when it has a world, of the running one.
// Array order and the index's newest-first order agree whenever a world holds one of the class.
void* WalkUnderTheRule(const wchar_t* name) {
    void* const world = WI::CurrentWorld();
    for (void* obj : R::FindObjectsByClass(name)) {
        const int32_t idx = R::InternalIndexOf(obj);
        if (R::SlotFlags(idx) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) continue;
        void* const w = WI::WorldOf(obj);
        if (w && world && w != world) continue;
        return obj;
    }
    return nullptr;
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::world_singleton_parity);
    if (!s_on) return;
    static uint32_t s_doneGen = 0;
    const uint32_t gen = WI::Generation();
    if (gen == s_doneGen) return;
    // Once per world; a gameplay world is compared once its gamemode is up.
    const WI::WorldKind kind = WI::CurrentWorldKind();
    if (kind == WI::WorldKind::Unknown) return;
    if (kind == WI::WorldKind::Gameplay && !WS::Gamemode()) return;
    s_doneGen = gen;
    int ok = 0, n = 0, bothNull = 0;
    auto judge = [&](const wchar_t* what, void* index, void* walk) {
        ++n;
        const bool same = index == walk;
        if (same) ++ok;
        if (same && !index) ++bothNull;
        UE_LOGI("ws_parity: %ls index=%p walk=%p -- %s", what, index, walk,
                !same ? "MISMATCH" : index ? "same" : "same (both null)");
    };
    for (const wchar_t* name : kSingletons) {
        judge(name, ue_wrap::object_index::ClassByName(name), R::FindClass(name));
        judge(name, WS::Find(name), WalkUnderTheRule(name));
    }
    // The named accessors hold their own references, which the ~50 call sites read.
    judge(L"Gamemode()", WS::Gamemode(), WalkUnderTheRule(P::name::GamemodeClass));
    judge(L"GameInstance()", WS::GameInstance(), WalkUnderTheRule(P::name::GameInstanceClass));
    for (const wchar_t* name : kClasses)
        judge(name, ue_wrap::object_index::ClassByName(name), R::FindClass(name));
    UE_LOGI("ws_parity: VERDICT %s (%d/%d, %d both null) world gen=%u kind=%d", ok == n ? "PASS" : "FAIL", ok, n,
            bothNull, gen, static_cast<int>(kind));
}

}  // namespace coop::dev::world_singleton_parity
