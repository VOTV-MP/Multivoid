// coop/dev/bucket_drill_verbs.cpp -- the soap, dip and transfer arms of the bucket drill; see coop/dev/bucket_drill.h.
//
// Each leg: the client holds the item and runs the verb as a player does (a call that reaches the family's watch, which asks the host);
// the host takes the client's commit and reads its own target against its value before plus the BP's own delta; it says the row seq
// its apply marked (bucket_sync::dev::CommitSeq); the client reads its copy once that row has reached it, and its held side, and says
// them; the host compares. A FAIL is said at the first leg that differs; the red `apply` leaves the host's target unmoved.

#include "bucket_drill_detail.h"

#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/props/bucket_sync.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/reflection.h"

#include <string>

namespace coop::dev::bucket_drill::detail {

namespace B  = ue_wrap::bucket;
namespace BS = coop::bucket_sync;
namespace PU = ue_wrap::prop_use;
namespace HB = coop::hand_item;

namespace {

uint32_t Seq(int eid) { return BS::dev::CommitSeq(static_cast<uint32_t>(eid)); }

// ---- soap ----

struct Soap {
    bool begun = false;
    int ph = 0, cph = 0, eid = 0, soap0 = 0, amount = 0;
    Round round;
} g_soap;

// ---- dip ----

struct Dip {
    bool begun = false;
    int ph = 0, cph = 0, eid = 0, h0 = 0, soap0 = 0, calm = 0, drop = 0;
    Round round;
    ue_wrap::CachedObjRef soapItem;
    HostUse use;
} g_dip;

// ---- transfer ----

struct Transfer {
    bool begun = false;
    int ph = 0, cph = 0, eidX = 0, units0 = 0, held0 = 0, calm = 0, drop = 0;
    Round round;
    HostUse use;
} g_tr;

}  // namespace

// ================================================================ soap ====================================================

void SoapHost(coop::net::Session& s) {
    Soap& a = g_soap;
    switch (a.ph) {
    case 0: {
        if (!a.begun) {
            Spec sp;
            sp.plains = 1;
            sp.items[0] = L"prop_food_soap";
            sp.nItems = 1;
            StageBegin(sp);
            a.begun = true;
        }
        if (!StageStep(s)) return;
        // The soap's amount is read while the host's copy stands: the client's hold takes that copy (a family 3 take destroys it).
        if (!PU::ReadSoapAmount(StageItem(0), a.amount)) return Invalid("the host's soap cannot be read before the client holds it");
        a.ph = 1;
        Go();
        return;
    }
    case 1:
        if (!StageSay()) return;
        a.ph = 2;
        Go();
        return;
    case 2: {
        if (!Seen("c-ready")) {
            Late("the client never said it holds the soap");
            return;
        }
        void* const b = StageBucket(0);
        B::Plain p;
        if (!b || !B::ReadPlain(b, p)) return Invalid("the host's bucket cannot be read");
        a.eid = EidOf(b);
        a.soap0 = p.soap;
        a.round = RoundBegin();
        Fact("soap: the bucket eid=%d holds soap %d, the soap item carries %d", a.eid, a.soap0, a.amount);
        Say("go1");
        a.ph = 3;
        Go();
        return;
    }
    case 3: {
        if (RoundStep(a.round, "soap") <= 0) return;
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the host's bucket cannot be read");
        if (p.soap != a.soap0 + a.amount)
            return Fail("the host's target did not move by its commit: soap %d, the bucket held %d and the soap carries %d", p.soap,
                        a.soap0, a.amount);
        Say("seq1 " + Ints({a.eid, static_cast<int>(Seq(a.eid)), p.soap}));
        a.ph = 4;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[2] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its copy and hand read");
            return;
        }
        if (!ParseInts(rest, 2, v)) return Invalid("the client's reading could not be read");
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the host's bucket cannot be read");
        if (v[0] != p.soap) return Fail("the client's copy reads soap %d at the row past the commit, the host's reads %d", v[0], p.soap);
        if (!v[1]) return Fail("the client's hand still holds the soap after its replay: its held side did not move");
        return Pass("soap: the host's bucket gained the soap's amount, the client's copy equals it at the row past the commit, and the "
                    "client's soap is spent");
    }
    }
}

