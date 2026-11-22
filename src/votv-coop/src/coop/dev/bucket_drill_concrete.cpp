// coop/dev/bucket_drill_concrete.cpp -- the mix, trowel and ctick arms of the bucket drill; see coop/dev/bucket_drill.h.
//
// mix: the client holds a cement bag and runs its left click on a filled bucket; the host replaces the bucket with a concrete bucket
// of `need` units at its place, and the client's copy of it stands with the same units, no concrete copy without an element id, and the
// held bag spent by `need`. trowel: the host's own mix and takes leave a concrete bucket two units from its last; the client takes
// one unit with a held trowel, then the last with a second trowel, which turns the bucket into a plain one. ctick: the same first take,
// judged on the client's concrete copy's tick counters; a copy that ran its own tick says so at once.
// A bucket the host's apply makes has no element id when the apply returns, so the host marks its row itself once it has one and says
// that row's seq (bucket_sync::MarkDirty).

#include "bucket_drill_detail.h"

#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/props/bucket_sync.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace coop::dev::bucket_drill::detail {

namespace B  = ue_wrap::bucket;
namespace BS = coop::bucket_sync;
namespace E  = ue_wrap::engine;
namespace HB = coop::hand_item;

namespace {

// The need of a bucket's water, as the BP rounds it.
int MixNeed(const B::Plain& p) {
    return p.maxHeight > 0.f ? static_cast<int>(std::floor(p.height / p.maxHeight * 20.f + 0.5f)) : 0;
}

// The live instance of `cls` that stood in none of `before` and lies within `cm` of `at`; null when none.
void* Born(const wchar_t* cls, const std::vector<void*>& before, const ue_wrap::FVector& at, float cm) {
    for (void* a : LiveOf(cls)) {
        if (std::find(before.begin(), before.end(), a) != before.end()) continue;
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(a, p)) continue;
        const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= cm) return a;
    }
    return nullptr;
}

uint32_t Mark(void* actor) { return BS::MarkDirty(actor); }

int NoEidConcrete() {
    int n = 0;
    for (void* a : LiveOf(L"prop_concreteBucket_C"))
        if (EidOf(a) == 0) ++n;
    return n;
}

// ---- mix ----

struct Mix {
    bool begun = false;
    int ph = 0, cph = 0, need = 0, bag0 = 0, oldEid = 0, newEid = 0, calm = 0;
    ue_wrap::FVector at{};
    std::vector<void*> before;
    Round round;
    ue_wrap::CachedObjRef made;
} g_mix;

// ---- trowel and ctick ----

struct Trowel {
    bool begun = false;
    int ph = 0, cph = 0, calm = 0, bucketCalm = 0, drop = 0, units0 = 0, eidConc = 0;
    HostUse use;
    std::vector<void*> before;
    ue_wrap::FVector at{};
    Round round;
    ue_wrap::CachedObjRef conc, plain;
    std::wstring concKey;
    int eidPlain = 0;
    bool ranSaid = false, dropped = false;
} g_tw;

}  // namespace

// ================================================================ mix =====================================================

