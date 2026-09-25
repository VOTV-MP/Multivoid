// coop/dev/function_lookup_parity.cpp -- [dev] FindFunction against the walk it replaced. See the header.
#include "coop/dev/function_lookup_parity.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <string>

namespace coop::dev::function_lookup_parity {
namespace {

namespace R  = ue_wrap::reflection;
namespace WI = ue_wrap::world_identity;

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::function_lookup_parity);
    if (!s_on) return;
    static uint32_t s_doneGen = 0;
    const uint32_t gen = WI::Generation();
    if (gen == s_doneGen) return;
    // Once per world, when the index holds the world's load, so a class the load brought is compared too.
    if (WI::CurrentWorldKind() == WI::WorldKind::Unknown) return;
    if (ue_wrap::object_index::Backlog() != 0) return;
    s_doneGen = gen;
    void* classCls = R::FindClass(L"Class");
    if (!classCls) {
        UE_LOGW("fn_parity: VERDICT FAIL (the Class class did not resolve; nothing was judged) world gen=%u", gen);
        return;
    }
    int functions = 0, same = 0, shown = 0;
    for (void* fn : R::FindObjectsByClass(L"Function")) {
        const int32_t idx = R::InternalIndexOf(fn);
        if (R::SlotFlags(idx) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) continue;
        // A function a class owns: its outer is the class, whose own class descends from Class (a Blueprint's
        // generated class included).
        void* const owner = R::OuterOf(fn);
        if (!owner || !R::IsDescendantOfAny(R::ClassOf(owner), &classCls, 1)) continue;
        ++functions;
        const std::wstring name = R::ToString(R::NameOf(fn));
        void* const listed = R::FindFunction(owner, name.c_str());
        if (listed == fn) { ++same; continue; }
        if (shown++ < 20)
            UE_LOGI("fn_parity: %ls.%ls walk=%p list=%p -- MISMATCH", R::ToString(R::NameOf(owner)).c_str(),
                    name.c_str(), fn, listed);
    }
    UE_LOGI("fn_parity: VERDICT %s (%d/%d functions a class owns) world gen=%u", same == functions ? "PASS" : "FAIL",
            same, functions, gen);
}

}  // namespace coop::dev::function_lookup_parity
