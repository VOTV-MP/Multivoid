// coop/save/save_block.cpp -- see coop/save/save_block.h.

#include "coop/save/save_block.h"

#include "coop/net/session.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/engine/save_to_slot_hook.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <cstdint>

namespace coop::save_block {

namespace R = ue_wrap::reflection;

namespace {

// The session whose role the gate reads at the moment a save fires.
std::atomic<coop::net::Session*> g_session{nullptr};

// --- Part 3 state: the native save-cycle gate (see the header) ---
// The gamemode disableSave is held on, by slot, serial and world, taken from the world singleton
// again once that fails. disableSave is a BP BoolProperty -- resolved via FindBoolProperty (real
// byte+mask; never a raw whole-byte guess).
ue_wrap::CachedObjRef g_gm;
int32_t g_disableSaveOff = -1;        // disableSave byte offset within the gamemode
uint8_t g_disableSaveMask = 0;        // ...and its bit mask inside that byte

void LogBlockedSave(const wchar_t* slot) {
    // Throttle: the first few blocks + every 20th. Autosave fires every few
    // minutes so this is naturally sparse, but a forced-save loop shouldn't
    // be able to flood the log.
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 3 && (n % 20) != 0) return;
    UE_LOGI("save_block: BLOCKED client world-save to slot '%ls' (block #%llu) -- host-only "
            "persistence; client save left untouched", slot, static_cast<unsigned long long>(n));
}

// The world a client plays in is a mirror of the host's, and it can outlive the session that
// built it: the session is stopped BEFORE the trip to the menu, and a trip that fails leaves the
// mirror loaded. `g_gm` is that world's gamemode (the one Tick holds disableSave on), so "the
// mirror is what is loaded" is: it is still live, and it belongs to the current world.
bool MirrorWorldIsCurrent() {
    if (!g_gm.Get()) return false;
    void* const now = ue_wrap::world_identity::CurrentWorld();
    return now && g_gm.StampedWorld() == now;
}

// The gate on the engine's save function (ue_wrap/engine/save_to_slot_hook), asked for every world
// save in this process. Decided HERE, when the save fires: shut for a client in a running session,
// and shut for as long as a mirror world is what is loaded; open otherwise, so a process that was
// a client and has left plays single-player again and saves. The save cycle is held off on a
// mirror's gamemode for good, but not every save goes through the cycle (a trigger and a
// player-only save call the engine directly), so the cycle block alone is not the belt.
// Cancelling returns false to the Blueprint save flow -- "the save did not happen", which is
// honest -- and the on-disk .sav is never opened, so the client's pre-coop world save is preserved
// byte for byte.
bool WorldSaveGate(void* /*saveObject*/, const wchar_t* slotName) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    const bool client = s && s->running() && s->role() == coop::net::Role::Client;
    if (!client && !MirrorWorldIsCurrent()) return true;
    LogBlockedSave(slotName);
    return false;
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!session) return;  // session not ready yet -- retry next pump tick
    g_session.store(session, std::memory_order_release);
    // The gate first, so the detour never arms without it; the hook's own install retries until
    // saveSlot_C is loaded and is a cheap latch after that.
    ue_wrap::save_to_slot_hook::SetGate(&WorldSaveGate);
    ue_wrap::save_to_slot_hook::Install();
}

void Tick(coop::net::Session* session) {
    if (!session || !session->running() || session->role() != coop::net::Role::Client) return;

    // Steady state: cached live gamemode -> one masked-bit read, write only on the
    // false->true edge (the bit is ours to hold: no bytecode ever writes it).
    if (!g_gm.Get()) {
        // No gamemode (menu / join window / world change) is one lookup in the world singleton,
        // never a walk. A class without the field does not grow it, so its absence is said once
        // and the gate stays open from then on.
        void* gm = ue_wrap::world_singleton::Gamemode();
        if (!gm) return;
        if (g_disableSaveOff < 0) {
            static bool sNoField = false;
            if (sNoField) return;
            if (!R::FindBoolProperty(R::ClassOf(gm), L"disableSave", g_disableSaveOff, g_disableSaveMask)) {
                sNoField = true;
                UE_LOGE("save_block: mainGamemode_C.disableSave did not resolve -- native "
                        "save-cycle gate NOT held (disk write-block still active)");
                return;
            }
        }
        g_gm.Set(gm);
    }

    auto* p = reinterpret_cast<uint8_t*>(g_gm.Raw()) + g_disableSaveOff;
    if ((*p & g_disableSaveMask) == 0) {
        *p |= g_disableSaveMask;
        UE_LOGI("save_block: client native save cycle OFF -- disableSave=true on gamemode %p "
                "(+0x%X mask 0x%02X; saveSlot_C::save gates gather+write on it, disk hook "
                "stays as the belt)",
                g_gm.Raw(), static_cast<unsigned>(g_disableSaveOff), g_disableSaveMask);
    }
}

}  // namespace coop::save_block
