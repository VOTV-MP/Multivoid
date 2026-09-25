// coop/net/origin_context.cpp -- see coop/net/origin_context.h.
#include "coop/net/origin_context.h"

namespace coop::net {

namespace {

bool Valid(int slot) { return slot >= 0 && slot < OriginContext::kSlots; }

}  // namespace

std::uint8_t OriginContext::Admit(int slot) {
    if (!Valid(slot)) return 0;
    std::uint8_t next = static_cast<std::uint8_t>(hostLast_[slot].load(std::memory_order_relaxed) + 1);
    if (next == 0) next = 1;
    hostLast_[slot].store(next, std::memory_order_release);
    return next;
}

std::uint8_t OriginContext::Stamp(int slot) const {
    return Valid(slot) ? hostLast_[slot].load(std::memory_order_acquire) : 0;
}

bool OriginContext::Accepts(int slot, std::uint8_t ctx) const {
    if (!Valid(slot) || ctx == 0) return true;
    const std::uint8_t latched = latched_[slot];
    if (latched == 0) return true;
    return ctx == latched && !retired_[slot];
}

bool OriginContext::Conform(int slot, std::uint8_t ctx) {
    if (!Valid(slot) || ctx == 0) return false;
    const bool replaces = latched_[slot] != 0 && latched_[slot] != ctx;
    latched_[slot] = ctx;
    retired_[slot] = false;
    return replaces;
}

bool OriginContext::Retire(int slot, std::uint8_t ctx) {
    if (!Valid(slot)) return false;
    // A row with no context says only that the slot emptied: what is latched is what left.
    if (ctx != 0) latched_[slot] = ctx;
    retired_[slot] = latched_[slot] != 0;
    return true;
}

void OriginContext::Clear(int slot) {
    if (!Valid(slot)) return;
    latched_[slot] = 0;
    retired_[slot] = false;
}

}  // namespace coop::net
