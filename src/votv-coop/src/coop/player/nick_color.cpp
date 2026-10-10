#include "coop/player/nick_color.h"

#include "coop/comms/chat_feed.h"
#include "coop/player/players_registry.h"
#include "coop/session/player_handshake.h"
#include "coop/config/config.h"
#include "l10n/l10n.h"
#include "ue_wrap/core/log.h"

#include <array>
#include <atomic>
#include <cstdio>

namespace coop::nick_color {

namespace {

// Per-slot packed colors (0 = default) + the local pref. ALL atomic: written by
// game-thread wire handlers and the row's subscriber, and by the bringup-thread
// session-start reset.
std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<uint32_t>            g_local{0};
std::array<std::atomic<uint32_t>, coop::players::kMaxPeers> g_bySlot{};

// The local peer's CURRENT slot, mirrored here so the render-thread chat drawer
// can resolve "is this line MINE -> use the local pref" without touching the
// GT-only players Registry. Refreshed by Install() (called on the game thread
// from the per-tick subsystem installer). 0xFF = unknown (no session).
std::atomic<uint8_t> g_localSlot{0xFF};

// The `nick_color` row's text, as the row stores it. "unset" (the key ABSENT:
// a new identity, or a reset) = custom WHITE; an explicitly EMPTY value = the
// per-surface defaults; a 6-digit RRGGBB hex = that colour, and a malformed one
// falls back to the defaults.
uint32_t PackedFromIniText(const std::string& text) {
    uint32_t packed = 0;
    if (text == "unset") {
        packed = Pack(255, 255, 255);
    } else if (text.size() == 6) {
        unsigned rgb = 0;
        bool ok = true;
        for (char c : text) {
            rgb <<= 4;
            if (c >= '0' && c <= '9')      rgb |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') rgb |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') rgb |= static_cast<unsigned>(c - 'A' + 10);
            else { ok = false; break; }
        }
        if (ok)
            packed = Pack(static_cast<uint8_t>(rgb >> 16),
                          static_cast<uint8_t>(rgb >> 8),
                          static_cast<uint8_t>(rgb));
    }
    return packed;
}

// The `nick_color` row changed (a pane's SetValue, a ResetValue, a review keep-line): re-resolve
// it, apply it locally and announce it to the session (host: broadcast; client: to host for
// rebroadcast). Game thread -- the config notifier runs every subscriber there.
void OnNickColorRowChanged() {
    const uint32_t packed =
        PackedFromIniText(coop::config::ResolveString(::coop::config_registry::rows::nick_color));
    g_local.store(packed, std::memory_order_relaxed);
    const uint8_t selfSlot = coop::players::Registry::Get().LocalPeerId();
    if (selfSlot < coop::players::kMaxPeers)
        g_bySlot[selfSlot].store(packed, std::memory_order_relaxed);
    UE_LOGI("nick_color: local nick color -> %s (row applied; announcing)",
            IsCustom(packed) ? "CUSTOM" : "DEFAULT");
    if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
        coop::player_handshake::AnnounceLocalNickColor(*s, packed);
    // Transient: this player's own UI confirmation, not the lobby's record.
    coop::chat_feed::Push(
        std::string(IsCustom(packed) ? l10n::T("Nickname color: applied (synced to other players)")
                                     : l10n::T("Nickname color: reset to default")),
        coop::chat_feed::Keep::Transient);
}

}  // namespace

std::string IniTextFor(uint32_t packed) {
    if (!IsCustom(packed)) return std::string();
    char v[8];
    std::snprintf(v, sizeof(v), "%02X%02X%02X", R(packed), G(packed), B(packed));
    return std::string(v);
}

void SubscribeRow() {
    coop::config::Subscribe(::coop::config_registry::rows::nick_color, &OnNickColorRowChanged);
}

void SetInitialLocalFromIniHex(const std::string& hex) {
    SetInitialLocal(PackedFromIniText(hex));
}

void SetInitialLocal(uint32_t packed) {
    g_local.store(packed, std::memory_order_relaxed);
}

uint32_t LocalPacked() {
    return g_local.load(std::memory_order_relaxed);
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
    // Session-start reset, from event_feed::OnSessionStart on the BRINGUP thread;
    // the stores are atomic, so no game-thread hop. Not occupancy-gated, unlike
    // OnSlotDisconnected: a peer's colour arrives with its Join, before its roster
    // row exists, so a slot that never became occupied would otherwise carry that
    // colour into the next session.
    for (auto& c : g_bySlot) c.store(0, std::memory_order_relaxed);
}

void OnSlotDisconnected(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // From the ledger's occupant-change edge, which fires on a replacement too, so a
    // reused slot never inherits the departed peer's color.
    g_bySlot[slot].store(0, std::memory_order_relaxed);
}

}  // namespace coop::nick_color