void MixHost(coop::net::Session& s) {
    Mix& a = g_mix;
    switch (a.ph) {
    case 0:
        if (!a.begun) {
            Spec sp;
            sp.plains = 1;
            sp.items[0] = L"prop_cementBag";
            sp.nItems = 1;
            StageBegin(sp);
            a.begun = true;
        }
        if (!StageStep(s)) return;
        a.ph = 1;
        Go();
        return;
    case 1:
        if (!StageSay()) return;
        a.ph = 2;
        Go();
        return;
    case 2: {
        std::string rest;
        if (!Rest("c-ready ", &rest)) {
            Late("the client never said it holds the cement bag");
            return;
        }
        if (!ParseInts(rest, 1, &a.bag0)) return Invalid("the client's ready line could not be read");
        void* const b = StageBucket(0);
        B::Plain p;
        if (!b || !B::ReadPlain(b, p) || !E::TryGetActorLocation(b, a.at)) return Invalid("the host's bucket cannot be read");
        a.need = MixNeed(p);
        a.oldEid = EidOf(b);
        if (!p.canConcrete || a.need < 1 || a.need > 20) return Invalid("the host's bucket cannot be mixed: no water or no room for concrete");
        a.before = LiveOf(L"prop_concreteBucket_C");
        a.round = RoundBegin();
        Fact("mix: the bucket eid=%d holds %.2f cm, so the mix needs %d units; the client's bag holds %d", a.oldEid,
             static_cast<double>(p.height), a.need, a.bag0);
        Say("go1");
        a.ph = 3;
        Go();
        return;
    }
    case 3: {
        if (RoundStep(a.round, "mix") <= 0) return;
        void* const made = Born(L"prop_concreteBucket_C", a.before, a.at, 60.f);
        if (!made)
            return Fail("the host's target did not move by its commit: no concrete bucket stands where the bucket was (the bucket %s)",
                        StageBucket(0) ? "still stands" : "is gone");
        a.made.Set(made);
        a.ph = 4;
        Go();
        return;
    }
    case 4: {
        void* const made = a.made.Get();
        if (!made) return Invalid("the host's new concrete bucket died");
        B::Concrete c;
        if (EidOf(made) == 0 || !B::ReadConcrete(made, c)) {
            Late("the host's new concrete bucket never got an element id");
            return;
        }
        if (c.units != a.need) return Fail("the host's concrete bucket holds %d units, the mix needed %d", c.units, a.need);
        if (StageBucket(0)) return Fail("the host's bucket still stands after its mix: the old bucket was not replaced");
        a.newEid = EidOf(made);
        const uint32_t seq = Mark(made);
        if (!seq) return Invalid("the host's row of the new concrete bucket could not be marked");
        Say("seq1 " + Ints({a.newEid, static_cast<int>(seq), c.units, a.oldEid}));
        a.ph = 5;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[4] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its copy and bag read");
            return;
        }
        if (!ParseInts(rest, 4, v)) return Invalid("the client's reading could not be read");
        if (v[0] != a.need) return Fail("the client's concrete copy holds %d units at the row past the commit, the host's holds %d", v[0], a.need);
        if (v[1] != 0) return Fail("%d concrete buckets stand on the client without an element id: a client-local one was made", v[1]);
        if (v[2] != a.bag0 - a.need) return Fail("the client's held bag holds %d units, it held %d and the mix takes %d", v[2], a.bag0, a.need);
        if (!v[3]) return Fail("the client still has a copy of the bucket the host replaced");
        return Pass("mix: the host replaced the bucket with a concrete bucket of the mix's units, the client's copy equals it with no "
                    "client-local concrete bucket, the old copy is gone and the held bag is spent by the same units");
    }
    }
}

void MixClient() {
    Mix& a = g_mix;
    void* const me = Local();
    if (!me) return;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(1, 1)) return;
        if (!Hold(me, ClientItem(0))) return Invalid("Hold Object did not run");
        a.cph = 1;
        Go();
        return;
    case 1: {
        if (!HandReady()) {
            Late("the hand never held the cement bag");
            return;
        }
        int units = 0, max = 0;
        if (!B::ReadBag(HB::LocalHandActor(), units, max)) return Invalid("the held bag cannot be read");
        a.oldEid = ClientEid(0);
        Say("c-ready " + std::to_string(units));
        a.cph = 2;
        Go();
        return;
    }
    case 2: {
        if (!Seen("go1")) {
            Late("the host never said go");
            return;
        }
        void* const b = ClientBucket(0);
        if (Approach(me, b) <= 0) return;
        const bool ran = b && HandUse(me, HB::LocalHandActor(), b, false);
        const bool opened = PendingIdOpen();
        Fact("the bag's left click on the bucket ran=%d, a request to the host open=%d", ran ? 1 : 0, opened ? 1 : 0);
        if (!ran) return Invalid("the bag's click did not run");
        if (!opened) return Invalid("the mix asked the host nothing: the game's own refusal ran or the watch is dead");
        a.cph = 3;
        Go();
        return;
    }
    case 3:
        if (!RecordClosed()) return;
        a.cph = 4;
        Go();
        return;
    default: {
        int v[4] = {};
        if (!RowPast("seq1 ", 4, v)) return;
        B::Concrete c;
        int units = 0, max = 0;
        void* const hand = HB::LocalHandActor();
        // A bag the mix emptied is gone from the hand: its units read 0.
        if (!B::ReadConcrete(LiveProp(v[0]), c) || (hand && !B::ReadBag(hand, units, max)))
            return Invalid("this client's copy of the concrete bucket cannot be read");
        Say("c-r1 " + Ints({c.units, NoEidConcrete(), units, LiveProp(v[3]) ? 0 : 1}));
        Finish();
        return;
    }
    }
}

