// coop/dev/floppy_selftest.cpp -- see coop/dev/floppy_selftest.h.

#include "coop/dev/floppy_selftest.h"

#include "floppy_selftest_world.h"   // co-located private header: the driver's world half

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/serverbox.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::floppy_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace PR = ue_wrap::prop;
namespace SB = ue_wrap::serverbox;

namespace FD = ue_wrap::floppy_disc;  // the disc prop's own content fields
namespace W  = coop::dev::floppy_selftest::world;

using W::BoxSlot;
using W::ReadBoxSlot;
using W::kTargets;
using W::kDiscs;
using W::kMarker;
using W::kMarkerReadWrites;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::floppy_selftest);
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The episodes run in order from the first tick at which BOTH peers are in the world, and none of
// them waits on a clock. The peer that acts fires an episode when the state it needs is true
// here -- the disc in this world and the slot empty for an insert, the slot holding that disc for
// an eject -- and each peer moves past an episode when it SEES its effect here. A schedule did this
// before, and its every gap was a guess at the lane's speed: two roles' clocks ten seconds apart
// once had an eject read an empty slot. What stays timed is what is measured over time: the
// survival window below, and the deadline past which a state that never came is the failure.
constexpr uint64_t kCensusMs = 6000;    // the periodic census line: a log, not a milestone
constexpr uint64_t kPollMs   = 250;     // readiness reads; finding a disc walks the object array
constexpr uint64_t kNearMs   = 5000;    // the discs around the local player (the ERROR trace)
// The SECOND look at an ejected disc, this long after it appeared here: the re-swallow takes about
// a second, so "the eject produced a disc" is not "the disc survived". The next episode waits for
// this window to close on its own peer. That also keeps the one pair that shares a disc honest --
// the host re-inserting what the client's eject handed back -- since the host sees that disc after
// the client does, and so closes its window after the client's late sample, never across it.
constexpr uint64_t kLateMs   = 10000;
// A precondition or an effect still false this long after its episode became current never came:
// the episode fails there, named, and so does the run, since every later episode stands on it.
constexpr uint64_t kStepDeadlineMs = 30000;

enum class Verb { Insert, Eject };

// The `box` an episode names when its device is the base laptop rather than a signal server.
constexpr int kLaptop = -1;

struct Step {
    const char* id;
    bool        onHost;   // the role that fires it; the other role only watches
    int         box;
    int         disc;     // the disc this episode moves: the one inserted, or the one expected out
    Verb        verb;
    const char* under_test;
};

// One row per episode half. `under_test` is printed with the outcome, so one log says whether what
// happened is the thing the episode was built to catch.
const Step kSteps[] = {
    { "E1-insert", false, 0,  0, Verb::Insert,
      "a client insert: the disc dies here and the destroy is what crosses" },
    { "E1-eject",  true,  0,  0, Verb::Eject,
      "the host ejecting a client's insert: an empty slot means the transfer never crossed" },
    { "E2-insert", true,  1,  1, Verb::Insert,
      "a host insert: the slot state is authored on the host" },
    { "E2-eject",  false, 1,  1, Verb::Eject,
      "the client ejecting a host insert: an empty slot means the slot state never crossed" },
    { "E4-insert", true,  1,  1, Verb::Insert,
      "the host re-inserting the disc E2 handed back: the same-peer control needs its own insert" },
    { "E4-eject",  true,  1,  1, Verb::Eject,
      "the host ejecting its own insert: the disc and its content come back here" },
    { "E3-insert", false, 2,  2, Verb::Insert,
      "a client insert whose eject is the same client's" },
    { "E3-eject",  false, 2,  2, Verb::Eject,
      "a client eject: the disc is born on the client, through its own place seam" },
    // The repeats of E1, one fresh disc each, so the run answers "how often" and not "did it once".
    { "R2-insert", false, 0, 3, Verb::Insert, "repeat 2 of the cross-peer eject: insert" },
    { "R2-eject",  true,  0, 3, Verb::Eject,  "repeat 2 of the cross-peer eject" },
    { "R3-insert", false, 0, 4, Verb::Insert, "repeat 3 of the cross-peer eject: insert" },
    { "R3-eject",  true,  0, 4, Verb::Eject,  "repeat 3 of the cross-peer eject" },
    { "R4-insert", false, 0, 5, Verb::Insert, "repeat 4 of the cross-peer eject: insert" },
    { "R4-eject",  true,  0, 5, Verb::Eject,  "repeat 4 of the cross-peer eject" },
    { "R5-insert", false, 0, 6, Verb::Insert, "repeat 5 of the cross-peer eject: insert" },
    { "R5-eject",  true,  0, 6, Verb::Eject,  "repeat 5 of the cross-peer eject" },
    { "R6-insert", false, 0, 7, Verb::Insert, "repeat 6 of the cross-peer eject: insert" },
    { "R6-eject",  true,  0, 7, Verb::Eject,  "repeat 6 of the cross-peer eject" },
    // The laptop, where a live run's disc came back blank under a new key (bug 19). Its slot
    // content crossed as one blob cut at 4 KB, so L1 has the client eject from its copy of a slot
    // past that size; L2 is the same disc size ejected by the host, whose copy is its own.
    { "L1-insert", true,  kLaptop, 8,  Verb::Insert,
      "a host laptop insert of a disc whose slot content is past the old 4 KB blob cut" },
    { "L1-eject",  false, kLaptop, 8,  Verb::Eject,
      "the client ejecting from ITS copy of that slot, which is what crossed" },
    { "L2-insert", true,  kLaptop, 9,  Verb::Insert,
      "the same size into the laptop by the host: the control's insert" },
    { "L2-eject",  true,  kLaptop, 9,  Verb::Eject,
      "the host ejecting its own insert: its copy of the slot is its own" },
    { "L3-insert", false, kLaptop, 10, Verb::Insert,
      "a client laptop insert of a one-row disc: the client-to-host slot path" },
    { "L3-eject",  true,  kLaptop, 10, Verb::Eject,
      "the host ejecting a client's laptop insert" },
};
constexpr size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

