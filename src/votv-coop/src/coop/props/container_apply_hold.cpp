// coop/props/container_apply_hold.cpp -- see coop/props/container_apply_hold.h.

#include "coop/props/container_apply_hold.h"

#include <utility>

namespace coop::props::container_apply_hold {
namespace {

struct HeldBlob {
    uint8_t slot;
    std::vector<uint8_t> bytes;
};
bool g_holding = false;
uint32_t g_eid = 0;
std::vector<HeldBlob> g_kept;
void (*g_apply)(const std::vector<uint8_t>&, uint8_t) = nullptr;

}  // namespace

void SetApply(void (*apply)(const std::vector<uint8_t>& blob, uint8_t senderSlot)) { g_apply = apply; }

void Hold(uint32_t eid) {
    g_holding = true;
    g_eid = eid;
}

bool Armed() { return g_holding; }

void Release() {
    g_holding = false;
    std::vector<HeldBlob> kept;
    kept.swap(g_kept);
    if (!g_apply) return;
    for (const HeldBlob& b : kept) g_apply(b.bytes, b.slot);
}

bool Keep(uint32_t eid, std::vector<uint8_t>&& blob, uint8_t senderSlot) {
    if (!g_holding || eid != g_eid) return false;
    g_kept.push_back(HeldBlob{senderSlot, std::move(blob)});
    return true;
}

void Reset() {
    g_holding = false;
    g_eid = 0;
    g_kept.clear();
}

}  // namespace coop::props::container_apply_hold
