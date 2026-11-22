// coop/dev/bucket_drill_alc.cpp -- the alc and brew arms of the bucket drill; see coop/dev/bucket_drill.h.
//
// Both: the host fills a bucket on the water, carries a sugar and a bread and sets each down inside it by the game's own place, and
// reads the bucket's `itemsInside` (a bucket that does not hold them on the host is INVALID); the client's copies of the items are
// frozen, so its own `itemsInside` never lists them and is not read. alc: once the host says its items stand inside, the client calls
// `deleteAlc` on its copy; the lane refuses it (the refusal counter moves) and the body's POST never runs. The red `alc` takes the
// refusal off: the body runs and its POST is the FAIL. brew (its own run of about ten minutes): the host's own brew flips `alcohol` and
// destroys the items once; the client's copy reads `alcohol` at the row past the flip and its copies of the items are gone; then the
// client fills a held clean jar from the brewed bucket (family 7), judged as the soap arm is.

#include "bucket_drill_detail.h"

#include "coop/cheats/item_catalog.h"
#include "coop/cheats/item_spawn.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/props/bucket_sync.h"
#include "coop/props/prop_drive_host.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::bucket_drill::detail {

namespace B  = ue_wrap::bucket;
namespace BS = coop::bucket_sync;
namespace E  = ue_wrap::engine;
namespace HB = coop::hand_item;
namespace PD = coop::prop_drive_host;
namespace R  = ue_wrap::reflection;
namespace IS = coop::cheats::item_spawn;
namespace IC = coop::cheats::item_catalog;

namespace {

constexpr const char* kJarRow = "cleanjar";
constexpr auto kBrewBound = std::chrono::seconds(900);   // the host's own brew after the items went in: past it the run says INVALID
constexpr float kLoafPitch = 90.f;       // the bread's placement ghost, so the loaf lies flat in the bucket

struct Alc {
    bool begun = false;
    int ph = 0, cph = 0, calm = 0, jarCalm = 0, eidJar = 0, drop = 0;
    PlaceJob job[2];
    int placing = 0;
    int h0 = 0;
    float heightF = 0.f;
    Round round;
    std::vector<void*> jarBefore;
    ue_wrap::CachedObjRef jar;
    int jarMade = 0;
    std::chrono::steady_clock::time_point brewFrom;   // the setup's itemsInside reading
} g_a;

bool Contains(void* const* list, size_t n, void* x) { return x && std::find(list, list + n, x) != list + n; }

// The bucket's overlap box: its world centre and scaled half-extent.
bool ReadBox(void* bucket, ue_wrap::FVector& centre, ue_wrap::FVector& half) {
    const int32_t off = bucket ? R::FindPropertyOffset(R::ClassOf(bucket), L"Box") : -1;
    void* comp = nullptr;
    if (off >= 0) std::memcpy(&comp, static_cast<const char*>(bucket) + off, sizeof(comp));
    void* const fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetScaledBoxExtent") : nullptr;
    ue_wrap::ParamFrame frame(fn);
    if (!comp || !fn || !frame.valid() || !ue_wrap::Call(comp, frame) || !E::TryGetComponentLocation(comp, centre)) return false;
    half = frame.Get<ue_wrap::FVector>(L"ReturnValue");
    return true;
}

// The element ids whose drive the host closed with an End since the arm began (prop_drive_host's End listener): a prop's own End is its
// rest. An element id is never reused, so an End of the item's old actor cannot stand for the placed one's.
std::vector<uint32_t> g_ended;

void OnDriveEnd(uint32_t eid, uint32_t) {
    constexpr size_t kMaxEnded = 256;
    if (g_ended.size() < kMaxEnded) g_ended.push_back(eid);
}

bool EndedEid(int eid) { return eid > 0 && std::find(g_ended.begin(), g_ended.end(), static_cast<uint32_t>(eid)) != g_ended.end(); }

// What the bucket's `itemsInside` and box hold, for a verdict that says the items were not inside.
void InsideReadings(void* bucket, void* sugar, void* bread) {
    void* inside[16] = {};
    size_t total = 0;
    ue_wrap::FVector at{}, s{}, b{}, centre{}, half{};
    B::Plain p;
    const bool read = B::ReadItemsInside(bucket, inside, 16, total);
    const size_t n = std::min<size_t>(total, 16);
    const bool haveBox = ReadBox(bucket, centre, half);
    const bool haveBucket = bucket && E::TryGetActorLocation(bucket, at) && B::ReadPlain(bucket, p);
    Fact("itemsInside read=%d total=%zu, the sugar %s and the bread %s listed; the bucket z=%.1f height=%.1f; the sugar z=%.1f, the bread z=%.1f",
         read ? 1 : 0, total, Contains(inside, n, sugar) ? "is" : "is not", Contains(inside, n, bread) ? "is" : "is not",
         haveBucket ? static_cast<double>(at.Z) : -1.0, haveBucket ? static_cast<double>(p.maxHeight) : -1.0,
         sugar && E::TryGetActorLocation(sugar, s) ? static_cast<double>(s.Z) : -1.0,
         bread && E::TryGetActorLocation(bread, b) ? static_cast<double>(b.Z) : -1.0);
    if (haveBox)
        Fact("the bucket's box: centre (%.1f,%.1f,%.1f), half-extent (%.1f,%.1f,%.1f)", static_cast<double>(centre.X), static_cast<double>(centre.Y),
             static_cast<double>(centre.Z), static_cast<double>(half.X), static_cast<double>(half.Y), static_cast<double>(half.Z));
    else
        Fact("the bucket's box did not read");
}

// Both items stand in the bucket's box, as this peer reads it.
bool BothInside(void* bucket, void* sugar, void* bread) {
    void* inside[16] = {};
    size_t total = 0;
    if (!B::ReadItemsInside(bucket, inside, 16, total)) return false;
    const size_t n = std::min<size_t>(total, 16);
    return Contains(inside, n, sugar) && Contains(inside, n, bread);
}

}  // namespace

