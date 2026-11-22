// coop/props/bucket_record_gate.cpp -- the class gate on a client's save record of a water bucket, a concrete bucket, a cement bag
// or a trowel. See coop/props/bucket_sync.h.
//
// A bucket, a bag or a trowel the client held reaches the host in its drop's record. This gate lets one land only when (a) it is
// the record the sender's own drop awaits (prop_save_data::AwaitsRecordFrom), and (b) every number is one the class can hold:
// the water within the bucket's maxHeight and the soap within a forged-row bound (the BP caps neither), a concrete bucket's
// units within 20 and the units it had when the sender took it, a bag's within its maxUnits, a trowel's load within 5. Any other
// record is refused, final for that record, counted by cause: its wait ends, so the element's rows are not held back, and the host's row of
// the element goes to the sender (at once, or when the copy has an element id). A held item is its holder's own
// (coop/player/hand_spend): the gate holds what the host can see, a record for an item in the world.
//
// Residual: the amounts of an item a client dropped from its own hand are the holder's word, bounded by the classes' maxima, so a
// modified client can drop a water bucket fuller than it picked it up, to the bucket's own cap. MTA trusts a client's reported
// ammo once the server's weapon type matches (CPlayerPuresyncPacket.cpp:214-274); the host keeps a count for a held cement bag
// and a held concrete bucket's units at the take (coop/props/bucket_hold_ledger). A trowel's take is the game's own rule.

#include "bucket_internal.h"

#include "coop/props/bucket_hold_ledger.h"
#include "coop/props/bucket_sync.h"
#include "coop/props/prop_record_gates.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <string>

namespace coop::bucket_sync::detail {
namespace {

namespace B  = ue_wrap::bucket;
namespace SR = ue_wrap::save_record;

constexpr int32_t kSoapCap = 10000;          // the BP caps no soap (a soap adds 10): a bound against a forged record
constexpr int32_t kTrowelCap = 5;            // the trowel's load, 0 or 5
constexpr int32_t kConcreteUnitsCap = 20;    // a mix makes 20 at most

bool g_registered = false;

bool Refuse(void* actor, uint8_t slot, const std::wstring& key, Count cause, const char* why) {
    Note(cause);
    const uint64_t n = CountOf(Count::RecordsNotAwaited) + CountOf(Count::RecordsOutOfRange);
    if ((n & (n - 1)) == 0)
        UE_LOGW("bucket_sync: HOST record refused %ls from slot %u: %s (#%llu)", key.c_str(), static_cast<unsigned>(slot), why,
                static_cast<unsigned long long>(n));
    // A refused record is final for its key: the wait ends, so the lane sends the element's first row to every peer (the record lane's
    // own end of a wait, as a landed record's).
    coop::prop_save_data::EndAwaitFrom(key, slot);
    // The sender's copy is set right by the host's own numbers (MTA CGame.cpp:2775-2793). A copy with no element id yet is owed the row,
    // which goes at the mint of its id; any other refusal is the transport's, said once per key.
    if (!SendRowTo(slot, actor)) {
        if (EidOf(actor) == 0) OweRow(slot, actor);
        else if (FirstRowRefusal(key))
            UE_LOGW("bucket_sync: the host's row for %ls was not sent back to slot %u (the transport refused it)", key.c_str(),
                    static_cast<unsigned>(slot));
    }
    return false;
}

// Whether the record's numbers are ones the class can hold. The copy's own maxima bound the water and the bag, and the units a
// concrete bucket had at its take the concrete bucket's.
bool InRange(B::Kind kind, void* actor, uint8_t slot, const B::Record& r) {
    switch (kind) {
    case B::Kind::Plain: {
        B::Plain p;
        return B::ReadPlain(actor, p) && std::isfinite(r.height) && r.height >= 0.f && r.height <= p.maxHeight + 0.01f &&
               r.soap >= 0 && r.soap <= kSoapCap;
    }
    case B::Kind::Concrete: {
        const int32_t taken = coop::bucket_hold::ConcreteUnits(slot);   // -1: the host never saw this one taken
        return r.units >= 0 && r.units <= kConcreteUnitsCap && (taken < 0 || r.units <= taken);
    }
    case B::Kind::Bag: {
        int32_t units = 0, maxUnits = 0;
        return B::ReadBag(actor, units, maxUnits) && r.units >= 0 && r.units <= maxUnits;
    }
    case B::Kind::Trowel:
        return r.concrete >= 0 && r.concrete <= kTrowelCap;
    default:
        return false;
    }
}

bool Judge(void* actor, uint8_t slot, const SR::SaveRecord& rec) {
    const std::wstring key = ue_wrap::prop::GetKeyString(actor);
    const B::Kind kind = B::KindOf(actor);
    if (dev::RedOf() == dev::Red::Gate) {
        UE_LOGW("bucket_sync: bucket_red=gate -- slot %u's record for %ls is taken without the gate", static_cast<unsigned>(slot),
                key.c_str());
        return true;
    }
    if (key.empty() || !coop::prop_save_data::AwaitsRecordFrom(key, slot))
        return Refuse(actor, slot, key, Count::RecordsNotAwaited, "no drop of this sender awaits it");
    B::Record r;
    if (!B::ReadRecord(kind, rec, r)) return Refuse(actor, slot, key, Count::RecordsOutOfRange, "it is not this class's record layout");
    if (!InRange(kind, actor, slot, r)) return Refuse(actor, slot, key, Count::RecordsOutOfRange, "a number is past the class's range");
    return true;
}

}  // namespace

void RegisterRecordGate() {
    if (g_registered) return;
    g_registered = true;
    for (const wchar_t* cls : {L"prop_bucket_C", L"prop_concreteBucket_C", L"prop_cementBag_C", L"prop_wallfixer_C"})
        coop::prop_record_gates::RegisterIntentRecordGate(cls, &Judge);
}

}  // namespace coop::bucket_sync::detail
