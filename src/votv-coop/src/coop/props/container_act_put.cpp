// coop/props/container_act_put.cpp -- the put seats of a client's act on a world container, and their replay. Two
// routes put a record the client holds into a shared container: R2, the hotbar key with no slot hovered (the screen's
// em_take, which puts the selected record of the player's own pane), and R3, hold and run (the container's
// playerUsedOn, which puts the actor in the hand). Each is cancelled at its seat and asked of the host with the record
// the player would put in; on a grant the same verb runs again under the replay latch, if it still names that record,
// and the commit says where the container stored it, or that it stored nothing. The take seat, the pending record and
// the commit queue are container_act_request's (see container_act_request_internal.h). The fit runs on the client, on
// the real actors (MTA's confirm reads only server state, CGame.cpp:3373-3417): the container's test needs the actor.
// Gameplay layer (principle 7). Game thread.

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_request_internal.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <array>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace coop::props::container_act_request::detail {
namespace {

namespace caw = coop::props::container_act_wire;
namespace ci  = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;
namespace csw = coop::props::container_slice_wire;
namespace cv  = ue_wrap::container_view;
namespace E   = ue_wrap::engine;
namespace PU  = ue_wrap::prop_use;
namespace R   = ue_wrap::reflection;
namespace SR  = ue_wrap::save_record;
namespace sg  = ue_wrap::script_gate;
using coop::net::ContainerActFamily;

// The two seats: the screen's em_take (R2) and the container's playerUsedOn (R3).
UE_FACT_FN(kRowEmTake, "container_act", L"ui_playerInventory_C", L"em_take", Declared);
UE_FACT_FN(kRowUsedOn, "container_act", L"prop_container_C", L"playerUsedOn", Declared);
constexpr int kTagR2 = 0x43414332;   // "CAC2"
constexpr int kTagR3 = 0x43414333;   // "CAC3"

const char* RouteName(uint8_t route) { return route == kRouteR2 ? "R2" : "R3"; }

// The put drill's knobs (dev:: below): inert unless a drill calls them, all reset at the session's end.
bool g_noHoverGuard = false;   // R2's replay does not require that no slot is hovered
bool g_noHandTable = false;    // the hand hash leaves out no class's ticking fields
bool g_doubleDone = false;     // a put's done commit is queued twice

// A read that tells a put from any other press and fails leaves the verb to run natively, unjudged: said once per
// session, with the field, so a layout change cannot turn the feature off silently. kSayUnloaded is the hand-hash
// table's own once-per-session line, not a failed read.
enum SayKind : size_t { kSayScreen, kSayPlayer, kSayHold, kSayRun, kSayUnloaded, kSayCount };
bool g_said[kSayCount] = {};

sg::Verdict SayUnreadable(SayKind which, const char* seat, const char* field) {
    if (!g_said[which]) {
        g_said[which] = true;
        UE_LOGW("container_act: the %s seat could not read %s -- the verb runs natively, unjudged", seat, field);
    }
    return sg::Verdict::Run;
}

template <class T>
bool ReadParam(R::ParamOffset& offset, const sg::Call& c, T& out) {
    const int32_t o = offset.Of(c.function);
    if (o < 0 || !c.locals) return false;
    std::memcpy(&out, c.locals + o, sizeof(T));
    return true;
}

// The record's raw floats[0][1..3], for dev::LastPut: NaN for an index the record does not have.
void RawFloats(const SR::SaveRecord& rec, float (&out)[3]) {
    for (size_t i = 0; i < 3; ++i)
        out[i] = !rec.floats.empty() && rec.floats[0].size() > i + 1 ? rec.floats[0][i + 1]
                                                                      : std::numeric_limits<float>::quiet_NaN();
}

// May this record travel? A container's contents live at its GObjStack slot, which means nothing on another peer, so
// one that holds anything is refused with a line; one with no slot (-1) or an empty slot travels. An index or a store
// that does not read is refused too: a loaded container's contents are never lost to a read that failed.
bool MayTravel(const SR::SaveRecord& rec) {
    int32_t stack = -1;
    if (!ci::ContainerStackIndex(rec, stack) || stack == -1) return true;
    int32_t held = 0;
    if (!ci::ReadStackCount(stack, held)) {
        ShowBadReason();
        return false;
    }
    if (held > 0) {
        ShowEmptyFirst();
        return false;
    }
    return true;
}

// The one ask of both seats, the seat's own call cancelled whatever came of it. The host judges the record's class and
// name, so a record with no name is never asked; one too large for its stored form to pack is never asked either.
sg::Verdict AskPut(uint8_t route, uint32_t eid, void* container, const SR::SaveRecord& rec, uint16_t index,
                   void* screen, void* hand) {
    std::wstring name;
    if (!SR::RecordName(rec, name)) {
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    if (!MayTravel(rec)) return sg::Verdict::Cancel;
    const std::vector<uint8_t> wire = caw::RecordWire(rec);   // serialised once: its size and its hash
    if (wire.size() > caw::kMaxRecordBytes - caw::kPutStoredSlack) {
        UE_LOGW("container_act: put eid=%u route=%s: the %ls is past the byte cap a put may ask", eid, RouteName(route),
                rec.className.c_str());
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    caw::Request r;
    r.family = static_cast<uint8_t>(ContainerActFamily::PutRecord);
    r.eid = eid;
    r.record = rec;
    // Not on the wire: the form the replay hashes again. A held actor's record carries its pose and its lifespan, which
    // change while the player runs, and some classes carry a field that ticks by itself; R3 hashes it without them.
    r.hash = route == kRouteR3 ? internal::PutHandHash(rec, internal::PutExclusionRowsOf(hand)) : csw::HashBytes(wire);
    if (Ask(r)) {
        float asked[3];
        RawFloats(rec, asked);
        Pending& p = Rec();
        p.container.Set(container);
        p.screen.Set(screen);
        p.hand.Set(hand);
        p.route = route;
        p.index = index;
        p.hash = r.hash;
        p.name = name;
        UE_LOGI("container_act: put id=%u eid=%u route=%s asked %ls", static_cast<unsigned>(p.id),
                static_cast<unsigned>(eid), RouteName(route), rec.className.c_str());
        NotePut(p, dev::PutOutcome::Asked, p.index, 0, asked);
    }
    // A refused ask has shown its own line, and an open act shows nothing.
    return sg::Verdict::Cancel;
}

// Sends the seat's verdict for a container that is not shared: false when the act is the seat's to judge.
bool OwnOrNotReady(void* container, uint32_t& eid, sg::Verdict& verdict) {
    switch (SharedTarget(container, eid)) {
    case Target::Own:
        verdict = sg::Verdict::Run;
        return true;
    case Target::NotReady:
        ShowNotReady();
        verdict = sg::Verdict::Cancel;
        return true;
    case Target::Shared:
        break;
    }
    return false;
}

// R2: the hotbar key with no hovered slot puts the selected record of the player's pane into the container on the
// screen. A hovered slot is R1's (the take seat) or the hover's clear, and a disabled take button is the game's own
// refusal: both run natively. The game's own gate flags are locals of its graph and are not read: a put it would have
// swallowed is caught by the replay, which commits an abort when no record was added.
sg::Verdict PreR2(const sg::Call& c) {
    if (InReplay()) return sg::Verdict::Run;
    coop::net::Session* const s = LinkedSession();
    if (!s) return sg::Verdict::Run;
    cv::PutState st{};
    const char* failed = "";
    if (!cv::ReadPutState(c.object, st, &failed)) return SayUnreadable(kSayScreen, "R2 (em_take)", failed);
    if (st.hovered || !st.takeEnabled) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) return HostPre(st.container, /*anyHold=*/true);
    uint32_t eid = 0;
    sg::Verdict verdict = sg::Verdict::Run;
    if (OwnOrNotReady(st.container, eid, verdict)) return verdict;
    // The record is the player's pane's at the selected index, as it is: the host judges it, and the replay hashes it.
    void* const pane = cv::PlayerContainer();
    void* const inv = pane ? ci::InventoryOf(pane) : nullptr;
    std::vector<SR::SaveRecord> recs;
    if (!inv || !cio::ReadRecords(inv, recs, /*neuterNested=*/false) || st.index < 0 ||
        static_cast<size_t>(st.index) >= recs.size()) {
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    return AskPut(kRouteR2, eid, st.container, recs[static_cast<size_t>(st.index)], static_cast<uint16_t>(st.index),
                  c.object, nullptr);
}

// R3: hold and run puts the actor in the hand into the container the player looks at. The container's body returns on
// a locked container and on a player who is not running; the seat runs those natively and asks only for the put.
sg::Verdict PreR3(const sg::Call& c) {
    if (InReplay()) return sg::Verdict::Run;
    coop::net::Session* const s = LinkedSession();
    if (!s) return sg::Verdict::Run;
    static R::ParamOffset sPlayer{L"player"};
    static R::ParamOffset sHold{L"holdObject"};
    void* player = nullptr;
    void* hold = nullptr;
    bool runs = false;
    if (!ReadParam(sPlayer, c, player)) return SayUnreadable(kSayPlayer, "R3 (playerUsedOn)", "the player parameter");
    if (!ReadParam(sHold, c, hold)) return SayUnreadable(kSayHold, "R3 (playerUsedOn)", "the holdObject parameter");
    if (!E::ReadInputRun(player, runs)) return SayUnreadable(kSayRun, "R3 (playerUsedOn)", "input_run");
    if (!runs) return sg::Verdict::Run;
    if (!hold || !R::IsLive(hold)) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) return HostPre(c.object, /*anyHold=*/true);
    uint32_t eid = 0;
    sg::Verdict verdict = sg::Verdict::Run;
    if (OwnOrNotReady(c.object, eid, verdict)) return verdict;
    ci::PutTerms terms{};
    if (!ci::ReadPutTerms(c.object, terms)) {
        ShowBadReason();   // never an unjudged put
        return sg::Verdict::Cancel;
    }
    if (terms.locked) return sg::Verdict::Run;
    SR::SaveRecord rec;
    if (!SR::CaptureOwnRecord(hold, rec)) {
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    return AskPut(kRouteR3, eid, c.object, rec, 0, nullptr, hold);
}

// The container's records and their hashes, in order. False when the container or its records do not read.
bool ReadRecordHashes(void* container, std::vector<SR::SaveRecord>& recs, std::vector<uint64_t>& hashes) {
    void* const inv = container ? ci::InventoryOf(container) : nullptr;
    if (!inv || !cio::ReadRecords(inv, recs, /*neuterNested=*/false)) return false;
    hashes.clear();
    hashes.reserve(recs.size());
    for (const SR::SaveRecord& r : recs) hashes.push_back(csw::RecordHash(r));
    return true;
}

// The hash the replay read of the record it was asked about (what the check compared with the asked one), 0 until a
// check reads it; dev::LastPut says it.
uint64_t g_replayHash = 0;
// The record's raw floats[0][1..3] the replay's own capture read, before the hash zeroes anything; NaN until a capture
// reads them. dev::LastPut says it.
float g_replayFloats[3] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::quiet_NaN()};

// The reason of the last abort before the verb, for dev::LastWhy.
const char* g_lastWhy = "";

void NotReplayed(const Pending& p, const char* why) {
    g_lastWhy = why;
    UE_LOGI("container_act: put id=%u eid=%u route=%s not replayed (%s) -> aborted", static_cast<unsigned>(p.id),
            static_cast<unsigned>(p.eid), RouteName(p.route), why);
    NotePut(p, dev::PutOutcome::Aborted, p.index, g_replayHash, g_replayFloats);
    SendCommit(p.id, p.eid, 0);
    ShowBadReason();   // the click is lost, never the record: the player puts again
}

// R2's guards, each read at the moment of the replay. Replay, or Abort with the first guard that failed in `why`.
internal::PutCheck CheckR2(const Pending& p, void* container, bool containerLive, const char*& why) {
    cv::PutState st{};
    void* const screen = p.screen.Get();
    const bool read = screen && cv::ReadPutState(screen, st);
    bool hashMatches = false;
    if (read) {
        void* const pane = cv::PlayerContainer();
        void* const inv = pane ? ci::InventoryOf(pane) : nullptr;
        std::vector<SR::SaveRecord> mine;
        const bool readMine = inv && cio::ReadRecords(inv, mine, /*neuterNested=*/false) && p.index < mine.size();
        if (readMine) g_replayHash = csw::RecordHash(mine[p.index]);
        hashMatches = readMine && g_replayHash == p.hash;
    }
    hashMatches = hashMatches || g_noHoverGuard;
    const bool shows = read && st.container == container && cv::Viewed() == container;
    // The red (container_act_no_hover_guard) leaves the hover unjudged: a hovered slot reads index -1 and the pane's
    // record is not the one the em_take presses, so the hover, the index and the hash go together.
    const bool notHovered = g_noHoverGuard || (read && !st.hovered);
    const bool enabled = read && st.takeEnabled;
    const bool sameIndex = g_noHoverGuard || (read && st.index >= 0 && static_cast<uint32_t>(st.index) == p.index);
    why = !containerLive ? "the container is gone" : !shows ? "the screen changed" : !notHovered ? dev::kPutWhyHovered
          : !enabled ? "the take button is off" : !sameIndex ? "the selected record changed"
          : !hashMatches ? "the record changed" : "";
    return internal::CheckPutR2(containerLive, shows, notHovered, enabled, sameIndex, hashMatches);
}

// R3's guards. `hand` is the actor the local player holds now.
internal::PutCheck CheckR3(const Pending& p, void* hand, bool containerLive, const char*& why) {
    const bool isAsked = hand && hand == p.hand.Get();
    SR::SaveRecord rec;
    const bool captured = isAsked && SR::CaptureOwnRecord(hand, rec);
    if (captured) RawFloats(rec, g_replayFloats);
    if (captured) g_replayHash = internal::PutHandHash(rec, internal::PutExclusionRowsOf(hand));
    const bool hashMatches = captured && g_replayHash == p.hash;
    why = !containerLive ? "the container is gone" : !isAsked ? "the hand changed" : !hashMatches ? "the record changed" : "";
    return internal::CheckPutR3(containerLive, isAsked, hashMatches);
}

}  // namespace

void ResetPutLines() {
    for (bool& said : g_said) said = false;
    g_noHoverGuard = g_noHandTable = g_doubleDone = false;
    g_lastWhy = "";
}

const WatchRow* PutWatches(size_t& count) {
    static const WatchRow kRows[] = {
        {kRowEmTake, kTagR2, &PreR2},
        {kRowUsedOn, kTagR3, &PreR3},
    };
    count = std::size(kRows);
    return kRows;
}

// The granted put: the same verb runs again under the latch if it still names the record the host reserved room for,
// and the commit says where the container stored it. The container's records are read before and after; the commit is
// done exactly when one record was inserted, and carries it as the container stored it (the fridge flag added).
void ReplayPut() {
    Pending& p = Rec();
    g_replayHash = 0;
    g_lastWhy = "";
    for (float& f : g_replayFloats) f = std::numeric_limits<float>::quiet_NaN();
    void* const container = p.container.Get();
    std::vector<SR::SaveRecord> before;
    std::vector<uint64_t> beforeHashes;
    const bool containerLive = ReadRecordHashes(container, before, beforeHashes);
    const char* why = "";
    void* hand = nullptr;
    void* player = nullptr;
    internal::PutCheck check = internal::PutCheck::Abort;
    if (p.route == kRouteR2) {
        check = CheckR2(p, container, containerLive, why);
    } else {
        player = coop::players::Registry::Get().Local();
        E::MainPlayerGrabState grab{};
        hand = player && E::ReadMainPlayerGrabState(player, grab) ? grab.holdingActor : nullptr;
        check = CheckR3(p, hand, containerLive, why);
    }
    if (check == internal::PutCheck::Abort) {
        NotReplayed(p, why);
        return;
    }
    bool ran = false;
    {
        Latched latch;
        ran = p.route == kRouteR2
                  ? cv::RunPut(p.screen.Get())
                  : PU::CallPlayerUsedOn(container, player, nullptr, ue_wrap::FVector{}, nullptr, hand, R::FName{});
    }
    // D4: the verb may have changed the mirror under the latch, whatever the commit says; the host's next truth must apply.
    coop::props::container_contents_sync::ForgetApplied(p.eid);
    std::vector<SR::SaveRecord> after;
    std::vector<uint64_t> afterHashes;
    const bool read = ReadRecordHashes(p.container.Get(), after, afterHashes);
    const std::optional<size_t> at = ran && read ? internal::StoredIndex(beforeHashes, afterHashes) : std::nullopt;
    if (at) {
        caw::Commit done;
        done.id = p.id;
        done.eid = p.eid;
        done.committed = 1;
        done.hasRecord = true;
        done.index = static_cast<uint16_t>(*at);
        done.record = after[*at];
        if (QueueCommit(done)) {   // false: the commit does not pack, nothing queued
            UE_LOGI("container_act: put id=%u eid=%u route=%s replayed -> done at index %zu",
                    static_cast<unsigned>(p.id), static_cast<unsigned>(p.eid), RouteName(p.route), *at);
            NotePut(p, dev::PutOutcome::Done, static_cast<uint16_t>(*at), g_replayHash, g_replayFloats);
            if (g_doubleDone) QueueCommit(done);   // the drill's second send of the same done: the host's bad commit
            return;
        }
        // Cannot happen under the size gate; the container has the record and the host will not.
        UE_LOGE("container_act: put id=%u eid=%u route=%s the stored %ls does not pack into a commit -- aborted",
                static_cast<unsigned>(p.id), static_cast<unsigned>(p.eid), RouteName(p.route),
                done.record.className.c_str());
    }
    // The verb did not run, or the container stored nothing: the fit failed, the game's own gate swallowed the call, or
    // the player let go of run. An R2 whose fit failed leaves the item a world prop, as the single-player game does.
    UE_LOGI("container_act: put id=%u eid=%u route=%s replayed ran=%d -> aborted (the container stored nothing)",
            static_cast<unsigned>(p.id), static_cast<unsigned>(p.eid), RouteName(p.route), ran ? 1 : 0);
    NotePut(p, dev::PutOutcome::Aborted, p.index, g_replayHash, g_replayFloats);
    SendCommit(p.id, p.eid, 0);
}

}  // namespace coop::props::container_act_request::detail

namespace coop::props::container_act_request::internal {

PutCheck CheckPutR2(bool containerLive, bool screenShowsContainer, bool notHovered, bool takeEnabled, bool sameIndex,
                    bool hashMatches) {
    return containerLive && screenShowsContainer && notHovered && takeEnabled && sameIndex && hashMatches
               ? PutCheck::Replay : PutCheck::Abort;
}

PutCheck CheckPutR3(bool containerLive, bool handIsAsked, bool hashMatches) {
    return containerLive && handIsAsked && hashMatches ? PutCheck::Replay : PutCheck::Abort;
}

namespace {

// The fields of a held actor that tick by themselves, one row per class (a subclass matches its parent's row). A row
// zeroes `count` elements from [group][outer][first] of the record before the hash. Safe because the commit carries the
// record AS THE CONTAINER STORED IT and the replay still requires the same live holding_actor: an excluded field only
// stops a ticking value from aborting the put, it never lets a different item through. Each row's (:N) is the line of the
// class's own Blueprint dump that writes the field. A subclass appends one mFloat as floats[1], one mInt as ints[0] and
// one mBool as bools[1] to its parent's record, and prop_food replaces floats[0].
enum class Group : uint8_t { Floats, Ints, Bools, Vectors };
struct Exclusion {
    const wchar_t* cls;
    Group group;
    uint8_t outer, first, count;
};
constexpr Exclusion kExclusions[] = {
    // Temperature and ripeness drift (measured); the clock is written fresh by every getData (:808-832).
    {L"prop_food_C", Group::Floats, 0, 1, 3},
    {L"prop_atvcarbattery_C", Group::Floats, 1, 1, 1},   // heat cools each tick (:43)
    {L"prop_funGun_crossbow_C", Group::Floats, 1, 0, 1},  // temp, FInterpTo each tick (:41)
    {L"prop_concreteBucket_C", Group::Bools, 1, 0, 1},    // dry flips on its own timer (:370-377)
    {L"prop_lamp_C", Group::Floats, 1, 0, 1},             // fuel burns while lit (:38)
    {L"prop_torch_C", Group::Floats, 1, 0, 1},            // fuel burns while lit (:143)
    {L"prop_fireExt_C", Group::Floats, 1, 0, 1},          // life drains while spraying (:82)
    {L"prop_grenade_C", Group::Floats, 1, 0, 1},          // timer counts down when armed (:21)
    {L"prop_pipebomb_C", Group::Floats, 1, 0, 1},         // timer counts down when armed (:25)
    {L"prop_lemonade_C", Group::Floats, 1, 0, 1},         // timer, after activation (:24)
    {L"prop_radio_local_C", Group::Floats, 1, 0, 1},      // time, while playing (:136)
    {L"prop_librarian_C", Group::Ints, 0, 0, 1},          // secs, while counting (:48)
    {L"prop_physgun_soft_C", Group::Floats, 1, 0, 1},     // time, in use (:20)
    {L"prop_popcorn_C", Group::Floats, 1, 0, 1},          // inflat, heating (:70)
    // The cord head's world location and rotation: a world pose, the transform's kind (:187-215).
    {L"prop_corded_C", Group::Vectors, 0, 0, 2},
};
constexpr size_t kExclusionCount = std::size(kExclusions);
static_assert(kExclusionCount <= 32, "PutHandHash's row mask is 32 bits");

template <class V>
void ZeroElements(std::vector<std::vector<V>>& groups, const Exclusion& e, const V& zero) {
    if (e.outer >= groups.size()) return;
    std::vector<V>& group = groups[e.outer];
    for (size_t i = e.first; i < e.first + e.count && i < group.size(); ++i) group[i] = zero;
}

// One row's class, found again when the class table dropped it (ClassListingId moves on an unload).
struct ClassSlot {
    void* cls = nullptr;
    uint64_t listing = 0;
};
ClassSlot g_slots[kExclusionCount];

}  // namespace

size_t PutExclusionRowOf(const wchar_t* className) {
    for (size_t i = 0; i < kExclusionCount; ++i)
        if (std::wcscmp(kExclusions[i].cls, className) == 0) return i;
    return kNoExclusionRow;
}

uint32_t PutExclusionRowsOf(void* heldActor) {
    namespace OI = ue_wrap::object_index;
    namespace R  = ue_wrap::reflection;
    void* const actorClass = heldActor ? R::ClassOf(heldActor) : nullptr;
    if (!actorClass || detail::g_noHandTable) return 0;   // the red: an empty table (container_act_no_hand_table)
    uint32_t rows = 0;
    size_t unloaded = 0;
    for (size_t i = 0; i < kExclusionCount; ++i) {
        ClassSlot& s = g_slots[i];
        if (!s.cls || OI::ClassListingId(s.cls) != s.listing) {
            s.cls = OI::ClassByName(kExclusions[i].cls);
            s.listing = s.cls ? OI::ClassListingId(s.cls) : 0;
        }
        if (!s.cls) {   // a class no loaded actor can derive from: its row cannot match
            ++unloaded;
            continue;
        }
        void* const base = s.cls;
        if (R::IsDescendantOfAny(actorClass, &base, 1)) rows |= 1u << i;
    }
    bool& said = detail::g_said[detail::kSayUnloaded];
    if (unloaded && !said) {
        said = true;
        UE_LOGI("container_act: %zu hand-hash exclusion class(es) are not loaded in this world; their rows apply once "
                "they load", unloaded);
    }
    return rows;
}

uint64_t PutHandHash(const ue_wrap::save_record::SaveRecord& record, uint32_t rows) {
    ue_wrap::save_record::SaveRecord copy = record;
    copy.xform.fill(0.f);
    if (!copy.floats.empty() && !copy.floats[0].empty()) copy.floats[0][0] = 0.f;
    for (size_t i = 0; i < kExclusionCount; ++i) {
        if (!(rows & (1u << i))) continue;
        const Exclusion& e = kExclusions[i];
        switch (e.group) {
            case Group::Floats:  ZeroElements(copy.floats, e, 0.f); break;
            case Group::Ints:    ZeroElements(copy.ints, e, int32_t{0}); break;
            case Group::Bools:   ZeroElements(copy.bools, e, uint8_t{0}); break;
            case Group::Vectors: ZeroElements(copy.vectors, e, std::array<float, 3>{}); break;
        }
    }
    return coop::props::container_slice_wire::RecordHash(copy);
}

std::optional<size_t> StoredIndex(const std::vector<uint64_t>& before, const std::vector<uint64_t>& after) {
    if (after.size() != before.size() + 1) return std::nullopt;
    size_t i = 0;
    while (i < before.size() && before[i] == after[i]) ++i;
    for (size_t k = i; k < before.size(); ++k)
        if (before[k] != after[k + 1]) return std::nullopt;
    return i;
}

}  // namespace coop::props::container_act_request::internal

namespace coop::props::container_act_request::dev {

void NoHoverGuard(bool on) { detail::g_noHoverGuard = on; }
void NoHandTable(bool on) { detail::g_noHandTable = on; }
void DoubleDone(bool on) { detail::g_doubleDone = on; }
const char* LastWhy() { return detail::g_lastWhy; }

}  // namespace coop::props::container_act_request::dev