void SoapClient() {
    Soap& a = g_soap;
    void* const me = Local();
    if (!me) return;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(1, 1)) return;
        if (!Hold(me, ClientItem(0))) return Invalid("Hold Object did not run");
        a.cph = 1;
        Go();
        return;
    case 1:
        if (!HandReady()) {
            Late("the hand never held the soap");
            return;
        }
        Say("c-ready");
        a.cph = 2;
        Go();
        return;
    case 2: {
        if (!Seen("go1")) {
            Late("the host never said go");
            return;
        }
        void* const b = ClientBucket(0);
        if (Approach(me, b) <= 0) return;
        const bool ran = b && UseOn(me, b);
        const bool opened = PendingIdOpen();
        Fact("the soap's playerUsedOn on the bucket ran=%d, a request to the host open=%d", ran ? 1 : 0, opened ? 1 : 0);
        if (!ran) return Invalid("playerUsedOn did not run");
        if (!opened) return Invalid("the soap's use asked the host nothing: the game's own refusal ran or the watch is dead");
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
        int v[3] = {};
        if (!RowPast("seq1 ", 3, v)) return;
        B::Plain p;
        if (!B::ReadPlain(ClientBucket(0), p)) return Invalid("this client's copy cannot be read");
        Fact("at the row past the commit the copy reads soap %d, the hand is %s", p.soap, HandEmpty() ? "empty" : "not empty");
        Say("c-r1 " + Ints({p.soap, HandEmpty() ? 1 : 0}));
        Finish();
        return;
    }
    }
}

// ================================================================ dip =====================================================
// One bucket, two legs. Leg 1 is the water branch on the plain bucket. Then the host soaps that same bucket by its own game use (a held soap's
// playerUsedOn on its own copy) and marks the row; the client waits until its copy has applied that row, and leg 2 is the soap branch on the same bucket.

void DipHost(coop::net::Session& s) {
    Dip& a = g_dip;
    switch (a.ph) {
    case 0: {
        if (!a.begun) {
            Spec sp;
            sp.plains = 1;
            sp.items[0] = L"prop_sponge";
            sp.nItems = 1;
            StageBegin(sp);
            a.begun = true;
        }
        if (!StageStep(s)) return;
        a.ph = 1;
        Go();
        return;
    }
    case 1:
        if (!StageSay()) return;
        a.ph = 2;
        Go();
        return;
    case 2: {
        if (!Seen("c-ready")) {
            Late("the client never said it holds the sponge");
            return;
        }
        B::Plain p;
        void* const bk = StageBucket(0);
        if (!B::ReadPlain(bk, p)) return Invalid("the bucket cannot be read");
        a.eid = EidOf(bk);
        a.h0 = Centi(p.height);
        a.soap0 = p.soap;
        a.round = RoundBegin();
        Fact("dip, water branch: the bucket eid=%d holds %.2f cm and soap %d", a.eid, static_cast<double>(p.height), p.soap);
        Say("go1");
        a.ph = 3;
        Go();
        return;
    }
    case 3: {
        if (RoundStep(a.round, "dip") <= 0) return;
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the bucket cannot be read");
        if (Centi(p.height) != a.h0 - 200 || p.soap != a.soap0)
            return Fail("the host's target did not move by its commit: water %.2f cm, soap %d; it held %.2f cm and soap %d and a dip "
                        "takes 2 cm", static_cast<double>(p.height), p.soap, static_cast<double>(a.h0) / 100.0, a.soap0);
        Say("seq1 " + Ints({a.eid, static_cast<int>(Seq(a.eid)), Centi(p.height)}));
        a.ph = 4;
        Go();
        return;
    }
    case 4: {
        std::string rest;
        int v[3] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its copy and sponge read");
            return;
        }
        if (!ParseInts(rest, 3, v)) return Invalid("the client's reading could not be read");
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the bucket cannot be read");
        if (v[0] != Centi(p.height))
            return Fail("the client's copy of the bucket reads %.2f cm at the row past the commit, the host's reads %.2f cm",
                        static_cast<double>(v[0]) / 100.0, static_cast<double>(p.height));
        if (v[2] != 0) return Fail("the client's sponge carries soap %d after a water dip", v[2]);
        // The soap branch needs soap in the bucket: the host's own use of a soap puts it there.
        void* const soap = SpawnBefore(L"prop_food_soap", Local(), 150.f, 0.f);
        if (!soap) return Invalid("the host's soap did not spawn");
        a.soapItem.Set(soap);
        a.ph = 5;
        Go();
        return;
    }
    case 5: {
        void* const soap = a.soapItem.Get();
        if (!soap) return Invalid("the host's soap died before its use");
        if (EidOf(soap) == 0 || !Calm(soap, a.calm)) {
            Late("the host's soap never got an element id and came to rest");
            return;
        }
        a.use.item.Set(soap);
        a.use.target.Set(StageBucket(0));
        a.use.soap = true;
        a.ph = 6;
        Go();
        return;
    }
    case 6: {
        const int r = HostUseStep(a.use);
        if (r <= 0) return;
        a.ph = 7;
        Go();
        return;
    }
    case 7: {
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the bucket cannot be read");
        if (p.soap <= 0) {
            Late("the host's own soap never reached the bucket");
            return;
        }
        if (!HostDropStep(a.drop)) return;
        void* const bk = StageBucket(0);
        const uint32_t seq = BS::MarkDirty(bk);
        if (!seq) return Invalid("the host's row of the soaped bucket could not be marked");
        a.h0 = Centi(p.height);
        a.soap0 = p.soap;
        a.round = RoundBegin();
        Fact("dip, soap branch: the bucket eid=%d holds %.2f cm and soap %d after the host's own soap, its row carries seq %u", a.eid,
             static_cast<double>(p.height), p.soap, seq);
        Say("seqs " + Ints({a.eid, static_cast<int>(seq), p.soap}));
        a.ph = 8;
        Go();
        return;
    }
    case 8: {
        if (RoundStep(a.round, "dip") <= 0) return;
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the bucket cannot be read");
        if (p.soap != a.soap0 - 1 || Centi(p.height) != a.h0)
            return Fail("the host's target did not move by its commit: soap %d, water %.2f cm; it held soap %d and a soap dip takes one",
                        p.soap, static_cast<double>(p.height), a.soap0);
        Say("seq2 " + Ints({a.eid, static_cast<int>(Seq(a.eid)), p.soap}));
        a.ph = 9;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[2] = {};
        if (!Rest("c-r2 ", &rest)) {
            Late("the client never said what its copy and sponge read");
            return;
        }
        if (!ParseInts(rest, 2, v)) return Invalid("the client's reading could not be read");
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the bucket cannot be read");
        if (v[0] != p.soap) return Fail("the client's copy of the bucket reads soap %d at the row past the commit, the host's reads %d", v[0], p.soap);
        if (v[1] != 100) return Fail("the client's sponge carries soap %d after a soap dip, the BP gives it 100", v[1]);
        return Pass("dip: the host's bucket lost 2 cm on the water branch and one soap on the soap branch, the client's copy equals the "
                    "host's at each row past the commit, and the sponge took the soap");
    }
    }
}

