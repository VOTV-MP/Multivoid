// coop/creatures/kerfur_disc_carry.cpp -- see coop/creatures/kerfur_disc_carry.h.

#include "coop/creatures/kerfur_disc_carry.h"

#include "coop/player/players_registry.h"
#include "coop/props/prop_record_wire.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_mainplayer.h"

namespace coop::kerfur_disc_carry {
namespace {

namespace E   = ue_wrap::engine;
namespace OI  = ue_wrap::object_index;
namespace PR  = ue_wrap::prop;
namespace R   = ue_wrap::reflection;
namespace SR  = ue_wrap::save_record;
namespace prw = coop::props::prop_record_wire;

constexpr const wchar_t* kDiscClass   = L"prop_floppyDisc_C";
constexpr const wchar_t* kDiscWhClass = L"prop_floppyDisc_Wh_C";

// The game's own two class casts (get_reports, kerfurOmega.cpp:1869-1882): a disc of the floppy family that is not Wh.
// A class not loaded here can have no instance, so an unresolved class answers no.
bool IsCarriedDiscClass(void* cls) {
    void* disc = OI::ClassByName(kDiscClass);
    if (!cls || !disc || !PR::WalksToBase(cls, disc)) return false;
    void* wh = OI::ClassByName(kDiscWhClass);
    return !(wh && PR::WalksToBase(cls, wh));
}

}  // namespace

std::optional<Capture> CaptureHeld() {
    void* me = coop::players::Registry::Get().Local();
    E::MainPlayerGrabState gs{};
    void* held = (me && E::ReadMainPlayerGrabState(me, gs)) ? gs.holdingActor : nullptr;
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

}  // namespace coop::kerfur_disc_carry
