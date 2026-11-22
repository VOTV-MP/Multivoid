// coop/dev/bucket_drill_join.cpp -- the join arm of the bucket drill; see coop/dev/bucket_drill.h.
//
// Before the client joins, the host (alone in its world) fills a bucket on the water, soaps it with its own soap, and lets a concrete
// bucket dry past 8 seconds (`dryTimer` is a field the world's save record lacks, so only the host's join send can carry it). It says
// `bucket_drill join STAGED` (the line the rig launches the client on). Once the joiner has its world and the host's join send has gone,
// the host dips the soaped bucket (soap branch, its own call) and says that row's seq, its readings and the seq its join send numbered
// the concrete bucket's row with. The joiner's copies equal the host's at that row: the soap, and the concrete bucket's drying clock within
// 5 seconds, and the first row the joiner applied for the concrete bucket is the join row, told by its seq from a periodic row that came
// first. The row of the dip comes after the join send on the one ordered lane, so a joiner whose first row for the concrete bucket is any
// other never had it from the join send (the red `snapshot` takes the seq and sends the joiner nothing at its edge).

#include "bucket_drill_detail.h"

#include "coop/net/session.h"
#include "coop/props/bucket_sync.h"

#include "ue_wrap/actors/bucket.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace coop::dev::bucket_drill::detail {

namespace B  = ue_wrap::bucket;
namespace BS = coop::bucket_sync;

namespace {

constexpr float kDryStageSec = 8.f;   // the host's clock the staging waits for: past the 5 s the comparison allows
constexpr int kDryToleranceSec = 5;

struct Join {
    bool begun = false;
    int ph = 0, cph = 0, calm = 0, drop = 0, soapHost = 0, dryHost = 0, dryFlagHost = 0, joinSeq = 0;
    HostUse use;
} g_j;

}  // namespace

