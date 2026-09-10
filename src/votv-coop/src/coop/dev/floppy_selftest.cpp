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
#include "ue_wrap/devices/serverbox.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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

// The schedule, measured from the first tick at which BOTH peers are in the world. Anchoring on
// this peer's own connect instead put the two clocks about ten seconds apart -- the host binds
// and the client joins after a save transfer and a world load -- so a client's insert at its
// t+20s and the host's eject of that insert at the host's t+30s landed in the same wall second,
// and the eject read an empty slot. The gaps below are wide next to the lane's 1 Hz poll and
// wider than the box's out-timeline, which is what hands an eject its disc.
constexpr uint64_t kSeedMs   =  8000;
constexpr uint64_t kPickMs   = 16000;
constexpr uint64_t kCensusMs =  6000;
constexpr uint64_t kPostMs   =  4000;   // the after-picture of one episode
// The SECOND look at the same episode's disc. Ten seconds, not twelve, because the schedule has
// one pair that shares a disc on purpose -- a host re-inserting what the client's eject handed
// back -- and at twelve the re-insert CONSUMED that disc in the same tick as the eject's own late
// sample, which then read it as lost. An instrument must not schedule its own reading against its
// own next verb.
constexpr uint64_t kLateMs   = 10000;
constexpr uint64_t kFinalMs  = 200000;

enum class Verb { Insert, Eject };

struct Step {
    const char* id;
    uint64_t    atMs;
    bool        onHost;   // the role that fires it; the other role only watches
    int         box;
    int         disc;     // the disc this episode moves: the one inserted, or the one expected out
    Verb        verb;
    const char* under_test;
};

// One row per episode half. `under_test` is printed with the outcome, so one log says whether what
// happened is the thing the episode was built to catch.
const Step kSteps[] = {
    { "E1-insert", 20000, false, 0,  0, Verb::Insert,
      "a client insert: the disc dies here and the destroy is what crosses" },
    { "E1-eject",  30000, true,  0,  0, Verb::Eject,
      "the host ejecting a client's insert: an empty slot means the transfer never crossed" },
    { "E2-insert", 42000, true,  1,  1, Verb::Insert,
      "a host insert: the slot state is authored on the host" },
    { "E2-eject",  52000, false, 1,  1, Verb::Eject,
      "the client ejecting a host insert: an empty slot means the slot state never crossed" },
    { "E4-insert", 64000, true,  1,  1, Verb::Insert,
      "the host re-inserting the disc E2 handed back: the same-peer control needs its own insert" },
    { "E4-eject",  74000, true,  1,  1, Verb::Eject,
      "the host ejecting its own insert: the disc and its content come back here" },
    { "E3-insert", 86000, false, 2,  2, Verb::Insert,
      "a client insert whose eject is the same client's" },
    { "E3-eject",  96000, false, 2,  2, Verb::Eject,
      "a client eject: the disc is born on the client, through its own place seam" },
    // The repeats of E1, one fresh disc each, so the run answers "how often" and not "did it once".
    // Sixteen seconds a pair: the eject is eight after its insert, the next insert eight after
    // that, and both are wide next to the slot lane's 1 Hz poll and the box's out-timeline.
    { "R2-insert", 108000, false, 0, 3, Verb::Insert, "repeat 2 of the cross-peer eject: insert" },
    { "R2-eject",  116000, true,  0, 3, Verb::Eject,  "repeat 2 of the cross-peer eject" },
    { "R3-insert", 124000, false, 0, 4, Verb::Insert, "repeat 3 of the cross-peer eject: insert" },
    { "R3-eject",  132000, true,  0, 4, Verb::Eject,  "repeat 3 of the cross-peer eject" },
    { "R4-insert", 140000, false, 0, 5, Verb::Insert, "repeat 4 of the cross-peer eject: insert" },
    { "R4-eject",  148000, true,  0, 5, Verb::Eject,  "repeat 4 of the cross-peer eject" },
    { "R5-insert", 156000, false, 0, 6, Verb::Insert, "repeat 5 of the cross-peer eject: insert" },
    { "R5-eject",  164000, true,  0, 6, Verb::Eject,  "repeat 5 of the cross-peer eject" },
    { "R6-insert", 172000, false, 0, 7, Verb::Insert, "repeat 6 of the cross-peer eject: insert" },
    { "R6-eject",  180000, true,  0, 7, Verb::Eject,  "repeat 6 of the cross-peer eject" },
};
constexpr size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

