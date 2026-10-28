// coop/props/container_act_put.cpp -- the put seats of a client's act on a world container, and their replay. Two
// routes put a record the client holds into a shared container: R2, the hotbar key with no slot hovered (the screen's
// em_take, which puts the selected record of the player's own pane), and R3, hold and run (the container's
// playerUsedOn, which puts the actor in the hand). Each is cancelled at its seat and asked of the host with the record
// the player would put in; on a grant the same verb runs again under the replay latch, if it still names that record,
// and the commit says where the container stored it, or that it stored nothing. The take seat, the pending record and
// the commit queue are container_act_request's (see container_act_request_internal.h). The fit runs on the client, on
// the real actors (MTA's server has no client-side fit, CGame.cpp:3150-3153): the container's test needs the actor.
// Gameplay layer (principle 7). Game thread.

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_request_internal.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <cstring>
#include <iterator>
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

// A read that tells a put from any other press and fails leaves the verb to run natively, unjudged: said once per
// session, with the field, so a layout change cannot turn the feature off silently.
enum SayKind : size_t { kSayScreen, kSayPlayer, kSayHold, kSayRun, kSayCount };
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
    // change while the player runs; R3 hashes it without them.
    r.hash = route == kRouteR3 ? internal::PutHandHash(rec) : csw::HashBytes(wire);
    if (Ask(r)) {
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

void NotReplayed(const Pending& p, const char* why) {
    UE_LOGI("container_act: put id=%u eid=%u route=%s not replayed (%s) -> aborted", static_cast<unsigned>(p.id),
            static_cast<unsigned>(p.eid), RouteName(p.route), why);
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
        hashMatches = inv && cio::ReadRecords(inv, mine, /*neuterNested=*/false) && p.index < mine.size() &&
                      csw::RecordHash(mine[p.index]) == p.hash;
    }
    const bool shows = read && st.container == container && cv::Viewed() == container;
    const bool notHovered = read && !st.hovered;
    const bool enabled = read && st.takeEnabled;
    const bool sameIndex = read && st.index >= 0 && static_cast<uint32_t>(st.index) == p.index;
    why = !containerLive ? "the container is gone" : !shows ? "the screen changed" : !notHovered ? "a slot is hovered"
          : !enabled ? "the take button is off" : !sameIndex ? "the selected record changed"
          : !hashMatches ? "the record changed" : "";
    return internal::CheckPutR2(containerLive, shows, notHovered, enabled, sameIndex, hashMatches);
}

// R3's guards. `hand` is the actor the local player holds now.
internal::PutCheck CheckR3(const Pending& p, void* hand, bool containerLive, const char*& why) {
    const bool isAsked = hand && hand == p.hand.Get();
    SR::SaveRecord rec;
    const bool hashMatches = isAsked && SR::CaptureOwnRecord(hand, rec) && internal::PutHandHash(rec) == p.hash;
    why = !containerLive ? "the container is gone" : !isAsked ? "the hand changed" : !hashMatches ? "the record changed" : "";
    return internal::CheckPutR3(containerLive, isAsked, hashMatches);
}

}  // namespace

void ResetPutLines() {
    for (bool& said : g_said) said = false;
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

uint64_t PutHandHash(const ue_wrap::save_record::SaveRecord& record) {
    ue_wrap::save_record::SaveRecord copy = record;
    copy.xform.fill(0.f);
    if (!copy.floats.empty() && !copy.floats[0].empty()) copy.floats[0][0] = 0.f;
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