void JoinHost(coop::net::Session& s) {
    Join& a = g_j;
    switch (a.ph) {
    case 0:
        if (!a.begun) {
            Spec sp;
            sp.plains = 1;
            sp.items[0] = L"prop_food_soap";
            sp.items[1] = L"prop_sponge";
            sp.items[2] = L"prop_concreteBucket";
            sp.nItems = 3;
            sp.nearHost = true;
            StageBegin(sp);
            a.begun = true;
        }
        if (!StageStep(s)) return;
        a.use.item.Set(StageItem(0));
        a.use.target.Set(StageBucket(0));
        a.use.soap = true;
        a.ph = 1;
        Go();
        return;
    case 1:   // the host's own soap on its bucket
        if (HostUseStep(a.use) <= 0) return;
        a.ph = 2;
        Go();
        return;
    case 2: {
        B::Plain p;
        B::Concrete c;
        if (!B::ReadPlain(StageBucket(0), p) || !B::ReadConcrete(StageItem(2), c)) return Invalid("the host's bucket or concrete bucket cannot be read");
        if (p.soap <= 0 || c.dryTimer < kDryStageSec) {
            Late("the host's soap never reached the bucket, or the concrete bucket never dried past 8 seconds");
            return;
        }
        if (!HostDropStep(a.drop)) return;
        Fact("bucket_drill join STAGED: bucket eid=%d holds soap %d, the concrete bucket eid=%d reads dryTimer %.1f s; the client may be launched",
             EidOf(StageBucket(0)), p.soap, EidOf(StageItem(2)), static_cast<double>(c.dryTimer));
        a.ph = 3;
        Go();
        return;
    }
    case 3:   // no bound of this drill's: the run's budget is the joiner's
        if (!HostMayStart(s)) return;
        {
            void* const b = StageBucket(0);
            void* const conc = StageItem(2);
            B::Plain p;
            B::Concrete c;
            bool dipped = false;
            a.joinSeq = b && conc ? static_cast<int>(BS::dev::JoinSeq(static_cast<uint32_t>(EidOf(conc)))) : 0;
            if (a.joinSeq == 0) {
                Late("the host's join send never numbered the concrete bucket's row");
                return;
            }
            if (!b || !conc || !B::CallDipped(b, StageItem(1), dipped) || !dipped || !B::ReadPlain(b, p) || !B::ReadConcrete(conc, c))
                return Invalid("the host's own dip of the soaped bucket did not run");
            const uint32_t seq = BS::MarkDirty(b);
            if (!seq) return Invalid("the host's row of the soaped bucket could not be marked");
            a.soapHost = p.soap;
            a.dryHost = static_cast<int>(std::lround(c.dryTimer));
            a.dryFlagHost = c.dry ? 1 : 0;
            Say("join-sent " + Ints({EidOf(b), static_cast<int>(seq), EidOf(conc), a.dryHost, a.dryFlagHost, a.soapHost, a.joinSeq}));
            Fact("the joiner has its world and the host's join send, which numbered the concrete bucket's row %d; the host's soaped bucket now "
                 "holds soap %d, its concrete bucket reads %d s", a.joinSeq, a.soapHost, a.dryHost);
        }
        a.ph = 4;
        Go();
        return;
    default: {
        std::string rest;
        int v[4] = {};
        if (!Rest("c-join ", &rest)) {
            Late("the joiner never said what its copies read");
            return;
        }
        if (!ParseInts(rest, 4, v)) return Invalid("the joiner's reading could not be read");
        const char* const red = BS::dev::RedOf() == BS::dev::Red::Snapshot ? " (bucket_red=snapshot: the host sent no join rows)" : "";
        if (v[0] != a.soapHost) return Fail("the joiner's soaped bucket holds soap %d at the row past the join, the host's holds %d%s", v[0], a.soapHost, red);
        if (v[3] != a.joinSeq)
            return Fail("the first row the joiner applied for the concrete bucket carries seq %d (0: none), the host's join send numbered its row %d: a "
                        "later row came first%s", v[3], a.joinSeq, red);
        if (std::abs(v[1] - a.dryHost) > kDryToleranceSec || v[2] != a.dryFlagHost)
            return Fail("the joiner's concrete bucket reads dryTimer %d s (dry %d) at the row past the join, the host's read %d s (dry %d)%s", v[1], v[2],
                        a.dryHost, a.dryFlagHost, red);
        return Pass("join: the joiner's soaped bucket and drying concrete bucket equal the host's at the row past the join send, the drying "
                    "clock within 5 seconds");
    }
    }
}

void JoinClient() {
    Join& a = g_j;
    if (!Local()) return;
    switch (a.cph) {
    case 0:
        if (!ClientMayStart()) return;
        a.cph = 1;
        Go();
        return;
    default: {
        int v[7] = {};
        std::string rest;
        if (!Rest("join-sent ", &rest)) {
            Late("the host never said its join send has gone");
            return;
        }
        if (!ParseInts(rest, 7, v)) return Invalid("the host's join line could not be read");
        void* const bucket = LiveProp(v[0]);
        void* const conc = LiveProp(v[2]);
        if (!bucket || !conc || BS::dev::AppliedSeq(static_cast<uint32_t>(v[0])) < static_cast<uint32_t>(v[1])) {
            Late("the joiner never had both copies bound with the row past the join send applied");
            return;
        }
        B::Plain pb;
        B::Concrete c;
        if (!B::ReadPlain(bucket, pb) || !B::ReadConcrete(conc, c)) return Invalid("the joiner's copies cannot be read");
        const int firstSeq = static_cast<int>(BS::dev::FirstAppliedSeq(static_cast<uint32_t>(v[2])));
        Fact("the joiner's soaped bucket holds soap %d, its concrete bucket reads dryTimer %.1f s and its first applied row carries seq %d", pb.soap,
             static_cast<double>(c.dryTimer), firstSeq);
        Say("c-join " + Ints({pb.soap, static_cast<int>(std::lround(c.dryTimer)), c.dry ? 1 : 0, firstSeq}));
        Finish();
        return;
    }
    }
}

void ResetJoin() { g_j = Join{}; }

}  // namespace coop::dev::bucket_drill::detail