// ================================================================ trowel and ctick ========================================

void TrowelHost(coop::net::Session& s) {
    Trowel& a = g_tw;
    const bool ctick = ArmOf() == Arm::Ctick;
    if (ctick && a.ph >= 6 && Seen("c-ctick-ran"))
        return Fail("a client's concrete bucket copy ran its own tick: its refusal did not hold");
    switch (a.ph) {
    case 0:
        if (!a.begun) {
            Spec sp;
            sp.plains = 1;
            sp.items[0] = L"prop_cementBag";
            sp.items[1] = L"prop_wallfixer";
            sp.items[2] = L"prop_wallfixer";
            sp.nItems = 3;
            // Behind the puppet, clear of every place the bucket may be set down (ItemClearOfSpots): nothing lies against the bucket to
            // jostle it and splash its water away.
            sp.itemFwdCm = -100.f;
            StageBegin(sp);
            a.begun = true;
        }
        if (!StageStep(s)) return;
        {
            void* const b = StageBucket(0);
            if (!b || !E::TryGetActorLocation(b, a.at)) return Invalid("the host's bucket cannot be read");
            a.before = LiveOf(L"prop_concreteBucket_C");
            a.use.item.Set(StageItem(0));
            a.use.target.Set(b);
        }
        a.ph = 1;
        Go();
        return;
    case 1: {   // the host's own mix makes the concrete bucket, once the placed bucket reads its water
        if (a.use.ph == 0) {
            // The place respawns the bucket and its own overlaps take a little water: it is read once it lies at rest, and the mix needs
            // the game's own gate (water, room for concrete, at least one unit), not a level.
            B::Plain p;
            void* const b = a.use.target.Get();
            if (!b || !B::ReadPlain(b, p)) return Invalid("the host's placed bucket cannot be read");
            if (!Calm(b, a.bucketCalm) || p.empty || !p.canConcrete || MixNeed(p) < 1) {
                if (Late("the placed bucket never lay at rest with water the mix can take"))
                    Fact("at the bound the placed bucket reads %.2f of %.2f cm, canConcrete=%d, empty=%d", static_cast<double>(p.height),
                         static_cast<double>(p.maxHeight), p.canConcrete ? 1 : 0, p.empty ? 1 : 0);
                return;
            }
            const int need = MixNeed(p);
            Fact("trowel: the placed bucket reads %.2f of %.2f cm, canConcrete=%d, the mix needs %d units", static_cast<double>(p.height),
                 static_cast<double>(p.maxHeight), p.canConcrete ? 1 : 0, need);
            if (!p.canConcrete || need < 1 || need > 20) return Invalid("the host's placed bucket cannot be mixed: no water or no room for concrete");
        }
        if (HostUseStep(a.use) <= 0) return;
        a.ph = 2;
        Go();
        return;
    }
    case 2: {
        void* const made = Born(L"prop_concreteBucket_C", a.before, a.at, 60.f);
        if (!made || EidOf(made) == 0 || StageBucket(0)) {
            Late("the host's own mix never left a concrete bucket with an element id");
            return;
        }
        a.conc.Set(made);
        a.concKey = KeyOf(made);
        StageSetPlains(0);
        a.ph = 3;
        Go();
        return;
    }
    case 3: {   // the bag is set down again, and the host takes units until two are left
        if (!HostDropStep(a.drop)) return;
        void* const x = StageItem(0);
        void* const c = a.conc.Get();
        B::Concrete cb;
        if (!x || !c || !B::ReadConcrete(c, cb) || !Calm(c, a.calm)) {
            Late("the bag never stood in the world again, or the concrete bucket never came to rest");
            return;
        }
        for (int guard = 0; cb.units > 2 && guard < 40; ++guard) {
            bool dry = false;
            if (!B::CallTakeConcrete(c, dry) || dry || !B::ReadConcrete(c, cb)) return Invalid("the host's own take from the concrete bucket did not run");
        }
        if (cb.units != 2) return Invalid("the concrete bucket could not be taken down to two units");
        a.units0 = cb.units;
        a.eidConc = EidOf(c);
        a.ph = 4;
        Go();
        return;
    }
    case 4:
        if (!StageSay()) return;
        {
            const uint32_t seq = Mark(a.conc.Get());
            if (!seq) return Invalid("the host's row of the concrete bucket could not be marked");
            Say("conc " + Ints({a.eidConc, static_cast<int>(seq), a.units0}));
        }
        a.ph = 5;
        Go();
        return;
    case 5:
        if (!Seen("c-ready")) {
            Late("the client never said it holds the first trowel");
            return;
        }
        a.round = RoundBegin();
        Fact("%s: the concrete bucket eid=%d holds %d units", ctick ? "ctick" : "trowel", a.eidConc, a.units0);
        Say("go1");
        a.ph = 6;
        Go();
        return;
    case 6: {
        if (RoundStep(a.round, "trowel") <= 0) return;
        void* const c = a.conc.Get();
        B::Concrete cb;
        if (!c || !B::ReadConcrete(c, cb)) return Invalid("the host's concrete bucket is gone after a take that left a unit");
        if (cb.units != a.units0 - 1)
            return Fail("the host's target did not move by its commit: the concrete bucket holds %d units, it held %d and a take removes one",
                        cb.units, a.units0);
        a.units0 = cb.units;
        Say("seq1 " + Ints({a.eidConc, static_cast<int>(BS::dev::CommitSeq(static_cast<uint32_t>(a.eidConc))), cb.units}));
        a.ph = 7;
        Go();
        return;
    }
    case 7: {
        std::string rest;
        int v[4] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its copy, trowel and tick counters read");
            return;
        }
        if (!ParseInts(rest, 4, v)) return Invalid("the client's reading could not be read");
        if (ctick) {
            if (v[2] <= 0) return Fail("a client's concrete bucket copy's tick was refused %d times: the refusal never saw it tick", v[2]);
            if (v[3] != 0) return Fail("a client's concrete bucket copy ran its own tick %d times", v[3]);
            return Pass("ctick: a client's concrete bucket copy's tick was refused and never ran, at the trowel's commit");
        }
        if (v[0] != a.units0) return Fail("the client's concrete copy holds %d units at the row past the commit, the host's holds %d", v[0], a.units0);
        if (v[1] != 5) return Fail("the client's held trowel carries %d after the take, the BP loads it with 5", v[1]);
        a.before = LiveOf(L"prop_bucket_C");
        a.round = RoundBegin();
        Say("go2");
        a.ph = 8;
        Go();
        return;
    }
    case 8: {   // the last unit: the concrete bucket is replaced by a plain one
        if (RoundStep(a.round, "trowel") <= 0) return;
        void* const plain = Born(L"prop_bucket_C", a.before, a.at, 80.f);
        if (!plain || a.conc.Get())
            return Fail("the host's target did not move by its commit: the last unit's take left the concrete bucket (%s) and %s plain bucket",
                        a.conc.Get() ? "standing" : "gone", plain ? "a" : "no");
        a.plain.Set(plain);
        a.ph = 9;
        Go();
        return;
    }
    case 9: {
        void* const plain = a.plain.Get();
        B::Plain p;
        if (!plain || !B::ReadPlain(plain, p)) return Invalid("the host's new plain bucket died");
        if (EidOf(plain) == 0) {
            Late("the host's new plain bucket never got an element id");
            return;
        }
        a.eidPlain = EidOf(plain);
        const uint32_t seq = Mark(plain);
        if (!seq) return Invalid("the host's row of the new plain bucket could not be marked");
        Say("seq2 " + Ints({a.eidPlain, static_cast<int>(seq), Centi(p.height)}));
        a.ph = 10;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[3] = {};
        if (!Rest("c-r2 ", &rest)) {
            Late("the client never said what its plain copy and trowel read");
            return;
        }
        if (!ParseInts(rest, 3, v)) return Invalid("the client's reading could not be read");
        B::Plain p;
        if (!B::ReadPlain(a.plain.Get(), p)) return Invalid("the host's new plain bucket died");
        if (v[0] != Centi(p.height))
            return Fail("the client's copy of the new plain bucket reads %.2f cm at the row past the commit, the host's reads %.2f cm",
                        static_cast<double>(v[0]) / 100.0, static_cast<double>(p.height));
        if (v[1] != 5) return Fail("the client's second trowel carries %d after the last take, the BP loads it with 5", v[1]);
        if (!v[2]) return Fail("the client still has a copy of the concrete bucket the last take replaced");
        return Pass("trowel: a unit out of the concrete bucket, then its last unit, each applied by the host; the last turned it into a "
                    "plain bucket, the client's copies equal the host's at each row past the commit, and each trowel was loaded");
    }
    }
}

