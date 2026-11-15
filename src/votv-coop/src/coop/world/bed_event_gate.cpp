// coop/world/bed_event_gate.cpp -- see coop/world/bed_event_gate.h.

#include "coop/world/bed_event_gate.h"

#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"  // Coast: the host's moved bed streams to every peer

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"

#include <atomic>
#include <cstdint>

namespace coop::bed_event_gate {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;

// bedEvent_C.woken is the handler bound to mainGamemode.wokenUp: it moves gamemode.bed to the event's own transform
// (K2_SetActorLocationAndRotation), teleports getMainPlayer beside it, then casts the bed to int_player and calls
// asProp().setPropProps(false x4) before destroying itself. Every call on the bed is a context call that does nothing
// on a null bed; the teleport reads the event's own transform.
UE_FACT_FN(kRowWoken, "bed_event_gate", L"bedEvent_C", L"woken", Declared);
constexpr int kTagWoken = 0x42454447;  // 'BEDG'

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_watched{false};

// A client's bed, held across the body that must not see it. The gamemode it was taken from is kept so a world change
// between the pre and the post writes nothing.
ue_wrap::CachedObjRef g_heldBed;
void* g_heldFrom = nullptr;

// The address of mainGamemode.bed on the live gamemode, or null while the class or the field does not resolve.
void** BedSlot(void* gm) {
    static int32_t s_off = -1;
    if (!gm) return nullptr;
    if (s_off < 0) {
        void* cls = R::ClassOf(gm);
        if (cls) s_off = R::FindPropertyOffset(cls, L"bed");
        if (s_off < 0) return nullptr;
    }
    return reinterpret_cast<void**>(static_cast<uint8_t*>(gm) + s_off);
}

coop::net::Session* RunningAs(coop::net::Role role) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == role) ? s : nullptr;
}

SG::Verdict OnWokenPre(const SG::Call&) {
    g_heldBed.Reset();
    g_heldFrom = nullptr;
    if (!RunningAs(coop::net::Role::Client)) return SG::Verdict::Run;
    void* const gm = ue_wrap::world_singleton::Gamemode();
    void** const slot = BedSlot(gm);
    if (!slot || !*slot) return SG::Verdict::Run;
    g_heldBed.Set(*slot);
    g_heldFrom = gm;
    *slot = nullptr;
    UE_LOGI("bed_event_gate: CLIENT wake -- the bed %p stays where it is (the host's bedEvent moves it); the teleport runs",
            g_heldBed.Raw());
    return SG::Verdict::Run;
}

void OnWokenPost(const SG::Call&) {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    void** const slot = BedSlot(gm);
    if (RunningAs(coop::net::Role::Client)) {
        if (g_heldBed.Raw() && gm == g_heldFrom && slot && !*slot) *slot = g_heldBed.Get();
        g_heldBed.Reset();
        g_heldFrom = nullptr;
        return;
    }
    if (!RunningAs(coop::net::Role::Host) || !slot || !*slot) return;
    UE_LOGI("bed_event_gate: HOST wake -- the bed %p moved to the event's transform; streaming it", *slot);
    coop::prop_drive_host::Coast(*slot, "bedEvent");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    if (!SG::WatchName(kRowWoken, kTagWoken, &OnWokenPre, &OnWokenPost))
        UE_LOGE("bed_event_gate: the watch on bedEvent_C::woken did not register -- a client's bedEvent moves its own bed");
}

}  // namespace coop::bed_event_gate