// ================================================================ host ====================================================

void AlcHost(coop::net::Session& s) {
    Alc& a = g_a;
    const bool brew = ArmOf() == Arm::Brew;
    switch (a.ph) {
    case 0:
        if (!a.begun) {
            Spec sp;
            sp.plains = 1;
            sp.items[0] = L"prop_sugar";
            sp.items[1] = L"prop_cookingFood_bread";
            sp.nItems = 2;
            StageBegin(sp);
            PD::SetEndListener(&OnDriveEnd);
            a.begun = true;
        }
        if (!StageStep(s)) return;
        a.ph = 1;
        Go();
        return;
    case 1: {   // each item is carried and set down inside the bucket by the game's place, the sugar low and the bread above it
        if (a.placing >= 2) {
            a.ph = 2;
            Go();
            return;
        }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(StageBucket(0), at)) return Invalid("the host's bucket cannot be read");
        at.Z += 20.f + 35.f * static_cast<float>(a.placing);
        // The loaf is set down lying (pitch 90).
        if (a.placing == 1) a.job[1].rot = ue_wrap::FRotator{kLoafPitch, 0.f, 0.f};
        const int r = PlaceHeld(a.job[a.placing], StageItem(a.placing), &at, 1);
        if (r > 0) {
            ++a.placing;
            Go();
        }
        return;
    }
    case 2: {
        // The game lists a prop only once it rests (its drive has ended): itemsInside is read from the second both items' drives end.
        const bool sugarEnded = EndedEid(EidOf(StageItem(0))), breadEnded = EndedEid(EidOf(StageItem(1)));
        if (!sugarEnded || !breadEnded || !BothInside(StageBucket(0), StageItem(0), StageItem(1))) {
            if (Late("the sugar and the bread were not both inside the bucket on the host after their drives ended")) {
                Fact("the sugar's drive ended=%d, the bread's ended=%d", sugarEnded ? 1 : 0, breadEnded ? 1 : 0);
                InsideReadings(StageBucket(0), StageItem(0), StageItem(1));
            }
            return;
        }
        Fact("the sugar and the bread stand inside the bucket on the host");
        Say("items-in");
        a.brewFrom = std::chrono::steady_clock::now();
        a.ph = 3;
        Go();
        return;
    }
    case 3:
        if (!StageSay()) return;
        a.ph = 4;
        Go();
        return;
    case 4: {
        std::string rest;
        int v[3] = {};
        if (brew) {
            a.ph = 6;   // the host's own brew is the event; its own bound (kBrewBound) says INVALID, never the run's budget
            Go();
            return;
        }
        // The client's wait for this line is the shorter bound, so the client says its result first.
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its deleteAlc did", 2 * kBoundTicks);
            return;
        }
        if (!ParseInts(rest, 3, v)) return Invalid("the client's reading could not be read");
        if (!v[1]) return Invalid("the client's deleteAlc did not run");
        if (v[2] > 0) return Fail("a client copy's deleteAlc ran its body (%d POSTs, refused %d times)", v[2], v[0]);
        if (v[0] < 1) return Invalid("the client's deleteAlc left no POST and the lane's refusal counter never moved: the call was not seen");
        if (!StageItem(0) || !StageItem(1)) return Fail("the host's bread or sugar is gone after the client's deleteAlc");
        return Pass("alc: a client copy's deleteAlc was refused by the lane and its body never ran");
    }
    case 6: {   // the host's own brew
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the host's bucket cannot be read");
        if (!p.alcohol) {
            if (std::chrono::steady_clock::now() - a.brewFrom > kBrewBound)
                Invalid("the host's own brew did not flip alcohol within 900 s of the sugar and the bread going in");
            return;
        }
        if (StageItem(0) || StageItem(1)) {
            Late("the brew flipped alcohol and left the bread or the sugar standing");
            return;
        }
        void* const b = StageBucket(0);
        const uint32_t seq = BS::MarkDirty(b);
        if (!seq) return Invalid("the host's row of the brewed bucket could not be marked");
        Fact("brew: the host's own brew flipped alcohol and destroyed the sugar and the bread");
        Say("seq1 " + Ints({EidOf(b), static_cast<int>(seq), Centi(p.height)}));
        a.ph = 7;
        Go();
        return;
    }
    case 7: {
        std::string rest;
        int v[2] = {};
        if (!Rest("c-r1 ", &rest)) {
            Late("the client never said what its copy read after the brew");
            return;
        }
        if (!ParseInts(rest, 2, v)) return Invalid("the client's reading could not be read");
        if (!v[0]) return Fail("the client's copy does not read alcohol at the row past the host's brew");
        if (!v[1]) return Fail("the client still has a copy of the bread or the sugar the host's brew destroyed");
        a.ph = 8;
        Go();
        return;
    }
    case 8: {   // the jar leg: a clean jar, spawned beside the client's puppet
        std::wstring canonical;
        if (IC::Resolve(kJarRow, canonical) == IC::Lookup::Unfilled) {
            Late("the game never filled its prop list");
            return;
        }
        std::wstring cls;
        if (!IS::SpawnClassName(kJarRow, cls)) return Invalid("the game lists no clean jar prop to spawn");
        a.jarBefore = LiveOf(cls.c_str());
        IS::Spawn(kJarRow, 1, kSlot, false, &a.jarMade);
        a.ph = 9;
        Go();
        return;
    }
    case 9: {
        std::wstring cls;
        IS::SpawnClassName(kJarRow, cls);
        void* jar = nullptr;
        for (void* x : LiveOf(cls.c_str()))
            if (std::find(a.jarBefore.begin(), a.jarBefore.end(), x) == a.jarBefore.end()) jar = x;
        if (!jar || EidOf(jar) == 0 || !Calm(jar, a.jarCalm)) {
            Late("the clean jar never got an element id and came to rest");
            return;
        }
        a.jar.Set(jar);
        a.eidJar = EidOf(jar);
        Say("jar " + std::to_string(a.eidJar));
        a.ph = 10;
        Go();
        return;
    }
    case 10: {
        if (!Seen("c-jar-ready")) {
            Late("the client never said it holds the clean jar");
            return;
        }
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the host's bucket cannot be read");
        a.heightF = p.height;
        a.h0 = Centi(p.height);
        if (!p.alcohol || !(p.height > p.maxHeight / 3.f)) return Invalid("the brewed bucket holds too little water for a jar");
        a.round = RoundBegin();
        Say("go-jar");
        a.ph = 11;
        Go();
        return;
    }
    case 11: {
        if (RoundStep(a.round, "jar") <= 0) return;
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the host's bucket cannot be read");
        const float want = std::max(a.heightF - p.maxHeight / 2.4f, 0.f);
        if (Centi(p.height) != Centi(want))
            return Fail("the host's target did not move by its commit: the brewed bucket holds %.2f cm, it held %.2f cm and a jar takes a "
                        "share of %.2f cm", static_cast<double>(p.height), static_cast<double>(a.heightF), static_cast<double>(p.maxHeight / 2.4f));
        const int eid = EidOf(StageBucket(0));
        Say("seqj " + Ints({eid, static_cast<int>(BS::dev::CommitSeq(static_cast<uint32_t>(eid))), Centi(p.height)}));
        a.ph = 12;
        Go();
        return;
    }
    default: {
        std::string rest;
        int v[2] = {};
        if (!Rest("c-rj ", &rest)) {
            Late("the client never said what its copy and hand read after the jar");
            return;
        }
        if (!ParseInts(rest, 2, v)) return Invalid("the client's reading could not be read");
        B::Plain p;
        if (!B::ReadPlain(StageBucket(0), p)) return Invalid("the host's bucket cannot be read");
        if (v[0] != Centi(p.height)) return Fail("the client's copy reads %.2f cm at the row past the jar, the host's reads %.2f cm", static_cast<double>(v[0]) / 100.0, static_cast<double>(p.height));
        if (!v[1]) return Fail("the client's hand still holds the jar after its replay: its held side did not move");
        return Pass("brew: the host's brew reached the client's copy and destroyed the sugar and the bread once, and a jar filled from the "
                    "brewed bucket took its share of the water on the host, the client's copy equal and the jar spent");
    }
    }
}

