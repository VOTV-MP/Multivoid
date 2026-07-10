#include "coop/player/nick_color.h"

#include "coop/comms/chat_feed.h"
#include "coop/player/players_registry.h"
#include "coop/session/player_handshake.h"
#include "coop/config/config.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"

#include <array>
#include <atomic>
#include <cstdio>

namespace coop::nick_color {

namespace {

// Per-slot packed colors (0 = default) + the local pref. ALL atomic: written by
// game-thread wire handlers, the render-thread F1 picker path and the bringup-
// thread session-start reset (the coop::nameplate v94 thread shape).
std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<uint32_t>            g_local{0};
std::array<std::atomic<uint32_t>, coop::players::kMaxPeers> g_bySlot{};

// The local peer's CURRENT slot, mirrored here so the render-thread chat drawer
// can resolve "is this line MINE -> use the local pref" without touching the
// GT-only players Registry. Refreshed by Install() (called on the game thread
// from the per-tick subsystem installer). 0xFF = unknown (no session).
std::atomic<uint8_t> g_localSlot{0xFF};

void PersistLocal(uint32_t packed) {
    // WriteIniValue is thread-safe (atomic swap) -- callable from the render
    // thread, same as the nameplate pref. Empty value = "no custom color".
    if (!IsCustom(packed)) {
        coop::config::WriteIniValue("nick_color", "");
        return;
    }
    char v[8];
    std::snprintf(v, sizeof(v), "%02X%02X%02X", R(packed), G(packed), B(packed));
    coop::config::WriteIniValue("nick_color", v);
}

}  // namespace

void SetInitialLocal(uint32_t packed) {
    g_local.store(packed, std::memory_order_relaxed);
}

uint32_t LocalPacked() {
    return g_local.load(std::memory_order_relaxed);
}

void RequestLocal(uint32_t packed) {
    // Render thread (the F1 picker). Persist NOW; state + announce hop to the
    // game thread -- the RequestSkin/RequestLocalVisible discipline.
    PersistLocal(packed);
    ue_wrap::game_thread::Post([packed] {
        g_local.store(packed, std::memory_order_relaxed);
        const uint8_t selfSlot = coop::players::Registry::Get().LocalPeerId();
        if (selfSlot < coop::players::kMaxPeers)
            g_bySlot[selfSlot].store(packed, std::memory_order_relaxed);
        UE_LOGI("nick_color: local nick color -> %s (persisted; announcing)",
                IsCustom(packed) ? "CUSTOM" : "DEFAULT");
        if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
            coop::player_handshake::AnnounceLocalNickColor(*s, packed);
        coop::chat_feed::Push(IsCustom(packed) ? L"Nickname color: applied (synced to other players)"
                                               : L"Nickname color: reset to default");
    });
}

void StoreForSlot(int slot, uint32_t packed) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (g_bySlot[slot].exchange(packed, std::memory_order_relaxed) != packed)
        UE_LOGI("nick_color: slot %d nick color -> %s", slot,
                IsCustom(packed) ? "CUSTOM" : "DEFAULT");
}

uint32_t PackedForSlot(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return 0;
    const uint32_t stored = g_bySlot[slot].load(std::memory_order_relaxed);
    if (stored != 0) return stored;
    // Our own slot never receives its color over the wire (we ARE the origin) --
    // fall through to the local pref so our own chat lines / scoreboard row
    // agree with what every other peer renders for us.
    if (slot == g_localSlot.load(std::memory_order_relaxed))
        return g_local.load(std::memory_order_relaxed);
    return 0;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Game thread (per-tick subsystem installer) -- mirror the registry's local
    // slot for the render-thread readers. Idempotent store.
    g_localSlot.store(coop::players::Registry::Get().LocalPeerId(),
                      std::memory_order_relaxed);
}

void ResetSlots() {
    // Session-start reset -- runs on the BRINGUP thread (TimelineThread) via
    // player_handshake::Reset; the store is atomic, so no game-thread hop.
    for (auto& c : g_bySlot) c.store(0, std::memory_order_relaxed);
}

void OnSlotDisconnected(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // A reused slot must not inherit the departed peer's color.
    g_bySlot[slot].store(0, std::memory_order_relaxed);
}

}  // namespace coop::nick_color
