#include "coop/comms/chat_bubbles.h"

#include "coop/player/players_registry.h"  // kMaxPeers

#include <windows.h>

#include <cstring>
#include <mutex>

namespace coop::chat_bubbles {

namespace {

struct Bubble {
    char     text[kMaxBubbleBytes] = {};
    uint64_t bornMs = 0;  // 0 = none
};

std::mutex g_mu;
Bubble g_bySlot[coop::players::kMaxPeers];

}  // namespace

void OnChatLine(int slot, const char* utf8Text) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!utf8Text || !utf8Text[0]) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Bubble& b = g_bySlot[slot];
    size_t i = 0;
    for (; i + 1 < sizeof(b.text) && utf8Text[i]; ++i) b.text[i] = utf8Text[i];
    b.text[i] = '\0';
    b.bornMs = ::GetTickCount64();
}

float BubbleForSlot(int slot, char* out) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return 0.f;
    std::lock_guard<std::mutex> lk(g_mu);
    Bubble& b = g_bySlot[slot];
    if (b.bornMs == 0) return 0.f;
    const uint64_t age = ::GetTickCount64() - b.bornMs;
    if (age >= kHoldMs) {
        b.bornMs = 0;  // expire in place (no separate tick needed)
        return 0.f;
    }
    std::memcpy(out, b.text, sizeof(b.text));
    const uint64_t fadeStart = kHoldMs - kFadeMs;
    if (age <= fadeStart) return 1.f;
    return 1.f - static_cast<float>(age - fadeStart) / static_cast<float>(kFadeMs);
}

void ResetSlots() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& b : g_bySlot) { b.text[0] = '\0'; b.bornMs = 0; }
}

void OnSlotDisconnected(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_bySlot[slot].text[0] = '\0';
    g_bySlot[slot].bornMs = 0;
}

}  // namespace coop::chat_bubbles