// `lateLive` and `lateIntact` are the readings the acceptance turns on: not whether the eject
// produced a disc, but whether the disc is STILL there, carrying its content, when the survival
// window closes. The re-swallow takes about a second, so the first sighting alone can catch a disc
// that is already doomed. -1 = never sampled.
// `preLive` is what keeps the ratio honest. An eject whose named disc is ALREADY lying in this
// peer's world is not ejecting that disc -- its insert never consumed it -- so the episode measures
// nothing and must not be counted as a survival.
struct Outcome {
    bool        done       = false;
    bool        fired      = false;  // the verb was dispatched AND the game acted on it
    int         lateLive   = -1;     // -1 not sampled, 0 gone, 1 present
    int         lateIntact = -1;     // -1 not sampled, 0 emptied or gone, 1 the seeded content
    int         preLive    = -1;     // eject only: -1 not sampled, 0 the disc was away, 1 still here
    std::string note;                // the refusal reason, or what the slot did
};
Outcome g_outcome[kStepCount];

uint64_t g_connectedAtMs = 0;
uint64_t g_nextCensusMs  = 0;
uint64_t g_nextNearMs    = 0;

// Where this peer is in the episode list. PRE: the acting peer waits for its state, the watching
// peer goes straight to EFFECT. EFFECT: both wait to see the episode's outcome here. WINDOW: an
// eject's survival window. The deadline runs from the moment the current phase began.
enum class Phase { Pre, Effect, Window };
size_t      g_cur          = 0;
Phase       g_phase        = Phase::Pre;
uint64_t    g_phaseSinceMs = 0;
uint64_t    g_lateAtMs     = 0;
uint64_t    g_nextPollMs   = 0;
const char* g_deadStep     = nullptr;  // the episode that never became true here, if one did
std::string g_deadWhy;
bool        g_finished     = false;    // the verdict, the result and the done line are out

// The fast-destroy watch: during an eject episode, whether the disc that came out was destroyed
// again within moments. That is the re-swallow's signature and nothing else's -- no player is in
// the room, and the only other actor that can reach the disc is a device.
//
// TWO cadences, because one cannot do it. FINDING the disc costs an object-array walk, so it runs
// on a throttle; WATCHING it must not miss, because the whole defect is that the disc lives for
// well under a second -- a quarter-second poll walked straight past it, and this counter read ZERO
// through a run in which the disc was measurably eaten. So the moment the walk finds it, the actor
// is held in a slot-validated ref and its liveness is read EVERY tick, which touches no memory and
// costs nothing. The first sighting is also the eject's effect on this peer, which is why the
// episode reads it from here rather than from its own quarter-second readiness poll.
constexpr uint64_t kFastWatchMs = 4000;   // watched this long from its first sighting
constexpr uint64_t kFastFindMs  =  100;   // the object-array walk, until the disc is found
struct EjectWatch {
    std::wstring          key;
    uint64_t              nextFind  = 0;
    uint64_t              seenAtMs  = 0;  // 0 until the disc is first seen here
    ue_wrap::CachedObjRef actor;          // held from the first sighting; Alive() is a slot read
    bool                  counted   = false;
    int                   step      = -1;
};
EjectWatch g_fastWatch;
int        g_fastLost = 0;
bool     g_seeded        = false;
bool     g_picked        = false;

