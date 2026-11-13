// coop/player/puppet_hide.cpp -- see coop/player/puppet_hide.h.

#include "coop/player/puppet_hide.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

namespace coop::puppet_hide {
namespace {

constexpr int kSlots = coop::players::kMaxPeers;

uint8_t g_reasons[kSlots] = {};         // bit set of Reason per slot
bool g_hidden[kSlots] = {};             // the flag as last written on g_body
void* g_body[kSlots] = {};              // the body the latch describes: compared, never dereferenced

const char* Why(uint8_t reasons) {
    if ((reasons & static_cast<uint8_t>(Reason::Dream)) && (reasons & static_cast<uint8_t>(Reason::Visit)))
        return "that player is in a nightmare and in another map";
    if (reasons & static_cast<uint8_t>(Reason::Dream)) return "that player is in a nightmare";
    if (reasons & static_cast<uint8_t>(Reason::Visit)) return "that player is in another map";
    return "that player is back";
}

// Writes the flag when the answer, or the body, differs from what was last written. A body that is new starts shown.
void Apply(int slot) {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* actor = (p && p->valid()) ? p->actor() : nullptr;
    if (!actor) {
        g_body[slot] = nullptr;
        g_hidden[slot] = false;
        return;
    }
    if (actor != g_body[slot]) {
        g_body[slot] = actor;
        g_hidden[slot] = false;
    }
    const bool want = g_reasons[slot] != 0;
    if (want == g_hidden[slot]) return;
    ue_wrap::engine::SetActorHiddenInGame(actor, want);
    g_hidden[slot] = want;
    UE_LOGI("puppet_hide: slot %d's copy %s -- %s", slot, want ? "hidden" : "shown", Why(g_reasons[slot]));
}

}  // namespace

void Set(uint8_t slot, Reason reason, bool on) {
    UE_ASSERT_GAME_THREAD("puppet_hide::Set");
    if (slot >= kSlots) return;
    const uint8_t bit = static_cast<uint8_t>(reason);
    g_reasons[slot] = static_cast<uint8_t>(on ? (g_reasons[slot] | bit) : (g_reasons[slot] & ~bit));
    Apply(slot);
}

void OnPuppetSpawned(uint8_t slot) {
    UE_ASSERT_GAME_THREAD("puppet_hide::OnPuppetSpawned");
    if (slot >= kSlots) return;
    g_body[slot] = nullptr;  // a new body starts shown, whatever the latch said of the last
    g_hidden[slot] = false;
    Apply(slot);
}

void Forget(uint8_t slot) {
    if (slot >= kSlots) return;
    g_reasons[slot] = 0;
    g_body[slot] = nullptr;
    g_hidden[slot] = false;
}

void OnDisconnect() {
    for (int slot = 0; slot < kSlots; ++slot) {
        g_reasons[slot] = 0;
        Apply(slot);
        g_body[slot] = nullptr;
        g_hidden[slot] = false;
    }
}

}  // namespace coop::puppet_hide
