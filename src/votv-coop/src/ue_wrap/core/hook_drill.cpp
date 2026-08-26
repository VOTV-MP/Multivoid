#include "ue_wrap/core/hook_drill.h"

#include "ue_wrap/core/log.h"

#include <windows.h>

namespace ue_wrap::hook_drill {
namespace {

constexpr int kMaxSlots = 8;

bool Armed() {
    static int s_armed = -1;
    if (s_armed < 0) {
        char v[8]{};
        s_armed = (::GetEnvironmentVariableA("VOTVCOOP_TRAMPOLINE_DRILL", v, sizeof(v)) > 0 &&
                   v[0] == '1') ? 1 : 0;
    }
    return s_armed == 1;
}

struct Baseline {
    unsigned long long bytes = 0;
    bool have = false;
};
Baseline g_baseline[kMaxSlots];

}  // namespace

void SampleTrampoline(const char* when, int slot, const void* trampoline) {
    if (!Armed()) return;
    if (slot < 0 || slot >= kMaxSlots) {
        UE_LOGW("tramp_drill[%s]: slot %d out of range -- not sampled", when, slot);
        return;
    }
    const auto* p = static_cast<const unsigned char*>(trampoline);
    if (!p) {
        UE_LOGW("tramp_drill[%s] slot=%d: no trampoline pointer -- nothing to sample",
                when, slot);
        return;
    }

    // Read through a volatile view. The question is what the bytes ARE at this
    // instant; a compiler that hoisted the load across the teardown call it brackets
    // would defeat the entire drill.
    unsigned long long now = 0;
    for (int i = 0; i < 8; ++i) {
        const auto b = *static_cast<const volatile unsigned char*>(p + i);
        now |= static_cast<unsigned long long>(b) << (i * 8);
    }

    Baseline& b = g_baseline[slot];
    if (!b.have) {
        b.bytes = now;
        b.have = true;
        UE_LOGW("tramp_drill[%s] slot=%d: trampoline %p first8=%016llX (baseline)",
                when, slot, trampoline, now);
        ue_wrap::log::Flush();
        return;
    }

    const bool intact = (now == b.bytes);
    UE_LOGW("tramp_drill[%s] slot=%d: trampoline %p first8=%016llX (was %016llX) -- %s",
            when, slot, trampoline, now, b.bytes,
            intact ? "INTACT: slot survived the teardown (GREEN)"
                   : "CORRUPTED: slot freed under a live pointer (RED)");
    ue_wrap::log::Flush();
}

}  // namespace ue_wrap::hook_drill