// ---- the episodes ------------------------------------------------------------------------------

// Sight the disc an EJECT episode names, at the moment the verb runs, on BOTH roles -- the peer
// that fires and the peer that only watches. A disc already lying here was never consumed by its
// insert, so this eject cannot be handing it back and the episode must not count as a survival.
// Recorded on the firing side alone, the watching peer's ratio counted five episodes that had
// moved nothing as five successes.
void PreSight(size_t i, bool isHost) {
    const Step& s = kSteps[i];
    if (s.verb != Verb::Eject) return;
    if (s.disc < 0 || s.disc >= kDiscs || W::DiscKey(s.disc).empty()) return;
    void* already = PR::FindByKeyString(W::DiscKey(s.disc));
    g_outcome[i].preLive = (already && R::IsLive(already)) ? 1 : 0;
    if (g_outcome[i].preLive == 1)
        UE_LOGW("floppy_selftest: %s PRE-SIGHTED role=%s box=%d -- the disc key='%ls' this episode "
                "names is ALREADY in this peer's world, so its insert never consumed it and this "
                "eject speaks for some other content. Excluded from the survival ratio.",
                s.id, isHost ? "HOST" : "CLIENT", s.box, W::DiscKey(s.disc).c_str());
}

// The device an episode acts on, its slot and its two verbs: a signal server by target index, or
// the laptop.
void* DeviceOf(const Step& s) { return s.box == kLaptop ? W::Laptop() : W::Box(s.box); }
const wchar_t* DeviceName(const Step& s) {
    return s.box == kLaptop ? L"laptop" : W::BoxName(s.box).c_str();
}
bool ReadDeviceSlot(const Step& s, void* dev, BoxSlot& out) {
    return s.box == kLaptop ? W::ReadLaptopSlot(out) : ReadBoxSlot(dev, out);
}
bool CallInsert(const Step& s, void* dev, void* disc) {
    return s.box == kLaptop ? ue_wrap::laptop::CallInsertDisc(disc)
                            : SB::CallProcessFloppy(dev, disc);
}
bool CallEject(const Step& s, void* dev) {
    return s.box == kLaptop ? ue_wrap::laptop::CallEjectDisc() : SB::CallEjectFloppy(dev);
}

