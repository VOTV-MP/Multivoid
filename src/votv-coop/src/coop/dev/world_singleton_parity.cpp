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

// The classes the tree keeps one of: the class and the instance are both compared.
const wchar_t* const kSingletons[] = {
    P::name::GamemodeClass, P::name::GameInstanceClass, P::name::DaynightCycleClass,
    P::name::DirectionalWindClass, P::name::PlayerCameraManagerClass,
    L"drone_C", L"laptop_C", L"wallunit_tapes_C",
};
// Classes with many instances, or none: only the class is compared, since "the first" differs by
// design between the index's list and the array's order.
const wchar_t* const kClasses[] = {
    L"Actor", L"PrimitiveComponent", L"saveSlot_C", L"kerfurOmega_C", L"prop_kerfurOmega_C",
    L"mainPlayer_C", L"actorChipPile_C", L"no_such_class_C",
};

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::world_singleton_parity);
    if (!s_on) return;
    static uint32_t s_doneGen = 0;
    const uint32_t gen = WI::Generation();
    if (gen == s_doneGen) return;
    if (WI::CurrentWorldKind() != WI::WorldKind::Gameplay) return;
    if (!ue_wrap::world_singleton::Gamemode()) return;   // the world's gamemode is up
    s_doneGen = gen;
    int ok = 0, n = 0;
    for (const wchar_t* name : kSingletons) {
        ++n;
        void* clsIndex = ue_wrap::object_index::ClassByName(name);
        void* clsWalk  = R::FindClass(name);
        void* instIndex = ue_wrap::world_singleton::Find(name);
        void* instWalk  = R::FindObjectByClass(name);
        // The walk hands out an instance marked for death; the singleton never does.
        const bool instOk = instIndex == instWalk || (!instIndex && instWalk && !R::IsLive(instWalk));
        const bool rowOk = clsIndex == clsWalk && instOk;
        if (rowOk) ++ok;
        UE_LOGI("ws_parity: %ls class index=%p walk=%p | instance singleton=%p walk=%p -- %s", name, clsIndex,
                clsWalk, instIndex, instWalk, rowOk ? "same" : "MISMATCH");
    }
    for (const wchar_t* name : kClasses) {
        ++n;
        void* clsIndex = ue_wrap::object_index::ClassByName(name);
        void* clsWalk  = R::FindClass(name);
        const bool rowOk = clsIndex == clsWalk;
        if (rowOk) ++ok;
        UE_LOGI("ws_parity: %ls class index=%p walk=%p -- %s", name, clsIndex, clsWalk, rowOk ? "same" : "MISMATCH");
    }
    UE_LOGI("ws_parity: VERDICT %s (%d/%d) world gen=%u", ok == n ? "PASS" : "FAIL", ok, n, gen);
}

}  // namespace coop::dev::world_singleton_parity
