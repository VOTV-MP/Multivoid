// coop/creatures/kerfur_disc_carry.cpp -- see coop/creatures/kerfur_disc_carry.h.

#include "coop/creatures/kerfur_disc_carry.h"

#include "coop/player/hand_history.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_record_wire.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

namespace coop::kerfur_disc_carry {
namespace {

namespace E   = ue_wrap::engine;
namespace K   = ue_wrap::kerfur;
namespace OI  = ue_wrap::object_index;
namespace PR  = ue_wrap::prop;
namespace R   = ue_wrap::reflection;
namespace SR  = ue_wrap::save_record;
namespace prw = coop::props::prop_record_wire;

constexpr const wchar_t* kDiscClass   = L"prop_floppyDisc_C";
constexpr const wchar_t* kDiscWhClass = L"prop_floppyDisc_Wh_C";

// How far behind its owner the hand lane's applied state can be: the newest hand state lands within one refill of the
// lane's budget (hand_item.cpp, kHandBudget {12, 4 per second}: a quarter second). The tail's class is asked of the lane
// with this much grace.
constexpr uint64_t kHandLagMs = 250;

// The game's own two class casts (get_reports, kerfurOmega.cpp:1869-1882): a disc of the floppy family that is not Wh.
// A class not loaded here can have no instance, so an unresolved class answers no.
bool IsCarriedDiscClass(void* cls) {
    void* disc = OI::ClassByName(kDiscClass);
    if (!cls || !disc || !PR::WalksToBase(cls, disc)) return false;
    void* wh = OI::ClassByName(kDiscWhClass);
    return !(wh && PR::WalksToBase(cls, wh));
}

void* HeldBy(void* player) {
    E::MainPlayerGrabState gs{};
    return (player && E::ReadMainPlayerGrabState(player, gs)) ? gs.holdingActor : nullptr;
}

}  // namespace

std::optional<Capture> CaptureHeld() {
    void* held = HeldBy(coop::players::Registry::Get().Local());
    if (!held || !R::IsLive(held) || !IsCarriedDiscClass(R::ClassOf(held))) return std::nullopt;
    Capture c;
    c.pledge.cls = R::ClassNameOf(held);
    if (!SR::CaptureOwnRecord(held, c.record) || !SR::RecordName(c.record, c.pledge.name) ||
        !prw::PutNameOk(c.record) || prw::RecordBytes(c.record) > prw::kMaxRecordBytes) {
        UE_LOGW("kerfur_disc_carry[client]: the held disc %p (%ls) has no record the host would take -- get_reports "
                "goes without it", held, c.pledge.cls.c_str());
        return std::nullopt;
    }
    c.pledge.actor.Set(held);
    UE_LOGI("kerfur_disc_carry[client]: get_reports carries the held disc %ls '%ls' (%zu record bytes)",
            c.pledge.cls.c_str(), c.pledge.name.c_str(), prw::RecordBytes(c.record));
    return c;
}

bool OnConsumed(const Pledge& pledge) {
    void* held = HeldBy(coop::players::Registry::Get().Local());
    if (!pledge.actor.Is(held)) {
        UE_LOGW("kerfur_disc_carry[client]: consume-miss -- the host's verb took the disc %ls '%ls' this press "
                "pledged, but the hand now holds %ls: that is kept", pledge.cls.c_str(), pledge.name.c_str(),
                held && R::IsLive(held) ? R::ClassNameOf(held).c_str() : L"nothing");
        return false;
    }
    if (!E::DestroyActor(held)) {
        UE_LOGW("kerfur_disc_carry[client]: the destroy of the pledged disc %ls '%ls' did not run", pledge.cls.c_str(),
                pledge.name.c_str());
        return false;
    }
    UE_LOGI("kerfur_disc_carry[client]: consumed -- the host's verb took the disc %ls '%ls'; it is destroyed here",
            pledge.cls.c_str(), pledge.name.c_str());
    return true;
}

TailVerdict JudgeTail(const SR::SaveRecord& tail, uint8_t slot, uint64_t arrivalMs, const char** why) {
    if (!prw::PutNameOk(tail)) {
        if (why) *why = "tail name missing or past its bound";
        return TailVerdict::Malformed;
    }
    void* cls = tail.className.empty() ? nullptr : OI::ClassByName(tail.className.c_str());
    if (!cls) {
        if (why) *why = "tail class is not loaded here";
        return tail.className.empty() ? TailVerdict::Malformed : TailVerdict::Unavailable;
    }
    void* disc = OI::ClassByName(kDiscClass);
    if (!disc || !PR::WalksToBase(cls, disc)) {
        if (why) *why = "tail class is no floppy disc";
        return TailVerdict::Malformed;
    }
    if (!coop::hand_history::HeldWithin(slot, tail.className.c_str(), kHandLagMs, arrivalMs)) {
        if (why) *why = "the hand lane does not show that disc";
        return TailVerdict::Unavailable;
    }
    return TailVerdict::Ok;
}

Gate ReadGate(void* omega) {
    bool has = false, animation = false, remote = false, server = false;
    // An unreadable gate is a closed one: the span cannot be shown to hold.
    if (!K::ReadHasFloppy(omega, has)) return Gate::Busy;
    if (has) return Gate::HasDisc;
    if (!K::ReadAnimation(omega, animation) || !K::ReadRemoteControl(omega, remote) ||
        !K::ReadServerValid(omega, server))
        return Gate::Busy;
    return (animation || remote || server) ? Gate::Busy : Gate::Open;
}

HandScope::HandScope(uint8_t slot, const SR::SaveRecord& record) : slot_(slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    puppet_ = (rp && rp->valid() && rp->HasPose()) ? rp->GetActor() : nullptr;
    std::wstring name;
    void* cls = OI::ClassByName(record.className.c_str());
    ue_wrap::FVector loc{};
    if (!puppet_ || !cls || !SR::RecordName(record, name) || !E::TryGetActorLocation(puppet_, loc)) {
        UE_LOGW("kerfur_disc_carry[host]: slot %u's disc has no puppet to stand in the hand of, or no class or name "
                "to build from -- not carried", static_cast<unsigned>(slot));
        return;
    }
    // The copy is built as the hand-item mirror is: the begin inside the mirror-spawn scope, the name before Init, the
    // marks before the finish. It also carries the destroy's mark at once, which holds for its life, so the verb's own
    // destroy is not published either.
    const ue_wrap::FRotator rot{};
    void* actor = nullptr;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn mirrorScope;
        actor = E::BeginDeferredSpawn(cls, loc, rot);
    }
    if (!actor) {
        UE_LOGW("kerfur_disc_carry[host]: the begin of slot %u's disc %ls failed -- not carried",
                static_cast<unsigned>(slot), record.className.c_str());
        return;
    }
    PR::WriteSpParityIdentity(actor, ue_wrap::fname_utils::StringToFName(name), false, false, false, false);
    coop::prop_echo_suppress::MarkMirrorSpawn(actor);
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
    if (!E::FinishDeferredSpawn(actor, loc, rot)) {
        UE_LOGW("kerfur_disc_carry[host]: the finish of slot %u's disc %ls failed -- the begun actor is destroyed, "
                "silently to the peers", static_cast<unsigned>(slot), record.className.c_str());
        E::DestroyActor(actor);
        return;
    }
    E::SetActorSimulatePhysics(actor, false);
    E::SetActorEnableCollision(actor, false);
    copy_.Set(actor);
    // The record goes onto the finished actor: its own loadData, the leaf half the game keeps (the disc's data and
    // readWrites), resolved by the host's own class index.
    if (!SR::ApplyRecord(actor, record, &OI::ClassByName)) {
        UE_LOGW("kerfur_disc_carry[host]: the record of slot %u's disc %ls did not apply -- not carried",
                static_cast<unsigned>(slot), record.className.c_str());
        return;   // the destructor destroys the copy
    }
    E::MainPlayerGrabState gs{};
    if (!E::ReadMainPlayerGrabState(puppet_, gs) || !E::WriteMainPlayerHoldingActor(puppet_, actor)) {
        UE_LOGW("kerfur_disc_carry[host]: slot %u's puppet took no disc in its hand -- not carried",
                static_cast<unsigned>(slot));
        return;
    }
    previous_ = gs.holdingActor;
    wrote_ = true;
    ok_ = true;
    UE_LOGI("kerfur_disc_carry[host]: slot %u's disc %ls '%ls' %p stands in its puppet's hand for the verb "
            "(local to this host, never published)", static_cast<unsigned>(slot), record.className.c_str(),
            name.c_str(), actor);
}

HandScope::~HandScope() {
    if (wrote_) E::WriteMainPlayerHoldingActor(puppet_, previous_);
    if (void* alive = copy_.Get()) {
        E::DestroyActor(alive);
        if (ok_)
            UE_LOGI("kerfur_disc_carry[host]: slot %u's disc copy %p outlived the verb (it refused the disc) -- "
                    "destroyed here, silently to the peers", static_cast<unsigned>(slot_), alive);
    }
}

bool HandScope::Consumed() const { return ok_ && !copy_.Alive(); }

}  // namespace coop::kerfur_disc_carry