void Fire(size_t i) {
    const Step& s = kSteps[i];
    Outcome& o = g_outcome[i];
    o.done = true;
    void* box = DeviceOf(s);
    if (!box) {
        o.note = "the target device does not resolve";
        UE_LOGW("floppy_selftest: %s NOT FIRED -- %s", s.id, o.note.c_str());
        return;
    }
    BoxSlot before{};
    ReadDeviceSlot(s, box, before);

    if (s.verb == Verb::Insert) {
        if (W::DiscKey(s.disc).empty()) {
            o.note = "no disc was named for this episode";
            UE_LOGW("floppy_selftest: %s NOT FIRED -- %s", s.id, o.note.c_str());
            return;
        }
        void* disc = PR::FindByKeyString(W::DiscKey(s.disc));
        if (!disc) {
            o.note = "the named disc is not in this peer's world";
            UE_LOGW("floppy_selftest: %s NOT FIRED -- %s (key='%ls')", s.id, o.note.c_str(),
                    W::DiscKey(s.disc).c_str());
            return;
        }
        FD::DiscContent dc;
        FD::ReadDiscContent(disc, dc);
        // The game refuses a busy slot outright ("Floppy disc slot is busy"), and the refusal is
        // silent to us: the verb dispatches, the slot keeps the value it had, and reading the
        // AFTER value alone says "filled". Say it here, before the verb, so the episode is on
        // record as having moved nothing.
        const bool wasBusy = before.floppyType >= 0;
        const bool called = CallInsert(s, box, disc);
        BoxSlot after{};
        ReadDeviceSlot(s, box, after);
        const bool discGone = !R::IsLive(disc);
        o.fired = called;
        UE_LOGI("floppy_selftest: %s %s box=%d '%ls' disc key='%ls' rw=%d rows=%zu -- slot type "
                "%d -> %d rw %d -> %d rows %d -> %d json %d -> %d, disc destroyed=%d (%s)",
                s.id, called ? "FIRED" : "CALL REFUSED", s.box, DeviceName(s),
                W::DiscKey(s.disc).c_str(), dc.readWrites, dc.data.size(), before.floppyType,
                after.floppyType, before.readWrites, after.readWrites, before.dataNum,
                after.dataNum, before.objectDataLen, after.objectDataLen, discGone ? 1 : 0,
                s.under_test);
        const bool tookContent = after.objectDataLen > before.objectDataLen ||
                                 after.dataNum > before.dataNum ||
                                 after.readWrites != before.readWrites;
        if (wasBusy) {
            o.fired = false;
            o.note = "the slot was ALREADY OCCUPIED -- the game refuses a busy slot";
            UE_LOGW("floppy_selftest: %s REFUSED box=%d '%ls' -- the slot already held type %d "
                    "before the verb, so the game answered its busy hint and this episode moved "
                    "NOTHING. Its eject below has nothing of this run's to hand back, and a box "
                    "that stays occupied after a cross-peer eject is the re-swallow itself.",
                    s.id, s.box, DeviceName(s), before.floppyType);
        } else if (after.floppyType >= 0) {
            o.note = "slot filled";
        } else if (tookContent) {
            o.note = "the slot took the content but no type";
            UE_LOGW("floppy_selftest: %s left the slot TYPELESS -- the box holds the disc's data "
                    "and its type is still %d, and every eject gates on a type, so this disc "
                    "cannot come back out on any peer", s.id, after.floppyType);
        } else {
            o.note = "the slot did not change";
            UE_LOGW("floppy_selftest: %s changed NOTHING in the slot -- the TRIGGER is inert (a "
                    "busy slot or a zip disc refuses the insert), so nothing downstream of it can "
                    "be read from this run", s.id);
        }
        return;
    }

    // An empty slot here is not a failure of the instrument: it is the outcome the episode
    // exists to record, and the game answers it with its own refusal hint.
    if (before.floppyType < 0) {
        o.fired = false;
        o.note = "the slot was EMPTY at eject time";
        UE_LOGW("floppy_selftest: %s SLOT EMPTY box=%d '%ls' -- nothing to eject, the game answers "
                "its no-disc hint (%s)", s.id, s.box, DeviceName(s), s.under_test);
        return;
    }
    const bool called = CallEject(s, box);
    BoxSlot after{};
    ReadDeviceSlot(s, box, after);
    o.fired = called;
    o.note = "slot drained";
    UE_LOGI("floppy_selftest: %s %s box=%d '%ls' -- slot type %d -> %d rw %d -> %d rows %d -> %d "
            "json %d -> %d; the disc arrives on the out-timeline (%s)",
            s.id, called ? "FIRED" : "CALL REFUSED", s.box, DeviceName(s),
            before.floppyType, after.floppyType, before.readWrites, after.readWrites,
            before.dataNum, after.dataNum, before.objectDataLen, after.objectDataLen,
            s.under_test);
}

