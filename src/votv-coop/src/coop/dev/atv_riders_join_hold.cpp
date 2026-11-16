// coop/dev/atv_riders_join_hold.cpp -- see coop/dev/atv_riders_join_hold.h.

#include "coop/dev/atv_riders_join_hold.h"

#include "coop/config/config.h"
#include "coop/dev/atv_riders_arm.h"
#include "coop/element/mirror_defer.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>

namespace coop::dev::atv_riders_join_hold {
namespace {

// Resolved on the first ask: both holds start held in the drill's rejoined life, free everywhere else; the lift is held in the
// first life of a run that runs step 0.
struct Holds {
    std::atomic<bool> birth{false};
    std::atomic<bool> bind{false};
    std::atomic<bool> lift{false};
    Holds() {
        const bool on = RejoinedLife() && !coop::dev::atv_riders_arm::Is("off");
        birth.store(on);
        bind.store(on);
        lift.store(!RejoinedLife() && coop::dev::atv_riders_arm::RunsFirstJoin());
    }
};

Holds& H() {
    static Holds h;
    return h;
}

// The kerfur the lift waits for, 0 until a stamp row names it. Game thread.
uint32_t g_liftEid = 0;

bool LiftHeld() {
    return H().lift.load(std::memory_order_acquire) &&
           coop::players::Registry::Get().LocalPeerId() == kDriverSlot;
}

}  // namespace

bool RejoinedLife() {
    static const bool rejoined = coop::config::ReadEnv("VOTVCOOP_ATV_RIDERS_REJOIN") == "1";
    return rejoined;
}

bool BirthHeld(uint8_t slot) { return slot == kDriverSlot && H().birth.load(std::memory_order_acquire); }

bool BindHeld() { return H().bind.load(std::memory_order_acquire); }

void ReleaseBirth() { H().birth.store(false, std::memory_order_release); }

void ReleaseBind() { H().bind.store(false, std::memory_order_release); }

void InstallLiftHold() {
    if (H().lift.load(std::memory_order_acquire)) coop::mirror_defer::SetLiftHold(&LiftHeld);
}

void NoteRideEid(uint32_t eid, bool bound) {
    if (g_liftEid != 0 || eid == 0 || !LiftHeld()) return;
    g_liftEid = eid;
    UE_LOGI("atv_riders_join_hold: the join's lift waits for the kerfur eid %u to bind (%s)", eid,
            bound ? "bound already" : "not bound yet");
    if (bound) ue_wrap::game_thread::Post([eid] { OnKerfurBound(eid); });
}

void OnKerfurBound(uint32_t eid) {
    if (g_liftEid == 0 || eid != g_liftEid || !LiftHeld()) return;
    H().lift.store(false, std::memory_order_release);
    UE_LOGI("atv_riders_join_hold: the kerfur eid %u is bound -- the join's lift resumes", eid);
    coop::mirror_defer::ResumeHeldLift();
}

void ReleaseLift() {
    if (!H().lift.exchange(false, std::memory_order_acq_rel)) return;
    UE_LOGI("atv_riders_join_hold: the drill's bound ended the join's lift hold");
    ue_wrap::game_thread::Post([] { coop::mirror_defer::ResumeHeldLift(); });
}

}  // namespace coop::dev::atv_riders_join_hold