void TrowelClient() {
    Trowel& a = g_tw;
    const bool ctick = ArmOf() == Arm::Ctick;
    void* const me = Local();
    if (!me) return;
    // A concrete copy that ran its own tick says so at once.
    if (ctick && !a.ranSaid && BS::CountOf(BS::Count::ConcreteTicksRan) > 0) {
        a.ranSaid = true;
        Say("c-ctick-ran");
    }
    switch (a.cph) {
    case 0: {
        int v[3] = {};
        if (!ClientBindStage(0, 3) || !RowPast("conc ", 3, v)) return;
        a.eidConc = v[0];
        if (!Hold(me, ClientItem(1))) return Invalid("Hold Object did not run");
        a.cph = 1;
        Go();
        return;
    }
    case 1:
        if (!HandReady()) {
            Late("the hand never held the trowel");
            return;
        }
        Say("c-ready");
        a.cph = 2;
        Go();
        return;
    case 2:
    case 7: {
        if (!Seen(a.cph == 2 ? "go1" : "go2")) {
            Late("the host never said go");
            return;
        }
        void* const c = LiveProp(a.eidConc);
        if (Approach(me, c) <= 0) return;
        const bool ran = c && HandUse(me, HB::LocalHandActor(), c, false);
        const bool opened = PendingIdOpen();
        Fact("the trowel's left click on the concrete bucket ran=%d, a request to the host open=%d", ran ? 1 : 0, opened ? 1 : 0);
        if (!ran) return Invalid("the trowel's click did not run");
        if (!opened) return Invalid("the take asked the host nothing: the game's own refusal ran or the watch is dead");
        a.cph += 1;
        Go();
        return;
    }
    case 3:
    case 8:
        if (!RecordClosed()) return;
        a.cph += 1;
        Go();
        return;
    case 4: {
        int v[3] = {};
        if (!RowPast("seq1 ", 3, v)) return;
        B::Concrete c;
        int load = 0;
        if (!B::ReadConcrete(LiveProp(a.eidConc), c) || !B::ReadTrowel(HB::LocalHandActor(), load))
            return Invalid("this client's copy or trowel cannot be read");
        Say("c-r1 " + Ints({c.units, load, static_cast<int>(BS::CountOf(BS::Count::ConcreteTicksRefused)),
                            static_cast<int>(BS::CountOf(BS::Count::ConcreteTicksRan))}));
        if (ctick) return Finish();
        a.cph = 5;
        Go();
        return;
    }
    case 5:   // the first trowel is set down, the second taken
        if (HandEmpty()) {
            if (!Hold(me, ClientItem(2))) return Invalid("Hold Object did not run");
            a.cph = 6;
            Go();
            return;
        }
        if (!a.dropped) {
            a.dropped = true;
            if (!DropHeld(me)) return Invalid("the drop of the first trowel did not run");
        }
        Late("the hand never emptied after the first trowel's drop");
        return;
    case 6:
        if (!HandReady()) {
            Late("the hand never held the second trowel");
            return;
        }
        a.cph = 7;
        Go();
        return;
    default: {
        int v[3] = {};
        if (!RowPast("seq2 ", 3, v)) return;
        B::Plain p;
        int load = 0;
        if (!B::ReadPlain(LiveProp(v[0]), p) || !B::ReadTrowel(HB::LocalHandActor(), load))
            return Invalid("this client's plain copy or trowel cannot be read");
        Say("c-r2 " + Ints({Centi(p.height), load, LiveProp(a.eidConc) ? 0 : 1}));
        Finish();
        return;
    }
    }
}

void ResetConcrete() {
    g_mix = Mix{};
    g_tw = Trowel{};
}

}  // namespace coop::dev::bucket_drill::detail