// After an episode, what THIS peer can see of the disc it names: whether the actor is here, and
// whether the content on it is the seeded content. A cross-peer eject is proven by this line on
// the peer that did not eject, and by nothing else -- the box census says where the state is, and
// only this says whether the disc came back carrying it.
void ReportContent(size_t stepIdx, bool isHost, const char* when, bool isLate) {
    const Step& s = kSteps[stepIdx];
    if (s.disc < 0 || s.disc >= kDiscs) return;
    const std::wstring& key = W::DiscKey(s.disc);
    if (key.empty()) return;
    const char* role = isHost ? "HOST" : "CLIENT";

    void* actor = PR::FindByKeyString(key);
    const bool live = actor && R::IsLive(actor);
    const bool judged = isLate && s.verb == Verb::Eject;
    if (judged) {
        g_outcome[stepIdx].lateLive = live ? 1 : 0;
        g_outcome[stepIdx].lateIntact = 0;   // raised below only for the seeded content
    }
    if (s.verb == Verb::Insert) {
        UE_LOGI("floppy_selftest: CONTENT[%s] %s role=%s -- disc key='%ls' is %s here (an insert "
                "consumes it, so PRESENT on the peer that did not insert means the destroy has "
                "not landed yet)", when, s.id, role, key.c_str(), live ? "PRESENT" : "gone");
        return;
    }
    if (!live) {
        UE_LOGW("floppy_selftest: CONTENT[%s] %s role=%s -- the ejected disc key='%ls' is NOT in "
                "this peer's world: the eject produced no actor here", when, s.id, role,
                key.c_str());
        return;
    }
    FD::DiscContent c;
    if (!FD::ReadDiscContent(actor, c)) {
        UE_LOGW("floppy_selftest: CONTENT[%s] %s role=%s -- disc key='%ls' is here but its content "
                "fields did not read", when, s.id, role, key.c_str());
        return;
    }
    bool marked = false;
    for (const std::wstring& row : c.data)
        if (row.find(kMarker) != std::wstring::npos) { marked = true; break; }
    const int32_t wantRw = kMarkerReadWrites + s.disc;
    // The row count too: a cut copy keeps the marker, which is row 0, and loses the tail.
    const size_t wantRows = static_cast<size_t>(W::ExpectedRows(s.disc));
    if (marked && c.readWrites == wantRw && c.data.size() == wantRows) {
        if (judged) g_outcome[stepIdx].lateIntact = 1;
        UE_LOGI("floppy_selftest: CONTENT[%s] %s role=%s -- disc key='%ls' came back INTACT "
                "(rw=%d rows=%zu, marker present)", when, s.id, role, key.c_str(), c.readWrites,
                c.data.size());
        return;
    }
    UE_LOGW("floppy_selftest: CONTENT[%s] %s role=%s -- disc key='%ls' came back EMPTIED: rw=%d "
            "(seeded %d) rows=%zu (seeded %zu) marker=%s -- the actor crossed and its save data "
            "did not", when, s.id, role, key.c_str(), c.readWrites, wantRw, c.data.size(),
            wantRows, marked ? "present" : "ABSENT");
}

// ---- readiness -------------------------------------------------------------------------------

// The key the episode's device holds in its slot, empty when the slot is empty or does not read.
std::wstring SlotKeyOf(const Step& s) {
    void* dev = DeviceOf(s);
    return dev ? W::SlotDiscKey(dev, s.box == kLaptop) : std::wstring();
}

bool SlotEmpty(const Step& s) {
    void* dev = DeviceOf(s);
    BoxSlot st{};
    return dev && ReadDeviceSlot(s, dev, st) && st.floppyType < 0;
}

// What the ACTING peer needs before its verb: an insert, the disc here and the slot empty; an
// eject, the slot holding that disc -- which for a cross-peer pair is the other peer's insert
// having crossed. The laptop must also have finished sliding its last disc: both its verbs return
// at once while its slot timeline runs, and an eject fired a second after an insert moved nothing.
// `why` names the part that is still false.
bool Ready(const Step& s, const char*& why) {
    const std::wstring& key = W::DiscKey(s.disc);
    if (s.box == kLaptop && ue_wrap::laptop::FloppyBusy()) {
        why = "the laptop's slot is still moving a disc";
        return false;
    }
    if (s.verb == Verb::Insert) {
        void* disc = key.empty() ? nullptr : PR::FindByKeyString(key);
        if (!disc || !R::IsLive(disc)) { why = "the named disc is not in this peer's world"; return false; }
        if (!SlotEmpty(s)) { why = "the slot is not empty"; return false; }
        return true;
    }
    if (SlotKeyOf(s) != key) { why = "the slot does not hold the named disc"; return false; }
    return true;
}

// Both roles watch every eject, because the destroy that kills the disc is raised on the peer that
// did NOT eject and relayed to the one that did: watching only the actor here would name the
// symptom on one side and miss its cause on the other.
void ArmFastWatch(size_t i) {
    g_fastWatch = EjectWatch{};
    g_fastWatch.key  = W::DiscKey(kSteps[i].disc);
    g_fastWatch.step = static_cast<int>(i);
}

// What BOTH peers wait to see here once the verb is due. An insert: the slot holding the disc --
// or, on the peer that only watches, the disc gone from its world, since the insert's destroy
// lands ahead of its slot state and stays true after it, where a peer that inserts and ejects in
// one breath can carry the slot past a quarter-second poll. An eject: the slot empty and the disc
// out in this world, first seen by the fast watch, which the watching peer arms the moment its
// slot empties -- a device refuses a disc while its slot is occupied, so no re-swallow can come
// before that, and finding a disc walks every object in the world.
bool EffectSeen(size_t i, bool mine, const char*& why) {
    const Step& s = kSteps[i];
    if (s.verb == Verb::Insert) {
        if (SlotKeyOf(s) == W::DiscKey(s.disc)) return true;
        void* disc = mine ? nullptr : PR::FindByKeyString(W::DiscKey(s.disc));
        if (!mine && !(disc && R::IsLive(disc))) return true;
        why = "the slot never held the inserted disc";
        return false;
    }
    if (!SlotEmpty(s)) { why = "the slot never emptied"; return false; }
    if (g_fastWatch.step != static_cast<int>(i)) ArmFastWatch(i);
    if (!g_fastWatch.seenAtMs) {
        why = "the ejected disc never appeared in this peer's world";
        return false;
    }
    return true;
}