// ================================================================ client ==================================================

void AlcClient() {
    Alc& a = g_a;
    const bool brew = ArmOf() == Arm::Brew;
    void* const me = Local();
    if (!me) return;
    switch (a.cph) {
    case 0:
        if (!ClientBindStage(1, 2)) return;
        a.cph = 1;
        Go();
        return;
    case 1: {   // the host says its items stand inside; the copy's own list is not read (the copies of the items are frozen)
        if (brew) {
            a.cph = 3;
            Go();
            return;
        }
        // The shorter bound: the host's wait for this client's result is twice as long.
        if (!AlcWatchSettled() || !Seen("items-in")) {
            Late("the host never said its sugar and bread stand inside the bucket, or the drill's watch on deleteAlc never settled", kBoundTicks / 2);
            return;
        }
        void* const copy = ClientBucket(0);
        if (!copy) return Invalid("this client's copy of the bucket is gone");
        AlcWatchArm(copy);
        const unsigned long long base = BS::CountOf(BS::Count::DeleteAlcRefused);
        const bool ran = B::CallDeleteAlc(copy);
        const unsigned long long refused = BS::CountOf(BS::Count::DeleteAlcRefused) - base;
        const unsigned long long posts = AlcPosts();
        Fact("deleteAlc on the copy ran=%d, refused %llu times, its POST ran %llu times", ran ? 1 : 0, refused, posts);
        Say("c-r1 " + Ints({static_cast<int>(refused), ran ? 1 : 0, static_cast<int>(posts)}));
        Finish();
        return;
    }
    case 3: {
        int v[3] = {};
        if (!RowPast("seq1 ", 3, v, false)) return;
        B::Plain p;
        if (!B::ReadPlain(ClientBucket(0), p)) return Invalid("this client's copy cannot be read");
        const bool gone = !LiveProp(ClientEid(kMaxPlains)) && !LiveProp(ClientEid(kMaxPlains + 1));
        Say("c-r1 " + Ints({p.alcohol ? 1 : 0, gone ? 1 : 0}));
        a.cph = 4;
        Go();
        return;
    }
    case 4: {   // the jar
        std::string rest;
        int eid = 0;
        if (!Rest("jar ", &rest)) {
            Late("the host never said the clean jar is here");
            return;
        }
        if (!ParseInts(rest, 1, &eid)) return Invalid("the host's jar line could not be read");
        void* const jar = LiveProp(eid);
        if (!jar || !Calm(jar, a.jarCalm)) {
            Late("this client's copy of the clean jar never stood here");
            return;
        }
        if (!Hold(me, jar)) return Invalid("Hold Object did not run");
        a.cph = 5;
        Go();
        return;
    }
    case 5:
        if (!HandReady()) {
            Late("the hand never held the clean jar");
            return;
        }
        Say("c-jar-ready");
        a.cph = 6;
        Go();
        return;
    case 6: {
        if (!Seen("go-jar")) {
            Late("the host never said go");
            return;
        }
        void* const b = ClientBucket(0);
        if (Approach(me, b) <= 0) return;
        const bool ran = b && UseOn(me, b);
        const bool opened = PendingIdOpen();
        Fact("the jar's playerUsedOn on the brewed bucket ran=%d, a request to the host open=%d", ran ? 1 : 0, opened ? 1 : 0);
        if (!ran) return Invalid("playerUsedOn did not run");
        if (!opened) return Invalid("the jar's use asked the host nothing: the game's own refusal ran or the watch is dead");
        a.cph = 7;
        Go();
        return;
    }
    case 7:
        if (!RecordClosed()) return;
        a.cph = 8;
        Go();
        return;
    default: {
        int v[3] = {};
        if (!RowPast("seqj ", 3, v)) return;
        B::Plain p;
        if (!B::ReadPlain(ClientBucket(0), p)) return Invalid("this client's copy cannot be read");
        Say("c-rj " + Ints({Centi(p.height), HandEmpty() ? 1 : 0}));
        Finish();
        return;
    }
    }
}

void ResetAlc() {
    PD::SetEndListener(nullptr);
    g_ended.clear();
    g_a = Alc{};
}

}  // namespace coop::dev::bucket_drill::detail