namespace {

// The client's dip of the sponge in the copy of bucket `slot`: the walk to a standpoint of the bucket, the camera turned until the player's own trace names it, then the
// sponge's own left click, which traces and calls the bucket's dipped. False while working or stopped.
bool ClientDip(void* me, int slot, int& cph, const char* what) {
    void* const b = ClientBucket(slot);
    void* const hand = HB::LocalHandActor();
    if (!b || !hand) {
        Invalid("this client's bucket copy or its sponge is gone");
        return false;
    }
    if (Approach(me, b) <= 0) return false;
    std::string why;
    const int aim = Aim(me, b, why);
    if (aim < 0) {   // no pose of the fan from this standpoint names the bucket: the next standpoint
        ApproachRetry(why);
        AimReset();
        return false;
    }
    if (aim == 0) {
        if (!Ended()) Late("the camera never turned until the player's trace named the bucket");
        return false;
    }
    const bool ran = B::CallHandUseLmb(hand, me);
    const bool opened = PendingIdOpen();
    Fact("%s: the sponge's left click ran=%d, a request to the host open=%d", what, ran ? 1 : 0, opened ? 1 : 0);
    if (!ran) {
        Invalid("the sponge's left click did not run");
        return false;
    }
    if (!opened) {
        Invalid("the dip asked the host nothing: the game's own refusal ran, the trace missed the bucket or the watch is dead");
        return false;
    }
    cph = cph + 1;
    Go();
    return true;
}

}  // namespace