void WatchFastDestroy(bool isHost, uint64_t now) {
    if (g_fastWatch.step < 0) return;
    const char* id = kSteps[g_fastWatch.step].id;
    const char* role = isHost ? "HOST" : "CLIENT";
    if (!g_fastWatch.seenAtMs) {
        if (now < g_fastWatch.nextFind) return;
        g_fastWatch.nextFind = now + kFastFindMs;
        void* a = g_fastWatch.key.empty() ? nullptr : PR::FindByKeyString(g_fastWatch.key);
        if (!a || !R::IsLive(a)) return;
        g_fastWatch.seenAtMs = now;
        g_fastWatch.actor.Set(a);
        UE_LOGI("floppy_selftest: FAST-WATCH %s role=%s -- disc key='%ls' appeared here; watching "
                "it every tick for %llu ms", id, role, g_fastWatch.key.c_str(),
                static_cast<unsigned long long>(kFastWatchMs));
        return;
    }
    if (now - g_fastWatch.seenAtMs > kFastWatchMs) return;   // the late sample takes it from here
    if (g_fastWatch.counted || g_fastWatch.actor.Alive()) return;
    g_fastWatch.counted = true;
    ++g_fastLost;
    UE_LOGW("floppy_selftest: FAST-DESTROY %s role=%s -- disc key='%ls' was in this peer's world "
            "after the eject and was GONE %llu ms later. Nobody is holding it and no player is near "
            "it, so a device took it: that is the re-swallow.", id, role, g_fastWatch.key.c_str(),
            static_cast<unsigned long long>(now - g_fastWatch.seenAtMs));
}

// The run's end on this peer, once: the final census, the episode table, this peer's judgement and
// the line the rig ends on. PASS is every episode reached, and every ejected disc still here and
// carrying its seeded content when its window closed, with nothing eaten on the way.
void Finish(bool isHost) {
    if (g_finished) return;
    g_finished = true;
    const char* role = isHost ? "HOST" : "CLIENT";
    W::Census("final", isHost, NowMs() - g_connectedAtMs);
    EmitVerdict();
    int ejects = 0, intact = 0, unproven = 0;
    for (size_t i = 0; i < kStepCount; ++i) {
        if (kSteps[i].verb != Verb::Eject) continue;
        ++ejects;
        if (g_outcome[i].preLive == 1) ++unproven;
        else if (g_outcome[i].lateIntact == 1) ++intact;
    }
    const bool pass = !g_deadStep && intact == ejects && g_fastLost == 0;
    const std::string died = g_deadStep ? std::string("; the run died at ") + g_deadStep + ": " + g_deadWhy
                                        : std::string();
    if (pass) {
        UE_LOGI("floppy_selftest: RESULT PASS role=%s -- every episode ran; %d of %d ejected discs "
                "were here with their seeded content when the window closed", role, intact, ejects);
    } else {
        UE_LOGW("floppy_selftest: RESULT FAIL role=%s -- %d of %d ejected discs intact here, %d "
                "eject(s) whose disc was never consumed, %d fast destroy(s)%s", role, intact,
                ejects, unproven, g_fastLost, died.c_str());
    }
    UE_LOGI("floppy_selftest: DONE role=%s", role);
}

// An episode whose state never came: said once with what was still false, and the run ends here.
void Die(bool isHost, const char* id, const char* why, uint64_t now) {
    g_deadStep = id;
    g_deadWhy = why;
    UE_LOGW("floppy_selftest: EPISODE DEAD %s role=%s -- %s, %llu ms after it became current; "
            "every later episode stands on it", id, isHost ? "HOST" : "CLIENT", why,
            static_cast<unsigned long long>(now - g_phaseSinceMs));
    Finish(isHost);
}