// `lateLive` is the reading the acceptance turns on: not whether the eject produced a disc, but
// whether the disc was STILL there twelve seconds later. The re-swallow takes about a second, so a
// post-picture alone can catch a disc that is already doomed. -1 = never sampled.
// `preLive` is what keeps the ratio honest. An eject whose named disc is ALREADY lying in this
// peer's world is not ejecting that disc -- its insert never consumed it -- so the episode measures
// nothing and must not be counted as a survival. One run read 7 of 9 ejects as survivals when five
// of them had never moved a disc at all.
struct Outcome {
    bool        done     = false;
    bool        fired    = false;  // the verb was dispatched AND the game acted on it
    int         lateLive = -1;     // -1 not sampled, 0 gone, 1 present
    int         preLive  = -1;     // eject only: -1 not sampled, 0 the disc was away, 1 still here
    std::string note;              // the refusal reason, or what the slot did
};
Outcome g_outcome[kStepCount];

uint64_t g_connectedAtMs = 0;
uint64_t g_nextCensusMs  = 0;
uint64_t g_postAtMs      = 0;
int      g_postStep      = -1;
uint64_t g_lateAtMs      = 0;
int      g_lateStep      = -1;

// The fast-destroy watch: after an eject episode, whether the disc that came out was destroyed
// again within moments. That is the re-swallow's signature and nothing else's -- no player is in
// the room, and the only other actor that can reach the disc is a device.
//
// TWO cadences, because one cannot do it. FINDING the disc costs an object-array walk, so it runs
// on a throttle; WATCHING it must not miss, because the whole defect is that the disc lives for
// well under a second -- a quarter-second poll walked straight past it, and this counter read ZERO
// through a run in which the disc was measurably eaten. So the moment the walk finds it, the actor
// is held in a slot-validated ref and its liveness is read EVERY tick, which touches no memory and
// costs nothing. One watch at a time: episodes are eight seconds apart.
constexpr uint64_t kFastWatchMs = 4000;
constexpr uint64_t kFastFindMs  =  100;   // the object-array walk, until the disc is found
struct EjectWatch {
    std::wstring          key;
    uint64_t              atMs      = 0;
    uint64_t              nextFind  = 0;
    ue_wrap::CachedObjRef actor;          // held from the first sighting; Alive() is a slot read
    bool                  sawLive   = false;
    bool                  counted   = false;
    int                   step      = -1;
};
EjectWatch g_fastWatch;
int        g_fastLost = 0;
bool     g_watched[kStepCount] = {};  // the non-firing role's one-shot arm per episode
static_assert(sizeof(g_watched) / sizeof(g_watched[0]) == kStepCount,
              "one watch flag per episode, or a new row overruns it silently");
bool     g_seeded        = false;
bool     g_picked        = false;
bool     g_finalDone     = false;

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

