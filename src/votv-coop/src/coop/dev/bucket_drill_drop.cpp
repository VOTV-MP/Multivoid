// coop/dev/bucket_drill_drop.cpp -- the drop arm of the bucket drill; see coop/dev/bucket_drill.h.
//
// Leg 1, honest: the client holds a filled bucket (one whose host-driven drive has ended here: a copy a drive still parks is not taken by
// the game's hold) and drops it by the game's drop. The record lane's land of the drop's record is the event: at it the host reads its
// copy's water (prop_save_data::dev::SetLandObserver), and it equals the water the client held, the record's own value. Leg 2, forged: the client writes 999 into its own held bucket (the forger's own item,
// which is what the leg tests) and drops it; the host's gate counts the record out of range, the host's copy never reads the forged
// water, and the host's own row reaches the sender's copy. Both legs are judged by events: the land observer says whether a record
// of the key was applied (and the water it carried at that moment), and the gate's out-of-range counter, taken before the client can
// drop, says whether the record was refused. The red `gate` takes the record without the gate: the land of the forged key is the FAIL.

#include "bucket_drill_detail.h"

#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/props/bucket_sync.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/bucket.h"

#include <string>

namespace coop::dev::bucket_drill::detail {

namespace B   = ue_wrap::bucket;
namespace BS  = coop::bucket_sync;
namespace PS  = coop::prop_save_data;
namespace PDS = coop::prop_drive_stream;
namespace HB  = coop::hand_item;

namespace {

constexpr float kForgedCm = 999.f;

struct Drop {
    bool begun = false;
    int ph = 0, slot = 0, heldCenti = 0, calm = 0, hostCenti = 0;
    // The staged buckets as they stood when the stage was said: the client's hold destroys the staged actor and the host spawns a new
    // one under the same key, so the key alone cannot tell the two apart.
    void* stage[2] = {};
    int stageEid[2] = {};
    std::wstring keys[2];
    std::wstring key;   // the key of the bucket the client holds
    // The land of a record under each staged key, read at the land itself: the client says its hold and drops in one tick, so the
    // record can land before the host reads the hold, and the observer records for both keys from the stage on.
    bool landed[2] = {};
    int landCenti[2] = {}, landMax[2] = {};
    unsigned long long outBase = 0, notAwaitedBase = 0;
    uint32_t expiredBase = 0;   // the record lane's waits that had run out before the drill began
    // the client
    int cph = 0, leg = 1, used = -1, pick = -1, eid = 0;
} g_d;

// The host's copy of the bucket the client dropped: standing under the key, a new actor with its own element id, at rest.
void* Dropped(Drop& a) {
    void* const b = ByKey(a.key);
    if (!b || b == a.stage[a.slot]) return nullptr;
    const int eid = EidOf(b);
    return eid != 0 && eid != a.stageEid[a.slot] && Calm(b, a.calm) ? b : nullptr;
}

// The record lane has applied a record under a staged key: the host's copy now holds the record's water, read here before any later
// splash of the copy's own can change it. The first land of each key is the one kept.
void OnLand(void* actor, const std::wstring& key) {
    Drop& a = g_d;
    B::Plain p;
    for (int i = 0; i < 2; ++i) {
        if (a.keys[i].empty() || key != a.keys[i] || a.landed[i] || !B::ReadPlain(actor, p)) continue;
        a.landed[i] = true;
        a.landCenti[i] = Centi(p.height);
        a.landMax[i] = Centi(p.maxHeight);
        Fact("drop: the record lane applied a record under the key of staged bucket %d: the host's copy reads %.2f cm at the land (a bucket "
             "holds %.2f cm at most)", i, static_cast<double>(p.height), static_cast<double>(p.maxHeight));
    }
}

}  // namespace

void DropHost(coop::net::Session& s) {
    Drop& a = g_d;
    switch (a.ph) {
    case 0:
        if (!a.begun) {
            Spec sp;
            sp.plains = 2;
            StageBegin(sp);
            PS::dev::SetLandObserver(&OnLand);
            a.begun = true;
        }
        if (!StageStep(s)) return;
        a.ph = 1;
        Go();
        return;
    case 1:
        if (!StageSay()) return;
        for (int i = 0; i < 2; ++i) {
            a.stage[i] = StageBucket(i);
            a.stageEid[i] = a.stage[i] ? EidOf(a.stage[i]) : 0;
            a.keys[i] = StageBucketKey(i);
        }
        // Taken here, before the client can drop: it says its hold and drops in one tick, so the gate can refuse a record before the
        // host reads the hold.
        a.outBase = BS::CountOf(BS::Count::RecordsOutOfRange);
        a.notAwaitedBase = BS::CountOf(BS::Count::RecordsNotAwaited);
        a.expiredBase = PS::dev::AwaitsExpired();
        a.ph = 2;
        Go();
        return;
    case 2:
    case 4: {   // the client has held a bucket: the host judges what the observer recorded at its land
        const bool forged = a.ph == 4;
        std::string rest;
        int v[2] = {};
        if (!Rest(forged ? "held2 " : "held1 ", &rest)) {
            Late("the client never held a bucket");
            return;
        }
        if (!ParseInts(rest, 2, v) || v[0] < 0 || v[0] >= 2) return Invalid("the client's held line could not be read");
        a.slot = v[0];
        a.heldCenti = v[1];
        a.key = a.keys[a.slot];
        if (!a.stage[a.slot] || a.stageEid[a.slot] == 0 || a.key.empty())
            return Invalid("the staged bucket's actor, element id and key were not recorded");
        a.calm = 0;
        Fact("drop leg %d: the client holds bucket %d, its water reads %.2f cm", forged ? 2 : 1, a.slot, static_cast<double>(v[1]) / 100.0);
        a.ph += 1;
        Go();
        return;
    }
    case 3: {   // honest: decided at the land of the drop's record
        if (!a.landed[a.slot]) {
            if (BS::CountOf(BS::Count::RecordsNotAwaited) > a.notAwaitedBase || BS::CountOf(BS::Count::RecordsOutOfRange) > a.outBase)
                return Fail("the host's gate refused the honest drop's record (not awaited %llu, out of range %llu): it never landed",
                            BS::CountOf(BS::Count::RecordsNotAwaited) - a.notAwaitedBase,
                            BS::CountOf(BS::Count::RecordsOutOfRange) - a.outBase);
            Late("the drop's record never landed on the host");
            return;
        }
        if (a.landCenti[a.slot] != a.heldCenti)
            return Fail("the host's copy of the dropped bucket reads %.2f cm at the land of the drop's record, the client held %.2f cm: the "
                        "record's water was not applied", static_cast<double>(a.landCenti[a.slot]) / 100.0, static_cast<double>(a.heldCenti) / 100.0);
        Fact("drop leg 1: at the land of the drop's record the host's copy reads %.2f cm, the water the client held",
             static_cast<double>(a.landCenti[a.slot]) / 100.0);
        a.outBase = BS::CountOf(BS::Count::RecordsOutOfRange);
        a.notAwaitedBase = BS::CountOf(BS::Count::RecordsNotAwaited);
        Say("go2");
        a.ph = 4;
        Go();
        return;
    }
    case 5: {   // forged: decided by the observer's land and the gate's counter, never by a read of the copy after the fact
        if (a.landed[a.slot])
            return Fail("the forged record was applied: at its land the host's copy reads %.2f cm and a bucket holds %.2f cm at most",
                        static_cast<double>(a.landCenti[a.slot]) / 100.0, static_cast<double>(a.landMax[a.slot]) / 100.0);
        if (BS::CountOf(BS::Count::RecordsNotAwaited) > a.notAwaitedBase)
            return Fail("the forged record was refused as not awaited, not for its range: the leg did not reach the gate's range check");
        if (BS::CountOf(BS::Count::RecordsOutOfRange) <= a.outBase) {
            Late("the gate never counted the forged record out of range");
            return;
        }
        void* const d = Dropped(a);
        B::Plain p;
        if (!d || !B::ReadPlain(d, p)) {
            Late("the host's copy of the forged bucket never stood at rest");
            return;
        }
        if (p.height > p.maxHeight + 0.5f)
            return Fail("the gate counted the forged record out of range, but the host's copy reads %.2f cm and a bucket holds %.2f cm at most",
                        static_cast<double>(p.height), static_cast<double>(p.maxHeight));
        a.hostCenti = Centi(p.height);
        Fact("drop leg 2: the gate counted the forged record out of range and no land followed; the host's copy reads %.2f cm",
             static_cast<double>(p.height));
        Say("forged-ok " + Ints({EidOf(d), a.hostCenti}));
        a.ph = 6;
        Go();
        return;
    }
    default: {
        // The refusal ends the key's wait, so the host's first row follows it at once. A wait that ran out is the timeout's publish: the
        // sender's copy would be set right by it and not by the correction.
        if (PS::dev::AwaitsExpired() > a.expiredBase)
            return Fail("the record lane's wait for the refused record ran out (%u expired): the sender's row came from the timeout, not from the "
                        "refusal", static_cast<unsigned>(PS::dev::AwaitsExpired() - a.expiredBase));
        std::string rest;
        int v[1] = {};
        if (!Rest("c-r2 ", &rest)) {
            Late("the client never said what its dropped copy read");
            return;
        }
        if (!ParseInts(rest, 1, v)) return Invalid("the client's reading could not be read");
        if (v[0] != a.hostCenti) return Fail("the sender's copy reads %.2f cm after the refusal, the host's reads %.2f cm", static_cast<double>(v[0]) / 100.0,
                                             static_cast<double>(a.hostCenti) / 100.0);
        return Pass("drop: the host's copy read the water of an honest drop's record, a forged record was counted out of range and never "
                    "applied, and the host's row reached the sender's copy");
    }
    }
}

void DropClient() {
    Drop& a = g_d;
    void* const me = Local();
    if (!me) return;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(2, 0)) return;
        a.cph = 1;
        Go();
        return;
    case 1: {   // the first bucket whose drive has ended here that this leg has not used
        a.pick = -1;
        std::string parked;
        for (int i = 0; i < 2; ++i) {
            const int eid = ClientEid(i);
            uint8_t gen = 0;
            if (i == a.used || eid <= 0) continue;
            if (PDS::dev::ClosedGen(static_cast<uint32_t>(eid), gen)) {
                a.pick = i;
                break;
            }
            parked += (parked.empty() ? "" : ", ") + std::to_string(eid);
        }
        if (a.pick < 0) {
            Late(("no placed bucket's drive ended on the client; still parked: " + parked).c_str());
            return;
        }
        a.eid = ClientEid(a.pick);
        if (Approach(me, ClientBucket(a.pick)) <= 0) return;
        if (!Hold(me, ClientBucket(a.pick))) return Invalid("Hold Object did not run");
        Fact("drop leg %d: the client takes bucket %d (eid=%d), whose last drive ended here", a.leg, a.pick, a.eid);
        a.cph = 2;
        Go();
        return;
    }
    case 2: {
        if (!HandReady()) {
            Late("the hand never held the bucket");
            return;
        }
        void* const hand = HB::LocalHandActor();
        B::Plain p;
        if (!B::ReadPlain(hand, p)) return Invalid("the held bucket cannot be read");
        if (!(p.height > 0.f)) return Invalid("the held bucket holds no water: a record of it cannot be told from a fresh bucket's");
        const int centi = Centi(p.height);
        if (a.leg == 2) {
            // The forger's own item: its record carries water no bucket can hold.
            if (!B::WritePlain(hand, kForgedCm, p.soap, p.alcoholP, p.alcohol, p.alcReady)) return Invalid("the held bucket's water could not be written");
        }
        Say(std::string(a.leg == 1 ? "held1 " : "held2 ") + Ints({a.pick, centi}));
        if (!DropHeld(me)) return Invalid("the game's drop did not run");
        if (a.leg == 2) Say("dropped2");
        a.cph = 3;
        Go();
        return;
    }
    case 3:
        if (!HandEmpty()) {
            Late("the hand never emptied after the drop");
            return;
        }
        if (a.leg == 2) {
            a.cph = 5;
            Go();
            return;
        }
        a.used = a.pick;
        a.leg = 2;
        a.cph = 4;
        Go();
        return;
    case 4:
        if (!Seen("go2")) {
            Late("the host never said the honest leg held");
            return;
        }
        a.cph = 1;
        Go();
        return;
    default: {   // the host's row reaches the dropped copy that held the forged water
        std::string rest;
        int v[2] = {};
        if (!Rest("forged-ok ", &rest)) {
            Late("the host never said it refused the forged record");
            return;
        }
        if (!ParseInts(rest, 2, v)) return Invalid("the host's line could not be read");
        // The host answers the sender once the host's copy has an element id: a row applied to the sender's copy (a seq) carrying the
        // host's own water.
        B::Plain p;
        if (BS::dev::AppliedSeq(static_cast<uint32_t>(v[0])) == 0 || !B::ReadPlain(LiveProp(v[0]), p) || Centi(p.height) != v[1]) {
            Late("the sender's copy never got the host's row after the refusal");
            return;
        }
        Say("c-r2 " + std::to_string(Centi(p.height)));
        Finish();
        return;
    }
    }
}

void ResetDrop() {
    PS::dev::SetLandObserver(nullptr);
    g_d = Drop{};
}

}  // namespace coop::dev::bucket_drill::detail