// One phase of the current episode per call. Every read here walks for a disc or a slot, so the
// PRE and EFFECT reads run on the poll period, not every tick.
void Advance(bool isHost, uint64_t now) {
    if (g_cur >= kStepCount) return;
    const Step& s = kSteps[g_cur];
    const bool mine = s.onHost == isHost;
    const char* why = "";
    switch (g_phase) {
    case Phase::Pre:
        if (!mine) {
            // The watching peer cannot see the verb, only what it does, and it sights an eject's
            // disc where the verb runs: before it.
            PreSight(g_cur, isHost);
            g_phase = Phase::Effect;
            g_phaseSinceMs = now;
            return;
        }
        if (now < g_nextPollMs) return;
        g_nextPollMs = now + kPollMs;
        if (!Ready(s, why)) {
            if (now - g_phaseSinceMs > kStepDeadlineMs) Die(isHost, s.id, why, now);
            return;
        }
        PreSight(g_cur, isHost);
        W::Census("before", isHost, NowMs() - g_connectedAtMs);
        if (s.verb == Verb::Eject) ArmFastWatch(g_cur);
        Fire(g_cur);
        if (!g_outcome[g_cur].fired) {
            Die(isHost, s.id, g_outcome[g_cur].note.c_str(), now);
            return;
        }
        g_phase = Phase::Effect;
        g_phaseSinceMs = now;
        return;
    case Phase::Effect:
        if (now < g_nextPollMs) return;
        g_nextPollMs = now + kPollMs;
        if (!EffectSeen(g_cur, mine, why)) {
            if (now - g_phaseSinceMs > kStepDeadlineMs) Die(isHost, s.id, why, now);
            return;
        }
        W::Census(s.id, isHost, NowMs() - g_connectedAtMs);
        ReportContent(g_cur, isHost, "post", false);
        if (s.verb == Verb::Eject) {
            g_lateAtMs = g_fastWatch.seenAtMs + kLateMs;
            g_phase = Phase::Window;
            return;
        }
        break;
    case Phase::Window:
        if (now < g_lateAtMs) return;
        ReportContent(g_cur, isHost, "late", true);
        break;
    }
    ++g_cur;
    g_phase = Phase::Pre;
    g_phaseSinceMs = now;
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
}