void DipClient() {
    Dip& a = g_dip;
    void* const me = Local();
    if (!me) return;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(1, 1)) return;
        if (!Hold(me, ClientItem(0))) return Invalid("Hold Object did not run");
        a.cph = 1;
        Go();
        return;
    case 1:
        if (!HandReady()) {
            Late("the hand never held the sponge");
            return;
        }
        Say("c-ready");
        AimReset();
        a.cph = 2;
        Go();
        return;
    case 2:
        if (!Seen("go1")) {
            Late("the host never said go");
            return;
        }
        ClientDip(me, 0, a.cph, "water branch");
        return;
    case 3:
        if (!RecordClosed()) return;
        a.cph = 4;
        Go();
        return;
    case 4: {
        int v[3] = {};
        if (!RowPast("seq1 ", 3, v)) return;
        B::Plain p;
        float sponge = 0.f;
        if (!B::ReadPlain(ClientBucket(0), p) || !PU::ReadSpongeSoap(HB::LocalHandActor(), sponge))
            return Invalid("this client's copy or sponge cannot be read");
        Say("c-r1 " + Ints({Centi(p.height), p.soap, static_cast<int>(sponge)}));
        a.cph = 5;
        Go();
        return;
    }
    case 5: {   // the host's own soap in the bucket: the copy has applied the row past it before the soap branch is clicked
        int v[3] = {};
        if (!RowPast("seqs ", 3, v)) return;
        B::Plain p;
        if (!B::ReadPlain(ClientBucket(0), p)) return Invalid("this client's copy cannot be read");
        if (p.soap != v[2])
            return Invalid("this client's copy reads a different soap than the host's row past its own soap carried");
        AimReset();
        a.cph = 6;
        Go();
        return;
    }
    case 6:
        ClientDip(me, 0, a.cph, "soap branch");
        return;
    case 7:
        if (!RecordClosed()) return;
        a.cph = 8;
        Go();
        return;
    default: {
        int v[3] = {};
        if (!RowPast("seq2 ", 3, v)) return;
        B::Plain p;
        float sponge = 0.f;
        if (!B::ReadPlain(ClientBucket(0), p) || !PU::ReadSpongeSoap(HB::LocalHandActor(), sponge))
            return Invalid("this client's copy or sponge cannot be read");
        Say("c-r2 " + Ints({p.soap, static_cast<int>(sponge)}));
        Finish();
        return;
    }
    }
}

// ================================================================ transfer ================================================

void TransferHost(coop::net::Session& s) {
    Transfer& a = g_tr;
    switch (a.ph) {
    case 0: {
        if (!a.begun) {
            Spec sp;
            sp.plains = 1;   // the bucket the host's own mix uses up, so the world bag holds fewer units than it can
            sp.items[0] = L"prop_cementBag";
            sp.items[1] = L"prop_cementBag";
            sp.nItems = 2;
            StageBegin(sp);
            a.begun = true;
        }
        if (!StageStep(s)) return;
        a.use.item.Set(StageItem(0));
        a.use.target.Set(StageBucket(0));
        a.ph = 1;
        Go();
        return;
    }
    case 1:   // the host's player mixes bag X into the bucket: the game's own verb, X is left with units to give
        if (HostUseStep(a.use) <= 0) return;
        a.ph = 2;
        Go();
        return;
    case 2:
        if (StageBucket(0)) {
            Late("the host's own mix never used the bucket up");
            return;
        }
        StageSetPlains(0);
        a.ph = 3;
        Go();
        return;
    case 3: {   // X is set down again by the host's own drop
        if (!HostDropStep(a.drop)) return;
        void* const x = StageItem(0);
        int units = 0, max = 0;
        if (!x || !B::ReadBag(x, units, max) || !Calm(x, a.calm)) {
            Late("bag X never stood in the world again");
            return;
        }
        a.ph = 4;
        Go();
        return;
    }
    case 4:
        if (!StageSay()) return;
        a.ph = 5;
        Go();
        return;
    case 5: {
        std::string rest;
        if (!Rest("c-ready ", &rest)) {
            Late("the client never said it holds bag Y");
            return;
        }
        if (!ParseInts(rest, 1, &a.held0)) return Invalid("the client's ready line could not be read");
        void* const x = StageItem(0);
        int max = 0;
        if (!B::ReadBag(x, a.units0, max) || a.units0 <= 0 || a.units0 >= max)
            return Invalid("bag X holds no units to take or no room to give");
        a.eidX = EidOf(x);
        a.round = RoundBegin();
        Fact("transfer: world bag X eid=%d holds %d units of %d, the client's bag holds %d", a.eidX, a.units0, max, a.held0);
        Say("go1");
        a.ph = 6;
        Go();
        return;
    }
    case 6: {
        if (RoundStep(a.round, "transfer") <= 0) return;
        int units = 0, max = 0;
        if (!B::ReadBag(StageItem(0), units, max)) return Invalid("bag X cannot be read");
        if (units != a.units0 + 1)
            return Fail("the host's target did not move by its commit: the world bag holds %d units, it held %d and a unit given adds one",
                        units, a.units0);
        Say("seq1 " + Ints({a.eidX, static_cast<int>(Seq(a.eidX)), units}));
        a.ph = 7;
        Go();
        return;
    }
    case 7: {
        std::string rest;
        int v[2] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its bags read");
            return;
        }
        if (!ParseInts(rest, 2, v)) return Invalid("the client's reading could not be read");
        int units = 0, max = 0;
        if (!B::ReadBag(StageItem(0), units, max)) return Invalid("bag X cannot be read");
        if (v[0] != units) return Fail("the client's copy of bag X holds %d units at the row past the commit, the host's holds %d", v[0], units);
        if (v[1] != a.held0 - 1) return Fail("the client's held bag holds %d units after giving one, it held %d", v[1], a.held0);
        a.units0 = units;
        a.held0 = v[1];
        a.round = RoundBegin();
        Say("go2");
        a.ph = 8;
        Go();
        return;
    }
    case 8: {
        if (RoundStep(a.round, "transfer") <= 0) return;
        int units = 0, max = 0;
        if (!B::ReadBag(StageItem(0), units, max)) return Invalid("bag X cannot be read");
        if (units != a.units0 - 1)
            return Fail("the host's target did not move by its commit: the world bag holds %d units, it held %d and a unit taken removes one",
                        units, a.units0);
        Say("seq2 " + Ints({a.eidX, static_cast<int>(Seq(a.eidX)), units}));
        a.ph = 9;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[2] = {};
        if (!Rest("c-r2 ", &rest)) {
            Late("the client never said what its bags read");
            return;
        }
        if (!ParseInts(rest, 2, v)) return Invalid("the client's reading could not be read");
        int units = 0, max = 0;
        if (!B::ReadBag(StageItem(0), units, max)) return Invalid("bag X cannot be read");
        if (v[0] != units) return Fail("the client's copy of bag X holds %d units at the row past the commit, the host's holds %d", v[0], units);
        if (v[1] != a.held0 + 1) return Fail("the client's held bag holds %d units after taking one, it held %d", v[1], a.held0);
        return Pass("transfer: a unit given to the world bag and a unit taken from it, each applied by the host, the client's copy equal at "
                    "each row past the commit, and the client's held bag moved by the same unit");
    }
    }
}