void Fire(size_t i) {
    const Step& s = kSteps[i];
    Outcome& o = g_outcome[i];
    o.done = true;
    void* box = W::Box(s.box);
    if (!box) {
        o.note = "the target box no longer resolves";
        UE_LOGW("floppy_selftest: %s NOT FIRED -- %s", s.id, o.note.c_str());
        return;
    }
    BoxSlot before{};
    ReadBoxSlot(box, before);

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
        const bool called = SB::CallProcessFloppy(box, disc);
        BoxSlot after{};
        ReadBoxSlot(box, after);
        const bool discGone = !R::IsLive(disc);
        o.fired = called;
        UE_LOGI("floppy_selftest: %s %s box=%d '%ls' disc key='%ls' rw=%d rows=%zu -- slot type "
                "%d -> %d rw %d -> %d rows %d -> %d json %d -> %d, disc destroyed=%d (%s)",
                s.id, called ? "FIRED" : "CALL REFUSED", s.box, W::BoxName(s.box).c_str(),
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
                    s.id, s.box, W::BoxName(s.box).c_str(), before.floppyType);
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
                "its no-disc hint (%s)", s.id, s.box, W::BoxName(s.box).c_str(), s.under_test);
        return;
    }
    const bool called = SB::CallEjectFloppy(box);
    BoxSlot after{};
    ReadBoxSlot(box, after);
    o.fired = called;
    o.note = "slot drained";
    UE_LOGI("floppy_selftest: %s %s box=%d '%ls' -- slot type %d -> %d rw %d -> %d rows %d -> %d "
            "json %d -> %d; the disc arrives on the out-timeline (%s)",
            s.id, called ? "FIRED" : "CALL REFUSED", s.box, W::BoxName(s.box).c_str(),
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
    if (isLate && s.verb == Verb::Eject) g_outcome[stepIdx].lateLive = live ? 1 : 0;
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
    if (marked && c.readWrites == wantRw) {
        UE_LOGI("floppy_selftest: CONTENT[%s] %s role=%s -- disc key='%ls' came back INTACT "
                "(rw=%d rows=%zu, marker present)", when, s.id, role, key.c_str(), c.readWrites,
                c.data.size());
        return;
    }
    UE_LOGW("floppy_selftest: CONTENT[%s] %s role=%s -- disc key='%ls' came back EMPTIED: rw=%d "
            "(seeded %d) rows=%zu marker=%s -- the actor crossed and its save data did not",
            when, s.id, role, key.c_str(), c.readWrites, wantRw, c.data.size(),
            marked ? "present" : "ABSENT");
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
            "their disc in THIS peer's world %llus after the verb (%d eject episodes total, %d "
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
        UE_LOGI("floppy_selftest: ARMED role=%s (seed +%llus, discs named +%llus, first episode "
                "+%llus)", isHost ? "HOST" : "CLIENT",
                static_cast<unsigned long long>(kSeedMs / 1000),
                static_cast<unsigned long long>(kPickMs / 1000),
                static_cast<unsigned long long>(kSteps[0].atMs / 1000));
    }
    const uint64_t since = now - g_connectedAtMs;
    if (!FD::EnsureResolved()) return;  // the disc fields come from there
    if (!W::ResolveBoxes()) return;

    if (!g_seeded && since >= kSeedMs) {
        g_seeded = true;
        if (isHost) W::SeedAndStamp();
        W::Census("seeded", isHost, NowMs() - g_connectedAtMs);
    }
    if (g_seeded && !g_picked && since >= kPickMs) {
        g_picked = true;
        W::PickDiscs(isHost);
    }
    if (g_picked) {
        for (size_t i = 0; i < kStepCount; ++i) {
            if (since < kSteps[i].atMs) continue;
            const bool mine = kSteps[i].onHost == isHost;
            if (mine ? g_outcome[i].done : g_watched[i]) continue;
            PreSight(i, isHost);
            if (mine) {
                W::Census("before", isHost, NowMs() - g_connectedAtMs);
                Fire(i);
            } else {
                // The peer that did NOT act still owes an after-picture: whether a cross-peer
                // transfer arrived is a fact only this side can report.
                g_watched[i] = true;
            }
            g_postAtMs = now + kPostMs;
            g_postStep = static_cast<int>(i);
            if (kSteps[i].verb == Verb::Eject) {
                // Ejects only. An insert's late sample would be armed and then overwritten by the
                // eject eight seconds behind it, so it would never fire; and it is the ejected
                // disc, not the consumed one, whose survival this run is about.
                g_lateAtMs = now + kLateMs;
                g_lateStep = static_cast<int>(i);
            }
            // Both roles arm the fast-destroy watch on an eject, because the destroy that kills
            // the disc is raised on the peer that did NOT eject and relayed to the one that did:
            // watching only the actor here would name the symptom on one side and miss its cause
            // on the other.
            if (kSteps[i].verb == Verb::Eject && kSteps[i].disc >= 0 &&
                kSteps[i].disc < kDiscs && !W::DiscKey(kSteps[i].disc).empty()) {
                g_fastWatch = EjectWatch{};
                g_fastWatch.key  = W::DiscKey(kSteps[i].disc);
                g_fastWatch.atMs = now;
                g_fastWatch.step = static_cast<int>(i);
            }
            break;  // one verb per tick, so two episodes can never share an after-picture
        }
    }
    if (g_postStep >= 0 && now >= g_postAtMs) {
        W::Census(kSteps[g_postStep].id, isHost, NowMs() - g_connectedAtMs);
        ReportContent(static_cast<size_t>(g_postStep), isHost, "post", false);
        g_postStep = -1;
    }
    // The second look. A disc the post-picture found can still be eaten a second later, and until
    // this run nothing sampled twice -- so "the eject produced a disc" was being read as "the disc
    // survived", which is the very difference this arc is about.
    if (g_lateStep >= 0 && now >= g_lateAtMs) {
        ReportContent(static_cast<size_t>(g_lateStep), isHost, "late", true);
        g_lateStep = -1;
    }
    if (g_fastWatch.step >= 0) {
        if (now - g_fastWatch.atMs > kFastWatchMs) {
            if (!g_fastWatch.sawLive)
                UE_LOGW("floppy_selftest: FAST-WATCH %s role=%s -- disc key='%ls' was NEVER seen "
                        "alive here in the %llu ms after the eject, so this peer cannot say whether "
                        "it was destroyed or never arrived. The survival reading below is what "
                        "answers that.", kSteps[g_fastWatch.step].id, isHost ? "HOST" : "CLIENT",
                        g_fastWatch.key.c_str(), static_cast<unsigned long long>(kFastWatchMs));
            g_fastWatch.step = -1;
        } else if (!g_fastWatch.sawLive) {
            if (now >= g_fastWatch.nextFind) {
                g_fastWatch.nextFind = now + kFastFindMs;
                void* a = PR::FindByKeyString(g_fastWatch.key);
                if (a && R::IsLive(a)) {
                    g_fastWatch.sawLive = true;
                    g_fastWatch.actor.Set(a);
                    UE_LOGI("floppy_selftest: FAST-WATCH %s role=%s -- disc key='%ls' appeared here "
                            "%llu ms after the eject; watching it every tick from now on",
                            kSteps[g_fastWatch.step].id, isHost ? "HOST" : "CLIENT",
                            g_fastWatch.key.c_str(),
                            static_cast<unsigned long long>(now - g_fastWatch.atMs));
                }
            }
        } else if (!g_fastWatch.counted && !g_fastWatch.actor.Alive()) {
            g_fastWatch.counted = true;
            ++g_fastLost;
            UE_LOGW("floppy_selftest: FAST-DESTROY %s role=%s -- disc key='%ls' was in this peer's "
                    "world after the eject and was GONE %llu ms later. Nobody is holding it and no "
                    "player is near it, so a device took it: that is the re-swallow.",
                    kSteps[g_fastWatch.step].id, isHost ? "HOST" : "CLIENT",
                    g_fastWatch.key.c_str(),
                    static_cast<unsigned long long>(now - g_fastWatch.atMs));
        }
    }
    if (now >= g_nextCensusMs) {
        g_nextCensusMs = now + kCensusMs;
        W::Census("tick", isHost, NowMs() - g_connectedAtMs);
    }
    if (!g_finalDone && since >= kFinalMs) {
        g_finalDone = true;
        W::Census("final", isHost, NowMs() - g_connectedAtMs);
        EmitVerdict();
    }
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_connectedAtMs = 0;
    g_nextCensusMs = 0;
    g_postAtMs = 0;
    g_postStep = -1;
    g_lateAtMs = 0;
    g_lateStep = -1;
    g_fastWatch = EjectWatch{};
    g_fastLost = 0;
    g_seeded = g_picked = g_finalDone = false;
    for (size_t i = 0; i < kStepCount; ++i) g_watched[i] = false;
    for (size_t i = 0; i < kStepCount; ++i) g_outcome[i] = Outcome{};
    W::Reset();
}

}  // namespace coop::dev::floppy_selftest