void EmitVerdict() {
    if (!Enabled() || !g_connectedAtMs) return;
    auto* s = g_session.load(std::memory_order_acquire);
    const bool isHost = s && s->role() == coop::net::Role::Host;
    int fired = 0, refused = 0, notReached = 0;
    for (size_t i = 0; i < kStepCount; ++i) {
        const Outcome& o = g_outcome[i];
        const char* state = !o.done ? "NOT REACHED" : (o.fired ? "fired" : "did not fire");
        if (!o.done) ++notReached;
        else if (o.fired) ++fired;
        else ++refused;
        // Only the role that owns an episode can say anything about it; the other peer prints the
        // row as not its own rather than as a gap.
        const bool mine = kSteps[i].onHost == isHost;
        UE_LOGI("floppy_selftest: VERDICT %-10s %-12s %s%s", kSteps[i].id,
                mine ? state : "not this role", mine && !o.note.empty() ? "-- " : "",
                mine ? o.note.c_str() : "");
    }
    UE_LOGI("floppy_selftest: VERDICT role=%s fired=%d did-not-fire=%d not-reached=%d of %zu "
            "episodes (a not-reached row measured NOTHING)", isHost ? "HOST" : "CLIENT", fired,
            refused, notReached, kStepCount);
    // The ratio the cross-peer eject is repeated for. Both roles report it: on the peer that
    // ejected it says whether its own disc survived, on the other whether the disc ever arrived
    // and stayed, and the two together are the whole answer.
    // The repeated pair is COUNTED off the schedule, never written down beside it: a hand-kept
    // number is a claim that goes stale the first time a row moves.
    int ejects = 0, survived = 0, sampled = 0, repeats = 0;
    for (size_t i = 0; i < kStepCount; ++i) {
        if (kSteps[i].verb != Verb::Eject) continue;
        ++ejects;
        if (kSteps[i].onHost && kSteps[i].box == 0) ++repeats;   // the cross-peer pair
        if (g_outcome[i].lateLive < 0 || g_outcome[i].preLive == 1) continue;
        ++sampled;
        if (g_outcome[i].lateLive == 1) ++survived;
    }
    int inserts = 0, admitted = 0;
    for (size_t i = 0; i < kStepCount; ++i) {
        if (kSteps[i].verb != Verb::Insert || !g_outcome[i].done) continue;
        ++inserts;
        if (g_outcome[i].fired) ++admitted;
    }
    UE_LOGI("floppy_selftest: INSERT ADMISSION role=%s -- %d of %d insert episodes this role "
            "reached found an EMPTY slot and moved their disc; %d found the slot already occupied, "
            "which the game refuses. A box that is still occupied when the next repeat comes round "
            "was refilled by something, and after a cross-peer eject the only candidate is the "
            "other peer's copy of that box.", isHost ? "HOST" : "CLIENT", admitted, inserts,
            inserts - admitted);
    UE_LOGI("floppy_selftest: EJECT SURVIVAL role=%s -- %d of %d sampled eject episodes still had "
            "their disc in THIS peer's world %llus after it first appeared (%d eject episodes total, %d "
            "not counted -- never sampled, or the disc was already lying here when the verb ran); "
            "fast-destroys seen here = %d. The cross-peer pair is repeated %d times, so a survival "
            "short of the sampled count is a RATE, not an anecdote.",
            isHost ? "HOST" : "CLIENT", survived, sampled,
            static_cast<unsigned long long>(kLateMs / 1000), ejects, ejects - sampled, g_fastLost,
            repeats);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const bool isHost = s->role() == coop::net::Role::Host;
    const uint64_t now = NowMs();
    // The shared origin: this peer is in the world, and so is the other one.
    if (!g_connectedAtMs) {
        // The two roles ask different questions: only a CLIENT announces ClientWorldReady, and
        // the host learns the same fact as that slot going world-ready.
        if (isHost ? !s->IsSlotWorldReady(1) : !coop::net_pump::HasAnnouncedWorldReady()) return;
        g_connectedAtMs = now;
        g_nextCensusMs = now + kCensusMs;
        g_phaseSinceMs = now;
        UE_LOGI("floppy_selftest: ARMED role=%s -- %zu episodes, each on its own readiness; an "
                "episode not ready within %llus fails the run", isHost ? "HOST" : "CLIENT",
                kStepCount, static_cast<unsigned long long>(kStepDeadlineMs / 1000));
    }
    // Past the run too: a player looking at the discs after it is who this is for.
    if (now >= g_nextNearMs) {
        g_nextNearMs = now + kNearMs;
        W::NearCensus(isHost, NowMs() - g_connectedAtMs);
    }
    if (g_finished) return;
    if (!FD::EnsureResolved()) return;  // the disc fields come from there
    if (!W::ResolveBoxes()) return;

    if (!g_seeded) {
        g_seeded = true;
        if (isHost) W::SeedAndStamp(s);
        W::Census("seeded", isHost, NowMs() - g_connectedAtMs);
        g_phaseSinceMs = now;
    }
    if (!g_picked && now >= g_nextPollMs) {
        g_nextPollMs = now + kPollMs * 4;   // the census walks every object in the world
        // The host's stamps reach this peer as each disc's save record, and both peers name their
        // discs by the same rule, so a client that named them before every stamp had landed would
        // name a different set.
        const int marked = isHost ? kDiscs : W::MarkedDiscCount();
        if (marked >= kDiscs) {
            g_picked = true;
            W::PickDiscs(isHost);
            g_phaseSinceMs = now;
        } else if (now - g_phaseSinceMs > kStepDeadlineMs) {
            char why[96];
            std::snprintf(why, sizeof(why), "%d of the host's %d stamped discs reached this peer",
                          marked, kDiscs);
            Die(isHost, "pick", why, now);
            return;
        }
    }
    if (g_picked) Advance(isHost, now);
    WatchFastDestroy(isHost, now);
    if (now >= g_nextCensusMs) {
        g_nextCensusMs = now + kCensusMs;
        W::Census("tick", isHost, NowMs() - g_connectedAtMs);
    }
    if (g_picked && g_cur >= kStepCount) Finish(isHost);
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_connectedAtMs = 0;
    g_nextCensusMs = 0;
    g_cur = 0;
    g_phase = Phase::Pre;
    g_phaseSinceMs = 0;
    g_nextNearMs = 0;
    g_lateAtMs = 0;
    g_nextPollMs = 0;
    g_deadStep = nullptr;
    g_deadWhy.clear();
    g_finished = false;
    g_fastWatch = EjectWatch{};
    g_fastLost = 0;
    g_seeded = g_picked = false;
    for (size_t i = 0; i < kStepCount; ++i) g_outcome[i] = Outcome{};
    W::Reset();
}

}  // namespace coop::dev::floppy_selftest
