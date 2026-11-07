// ue_wrap/core/class_reask.cpp -- see ue_wrap/core/class_reask.h.

#include "ue_wrap/core/class_reask.h"

#include "ue_wrap/core/object_index.h"

#include <chrono>

namespace ue_wrap::class_reask {
namespace {

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

bool Latch::Due() {
    const uint64_t set = object_index::ClassSetVersion();
    if (!opened_ || set != setAsked_) {
        opened_ = true;
        open_ = true;
        setAsked_ = set;
        windowEndMs_ = NowMs() + kLoadWindowMs;
        return true;
    }
    if (!open_) return false;
    if (NowMs() < windowEndMs_) return true;
    open_ = false;
    return false;
}

void Latch::Reset() {
    opened_ = false;
    open_ = false;
}

}  // namespace ue_wrap::class_reask