void TransferClient() {
    Transfer& a = g_tr;
    void* const me = Local();
    if (!me) return;
    int units = 0, max = 0;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(0, 2)) return;
        if (!Hold(me, ClientItem(1))) return Invalid("Hold Object did not run");
        a.cph = 1;
        Go();
        return;
    case 1:
        if (!HandReady()) {
            Late("the hand never held bag Y");
            return;
        }
        if (!B::ReadBag(HB::LocalHandActor(), units, max)) return Invalid("the held bag cannot be read");
        Say("c-ready " + std::to_string(units));
        a.cph = 2;
        Go();
        return;
    case 2:
    case 5: {
        const bool rmb = a.cph == 5;
        if (!Seen(rmb ? "go2" : "go1")) {
            Late("the host never said go");
            return;
        }
        void* const x = ClientItem(0);
        if (Approach(me, x) <= 0) return;
        const bool ran = x && HandUse(me, HB::LocalHandActor(), x, rmb);
        const bool opened = PendingIdOpen();
        Fact("the held bag's %s click on the world bag ran=%d, a request to the host open=%d", rmb ? "right" : "left", ran ? 1 : 0,
             opened ? 1 : 0);
        if (!ran) return Invalid("the held bag's click did not run");
        if (!opened) return Invalid("the transfer asked the host nothing: the game's own refusal ran or the watch is dead");
        a.cph += 1;
        Go();
        return;
    }
    case 3:
    case 6:
        if (!RecordClosed()) return;
        a.cph += 1;
        Go();
        return;
    default: {
        const bool second = a.cph == 7;
        int v[3] = {};
        if (!RowPast(second ? "seq2 " : "seq1 ", 3, v)) return;
        int cu = 0;
        if (!B::ReadBag(ClientItem(0), cu, max) || !B::ReadBag(HB::LocalHandActor(), units, max))
            return Invalid("this client's bags cannot be read");
        Say(std::string(second ? "c-r2 " : "c-r1 ") + Ints({cu, units}));
        if (second) return Finish();
        a.cph = 5;
        Go();
        return;
    }
    }
}

void ResetVerbs() {
    g_soap = Soap{};
    g_dip = Dip{};
    g_tr = Transfer{};
}

}  // namespace coop::dev::bucket_drill::detail
