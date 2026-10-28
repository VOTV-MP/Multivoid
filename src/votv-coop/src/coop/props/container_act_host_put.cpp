// coop/props/container_act_host_put.cpp -- the host's half of a put from a record: the judge of a PutRecord request (its
// inputs and its order), the reservation a grant opens, and the handling of a reservation's late commit. The queue,
// the holds' ends and the take are container_act_host.cpp's; what the two share is container_act_host_internal.h.

#include "coop/props/container_act_host.h"
#include "coop/props/container_act_host_internal.h"

#include "coop/element/registry.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/net_clock.h"
#include "coop/props/container_act_wire.h"
#include "coop/props/container_record_edit.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"

#include <string>
#include <utility>
#include <vector>

namespace coop::props::container_act_host {
namespace {

namespace cas = coop::props::container_act_state;
namespace cre = coop::props::container_record_edit;
namespace caw = coop::props::container_act_wire;
namespace cr  = coop::props::container_reach;
namespace csw = coop::props::container_slice_wire;
namespace ci  = ue_wrap::container_inventory;
namespace pg  = ue_wrap::prop_gather;
namespace SR  = ue_wrap::save_record;

// The container's room for one more record of `rec`'s size (and the fridge flag the container adds): the lane's record
// cap and the slice's byte ceiling, the one SendBlob enforces on what the container publishes. Done unless the
// container moved under the read; a count past the cap does not read at all, and is the cap.
cre::EditResult ReadFit(uint32_t eid, const SR::SaveRecord& rec, bool& countAtCap, bool& bytesOver) {
    countAtCap = bytesOver = false;
    std::vector<SR::SaveRecord> recs;
    const cre::EditResult r = cre::ReadRecords(eid, recs, /*neuterNested=*/false);
    if (r == cre::EditResult::Unreadable) {
        countAtCap = true;
        return cre::EditResult::Done;
    }
    if (r != cre::EditResult::Done) return r;
    countAtCap = recs.size() >= csw::kMaxRecords;
    bytesOver = csw::Pack(eid, 0, recs).size() + caw::RecordBytes(rec) + caw::kFridgeElementBytes >
                coop::blob_chunks::MaxBlobBytes();
    return cre::EditResult::Done;
}

}  // namespace

namespace detail {

// A put's own terms, read in DecidePut's order after the common ones; each read stops at the first term that decides.
// The class resolves through the object index, the resolver the container's own write uses, so a name it cannot
// resolve is refused here and never written as a null class; a forged name costs a hash probe, not an array walk.
void ReadPutInputs(uint8_t slot, const caw::Request& q, internal::PutInputs& p) {
    const internal::Inputs& b = p.base;
    if (!b.worldReady || b.holdsWait || b.reach != cr::Reach::Reached || b.container != internal::Container::Ready)
        return;
    void* const actor = coop::element::LivePropActor(q.eid);
    ci::PutTerms t{};
    p.termsRead = actor && ci::ReadPutTerms(actor, t);
    if (!p.termsRead) return;
    p.locked = t.locked;
    p.slowed = cas::Live().Slowed(slot, coop::net::NowMs());
    if (p.locked || p.slowed) return;
    std::wstring name;
    p.nameOk = SR::RecordName(q.record, name);
    if (!p.nameOk) return;
    void* const cls = ue_wrap::object_index::ClassByName(q.record.className.c_str());
    p.classResolved = cls != nullptr;
    p.propClass = SR::IsPropClass(cls);
    if (!p.classResolved || !p.propClass) return;
    p.filterRead = ci::FilterHolds(ci::InventoryOf(actor), cls, p.filterHolds);
    if (!p.filterRead || p.filterHolds) return;
    p.mailboxContainer = t.ownerIsMailbox && ci::IsContainerRecord(q.record);
    if (p.mailboxContainer) return;
    pg::PropDataFlags row{};
    bool found = false;
    p.rowRead = pg::ReadRowFlags(name, row, found);
    if (!p.rowRead) return;
    p.rowRefuses = found && (!row.canCollect || row.heavy);   // a class with no row is collectable, the game's rule
    if (p.rowRefuses) return;
    switch (ReadFit(q.eid, q.record, p.countAtCap, p.bytesOver)) {
        case cre::EditResult::Done:     break;
        case cre::EditResult::NotReady: p.base.container = internal::Container::NotReady; return;
        default:                        p.base.container = internal::Container::Gone; return;
    }
    if (p.countAtCap || p.bytesOver) return;
    p.infinite = t.infinite;
    p.maxVol = t.maxVol;
    p.currVol = t.currVol;
    if (!t.infinite) p.volumeRead = pg::VolumeOfName(name, actor, p.volume, p.volumeKnown);
}

// A put's grant: room is reserved for the asked record and nothing is written.
std::optional<coop::net::Served> GrantPut(uint8_t slot, const caw::Request& q) {
    cas::Hold h;
    h.kind = cas::HoldKind::Reservation;
    h.slot = slot;
    h.id = q.id;
    h.eid = q.eid;
    h.record = q.record;
    h.grantedMs = coop::net::NowMs();
    if (cas::Live().Open(std::move(h))) return std::nullopt;
    Bump(Tally::GrantUnsent);   // cannot happen past holdsWait
    Answer(slot, ReplyOf(internal::Verdict::Gone, q.id));
    return coop::net::Served::Done;
}

// A commit whose hold is gone: the deadline released a reservation and left its tombstone, so an honest client's late
// answer ends here. True when the commit was a tombstone's, consumed whatever came of it; false when it has none, so
// it is a bad commit. A late done is inserted like a timely one, unfitted at worst (volume is the client's fit), unless
// the container has since filled past a cap, taken a hold or filtered the class: then it is named and lost.
bool LateCommit(uint8_t slot, const caw::Commit& c) {
    if ((c.committed == 1) != c.hasRecord) return false;
    std::optional<cas::Tombstone> t = cas::Live().ConsumeTombstone(slot, c.id, c.eid);
    if (!t) return false;
    if (c.committed == 0) {
        UE_LOGI("container_act: slot=%u id=%u eid=%u late put abort, nothing to release", static_cast<unsigned>(slot),
                static_cast<unsigned>(c.id), static_cast<unsigned>(c.eid));
        return true;
    }
    std::wstring name;
    if (c.record.className != t->className || !SR::RecordName(c.record, name) || name != t->name) {
        Bump(Tally::BadCommit);
        NoteRefusal(slot, "late commit names another record");
        UE_LOGI("container_act: slot=%u id=%u eid=%u late put done names another record -> bad",
                static_cast<unsigned>(slot), static_cast<unsigned>(c.id), static_cast<unsigned>(c.eid));
        return true;
    }
    bool countAtCap = false, bytesOver = false;
    const cre::EditResult read = ReadFit(c.eid, c.record, countAtCap, bytesOver);
    if (read == cre::EditResult::NoElement || read == cre::EditResult::NoInventory) {
        Bump(Tally::TargetGone);
        UE_LOGI("container_act: slot=%u id=%u eid=%u late put done, the container is gone and the item with it",
                static_cast<unsigned>(slot), static_cast<unsigned>(c.id), static_cast<unsigned>(c.eid));
        return true;
    }
    void* const cls = ue_wrap::object_index::ClassByName(c.record.className.c_str());
    bool filtered = false;
    const bool filterRead = cls && ci::FilterHolds(ci::InventoryOf(coop::element::LivePropActor(c.eid)), cls, filtered);
    if (countAtCap || bytesOver || cas::Live().HoldOn(c.eid) || !filterRead || filtered) {
        Bump(Tally::BadCommit);
        Bump(Tally::LateDoneLost);
        NoteRefusal(slot, "late put done over the cap");
        UE_LOGW("container_act: late put done over the cap (eid %u, %ls) -- the item is lost",
                static_cast<unsigned>(c.eid), c.record.className.c_str());
        return true;
    }
    cas::Hold h;
    h.slot = slot;
    h.id = c.id;
    h.eid = c.eid;
    h.index = c.index;
    h.record = c.record;
    Bump(Tally::LateDone);
    UE_LOGI("container_act: late put done inserted (eid %u, %ls)", static_cast<unsigned>(c.eid),
            c.record.className.c_str());
    Place(std::move(h), "late put done", cre::Source::Wire, "inserted");
    return true;
}

}  // namespace detail

namespace internal {

Verdict DecidePut(const PutInputs& in, Tally* refusal) {
    Tally none = Tally::kCount;
    Tally& why = refusal ? *refusal : none;
    why = Tally::kCount;
    const Inputs& b = in.base;
    if (!b.worldReady) return Verdict::Wait;
    if (b.holdsWait) return Verdict::Wait;
    if (b.reach == cr::Reach::NoBody) return Verdict::Wait;
    if (b.container == Container::Gone) return Verdict::Gone;
    if (b.container == Container::NotReady) return Verdict::Wait;
    if (b.reach == cr::Reach::Unreachable) return Verdict::Reach;
    if (!in.termsRead) return Verdict::Wait;
    if (in.locked) return Verdict::Locked;
    if (in.slowed) return Verdict::Lapsed;
    if (!in.nameOk) { why = Tally::PutNoName; return Verdict::Refused; }
    if (!in.classResolved) { why = Tally::PutClassUnresolved; return Verdict::Refused; }
    if (!in.propClass) { why = Tally::PutNotProp; return Verdict::Refused; }
    if (!in.filterRead) return Verdict::Wait;
    if (in.filterHolds) { why = Tally::PutFiltered; return Verdict::Refused; }
    if (in.mailboxContainer) { why = Tally::PutIntoMailbox; return Verdict::Refused; }
    if (!in.rowRead) return Verdict::Wait;
    if (in.rowRefuses) { why = Tally::PutRowRefuses; return Verdict::Refused; }
    if (in.countAtCap || in.bytesOver) return Verdict::NoSpace;
    if (!in.infinite) {
        if (!in.volumeRead) return Verdict::Wait;
        if (!in.volumeKnown) { why = Tally::PutUnknownName; return Verdict::Refused; }
        if (in.currVol + in.volume > in.maxVol) return Verdict::NoSpace;
    }
    return Verdict::Grant;
}

bool StoredMatches(const SR::SaveRecord& asked, const SR::SaveRecord& stored) {
    std::wstring askedName, storedName;
    return asked.className == stored.className && SR::RecordName(asked, askedName) &&
           SR::RecordName(stored, storedName) && askedName == storedName;
}

}  // namespace internal

}  // namespace coop::props::container_act_host
