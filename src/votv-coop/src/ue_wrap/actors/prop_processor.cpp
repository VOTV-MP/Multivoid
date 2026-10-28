// ue_wrap/actors/prop_processor.cpp -- see ue_wrap/actors/prop_processor.h.

#include "ue_wrap/actors/prop_processor.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::prop_processor {
namespace {

namespace R = ue_wrap::reflection;

constexpr int kPhases = 3;  // fins increments: signals, audio import, the post-broadcast one

int32_t g_offFins = -2;  // propProcessor_C.fins

}  // namespace

void* WorldPropProcessor() { return ue_wrap::world_singleton::Find(L"propProcessor_C"); }

int Passes(void* processor) {
    if (!processor) return -1;
    if (g_offFins == -2) {
        g_offFins = R::FindPropertyOffset(R::ClassOf(processor), L"fins");
        if (g_offFins < 0) UE_LOGW("prop_processor: could not resolve fins -- the world-settle test is inert");
    }
    if (g_offFins < 0) return -1;
    int32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(processor) + g_offFins, sizeof v);
    return v;
}

bool Finished(void* processor) { return Passes(processor) >= kPhases; }

}  // namespace ue_wrap::prop_processor
