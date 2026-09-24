// coop/dev/recycled_slot_drill.cpp -- see coop/dev/recycled_slot_drill.h.

#include "coop/dev/recycled_slot_drill.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstring>

namespace coop::dev::recycled_slot_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

bool g_done = false;
bool g_waitSaid = false;

bool Lists(void* cls, void* obj) {
    struct Ctx { void* obj; bool hit; } ctx{obj, false};
    OI::ForEachInstance(cls, [](void* c, void* o, int32_t) {
        auto* x = static_cast<Ctx*>(c);
        if (o == x->obj) x->hit = true;
    }, &ctx);
    return ctx.hit;
}

// Whether the slot lists exactly `obj` under `cls`, read raw, past every hand-out check.
bool SlotLists(int32_t idx, void* obj, void* cls) {
    void* o = nullptr;
    void* c = nullptr;
    return OI::DebugSlotListing(idx, &o, &c) && o == obj && c == cls;
}

// The component back under its own class, as its own birth lists it.
bool Restore(void* comp, void* compCls, int32_t idx) {
    OI::DebugUnlinkSlot(idx);
    OI::DebugApplyCreate(comp, compCls, idx);
    return SlotLists(idx, comp, compCls);
}

void RunIndexHalf(void* pawnCls, void* comp, void* compCls, int32_t idx) {
    // A birth queued for the slot's old tenant, applied after the component took the slot and the
    // address: the slot holds the pointer the event names, under another class.
    OI::DebugUnlinkSlot(idx);
    OI::DebugApplyCreate(comp, pawnCls, idx);
    const bool linked = SlotLists(idx, comp, pawnCls);
    const bool back1 = Restore(comp, compCls, idx);
    // A listing whose tenant changed while its death and the next birth both waited in the queue.
    OI::DebugListUnchecked(comp, pawnCls, idx);
    const bool handed = Lists(pawnCls, comp);
    const bool back2 = Restore(comp, compCls, idx);
    UE_LOGI("[RECYCLED-SLOT] index: a birth naming the slot's old class listed the component=%d, a "
            "stale listing handed it out=%d, its own listing restored=%d/%d -- %s",
            linked ? 1 : 0, handed ? 1 : 0, back1 ? 1 : 0, back2 ? 1 : 0,
            (!linked && !handed && back1 && back2) ? "PASS" : "RED");
}

void RunDispatchHalf(void* comp, int32_t rootOff) {
    // The actor read loads the receiver's root-component slot and reads through it. A component
    // keeps other fields at that offset; a non-canonical value there faults, which is the case under
    // test. A null or a canonical value may not fault, and the half says so rather than judge.
    uint64_t v = 0;
    std::memcpy(&v, static_cast<const uint8_t*>(comp) + rootOff, sizeof v);
    const uint64_t top = v >> 47;
    if (v == 0 || top == 0 || top == 0x1FFFF) {
        UE_LOGI("[RECYCLED-SLOT] dispatch: the component's +0x%X holds 0x%016llX, a canonical value the "
                "read may not fault on -- INVALID", rootOff, static_cast<unsigned long long>(v));
        return;
    }
    ue_wrap::FVector loc{};
    const uint32_t before = ue_wrap::game_thread::AbsorbedFaultsOnThisThread();
    const bool ok = ue_wrap::engine::TryGetActorLocation(comp, loc);
    const uint32_t faults = ue_wrap::game_thread::AbsorbedFaultsOnThisThread() - before;
    const char* verdict = ok ? "RED" : (faults == 1 ? "PASS" : "INVALID (failed without one fault)");
    UE_LOGI("[RECYCLED-SLOT] dispatch: the actor location read on a '%ls' (its +0x%X holds 0x%016llX) "
            "returned %d at (%.1f, %.1f, %.1f), faults absorbed %u -- %s", R::ClassNameOf(comp).c_str(),
            rootOff, static_cast<unsigned long long>(v), ok ? 1 : 0, loc.X, loc.Y, loc.Z, faults, verdict);
}

}  // namespace

bool IsEnabled() {
    static const bool on = ::coop::config::ResolveFlag(::coop::config_registry::rows::recycled_slot_drill);
    return on;
}

void Tick() {
    if (g_done || !IsEnabled()) return;
    void* const pawn = coop::players::Registry::Get().Local();
    if (!pawn || !OI::IsSeeded()) return;
    void* const pawnCls = R::ClassOf(pawn);
    const int32_t rootOff = R::FindPropertyOffset(pawnCls, L"RootComponent");
    void* const comp = rootOff >= 0
        ? *reinterpret_cast<void**>(static_cast<uint8_t*>(pawn) + rootOff) : nullptr;
    if (!comp || !R::IsLive(comp)) return;
    void* const compCls = R::ClassOf(comp);
    const int32_t idx = R::InternalIndexOf(comp);
    // Ready once the index lists the component under its own class and nowhere else: the drill
    // stages from that state and restores to it.
    if (!SlotLists(idx, comp, compCls) || Lists(pawnCls, comp)) {
        if (!g_waitSaid) {
            g_waitSaid = true;
            UE_LOGI("[RECYCLED-SLOT] waiting for the index to list the pawn's root component under its "
                    "own class");
        }
        return;
    }
    g_done = true;
    UE_LOGI("[RECYCLED-SLOT] the local pawn '%ls', its root component '%ls' in slot %d",
            R::ClassNameOf(pawn).c_str(), R::ClassNameOf(comp).c_str(), idx);
    RunIndexHalf(pawnCls, comp, compCls, idx);
    RunDispatchHalf(comp, rootOff);
    UE_LOGI("[RECYCLED-SLOT] DONE");
}

}  // namespace coop::dev::recycled_slot_drill
