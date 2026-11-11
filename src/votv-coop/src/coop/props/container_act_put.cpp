// coop/props/container_act_put.cpp -- the put seat of a client's act on a world container, and its replay. Hold and run
// puts the actor in the hand into the container the player looks at (the container's playerUsedOn). The seat cancels it
// and asks the host with the record the player would put in; on a grant the same verb runs again under the replay
// latch, if it still names that record, and the commit says where the container stored it, or that it stored nothing.
// The pending record and the commit queue are container_act_request's (see container_act_request_internal.h). The fit
// runs on the client, on the real actor (MTA's confirm reads only server state, CGame.cpp:3373-3417): the container's
// test needs the actor. Gameplay layer (principle 7). Game thread.

#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_act_request_internal.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_target.h"
#include "coop/items/inventory_item_wire.h"
#include "coop/items/save_record_wire.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/actors/inventory_item.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace coop::props::container_act_request::detail {
namespace {

namespace caw = coop::props::container_act_wire;
namespace IW = coop::inventory_item_wire;
namespace ci  = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;
namespace csw = coop::props::container_slice_wire;
namespace E   = ue_wrap::engine;
namespace PU  = ue_wrap::prop_use;
namespace R   = ue_wrap::reflection;
namespace II  = ue_wrap::inventory_item;
namespace sg  = ue_wrap::script_gate;
using coop::net::ContainerActFamily;

// The seat: the container's playerUsedOn, entered by hold and run.
UE_FACT_FN(kRowUsedOn, "container_act", L"prop_container_C", L"playerUsedOn", Declared);
constexpr int kTagUsedOn = 0x43414333;   // "CAC3"

// A read that tells a put from any other use and fails leaves the verb to run natively, unjudged: said once per
// session, with the field, so a layout change cannot turn the feature off silently.
enum SayKind : size_t { kSayPlayer, kSayHold, kSayRun, kSayCount };
bool g_said[kSayCount] = {};

sg::Verdict SayUnreadable(SayKind which, const char* field) {
    if (!g_said[which]) {
        g_said[which] = true;
        UE_LOGW("container_act: the put seat could not read %s -- the verb runs natively, unjudged", field);
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

// Hold and run puts the actor in the hand into the container the player looks at. The container's body returns on a
// locked container and on a player who is not running; the seat runs those natively and asks only for the put.
sg::Verdict PreUsedOn(const sg::Call& c) {
    if (InReplay()) return sg::Verdict::Run;
    coop::net::Session* const s = LinkedSession();
    if (!s) return sg::Verdict::Run;
    static R::ParamOffset sPlayer{L"player"};
    static R::ParamOffset sHold{L"holdObject"};
    void* player = nullptr;
    void* hold = nullptr;
    bool runs = false;
    if (!ReadParam(sPlayer, c, player)) return SayUnreadable(kSayPlayer, "the player parameter");
    if (!ReadParam(sHold, c, hold)) return SayUnreadable(kSayHold, "the holdObject parameter");
    if (!E::ReadInputRun(player, runs)) return SayUnreadable(kSayRun, "input_run");
    if (!runs) return sg::Verdict::Run;
    if (!hold || !R::IsLive(hold)) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) return HostPre(c.object);
    uint32_t eid = 0;
    sg::Verdict verdict = sg::Verdict::Run;
    if (OwnOrNotReady(c.object, eid, verdict)) return verdict;
    ci::PutTerms terms{};
    if (!ci::ReadPutTerms(c.object, terms)) {
        ShowBadReason();   // never an unjudged put
        return sg::Verdict::Cancel;
    }
    if (terms.locked) return sg::Verdict::Run;
    II::Item rec;
    if (!II::Capture(hold, rec)) {
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    return AskPutRecord(PutKind::HoldAndRun, eid, c.object, rec, hold);
}

// The container's records and their hashes, in order. False when the container or its records do not read.
bool ReadRecordHashes(void* container, std::vector<II::Item>& recs, std::vector<uint64_t>& hashes) {
    void* const inv = container ? ci::InventoryOf(container) : nullptr;
    if (!inv || !cio::ReadRecords(inv, recs)) return false;
    hashes.clear();
    hashes.reserve(recs.size());
    for (const II::Item& r : recs) hashes.push_back(csw::RecordHash(r));
    return true;
}

void NotReplayed(const Pending& p, const char* why) {
    UE_LOGI("container_act: put id=%u eid=%u not replayed (%s) -> aborted", static_cast<unsigned>(p.id),
            static_cast<unsigned>(p.eid), why);
    SendCommit(p.id, p.eid, 0);
    ShowBadReason();   // the click is lost, never the record: the player puts again
}

// The guards, each read at the moment of the replay. `hand` is the actor the local player holds now. Replay, or Abort
// with the first guard that failed in `why`.
internal::PutCheck CheckReplay(const Pending& p, void* hand, bool containerLive, const char*& why) {
    const bool isAsked = hand && hand == p.source.Get();
    II::Item rec;
    const bool captured = isAsked && II::Capture(hand, rec);
    const bool hashMatches = captured && internal::PutHandHash(rec) == p.hash;
    why = !containerLive ? "the container is gone" : !isAsked ? "the hand changed" : !hashMatches ? "the record changed" : "";
    return internal::CheckPutR3(containerLive, isAsked, hashMatches);
}

}  // namespace

// The ask, the seat's own call cancelled whatever came of it. The host judges the record's class and name, so a record
// with no name is never asked; one too large for its stored form to pack is never asked either.
sg::Verdict AskPutRecord(PutKind kind, uint32_t eid, void* container, const II::Item& rec, void* source) {
    std::wstring name;
    if (!II::ItemName(rec, name)) {
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    const std::vector<uint8_t> wire = IW::ItemWire(rec);
    if (wire.size() > IW::kMaxItemBytes - caw::kPutStoredSlack) {
        UE_LOGW("container_act: put eid=%u: the %ls is past the byte cap a put may ask", eid,
                II::ClassLeaf(rec).c_str());
        ShowBadReason();
        return sg::Verdict::Cancel;
    }
    caw::Request r;
    r.family = static_cast<uint8_t>(ContainerActFamily::PutRecord);
    r.eid = eid;
    r.record = rec;
    Pending p;
    p.kind = kind;
    p.container.Set(container);
    p.source.Set(source);
    // The identity the replay checks again. A held actor's entry carries its pose and its own serialised state, which
    // ticks while the player runs; the hash is the item's identity alone.
    p.hash = internal::PutHandHash(rec);
    p.name = name;
    if (kind == PutKind::Drag) p.entry = rec;
    if (Ask(r, std::move(p)))
        UE_LOGI("container_act: put eid=%u asked %ls", static_cast<unsigned>(eid), II::ClassLeaf(rec).c_str());
    // A refused ask has shown its own line, and an open act shows nothing.
    return sg::Verdict::Cancel;
}

void ResetPutLines() {
    for (bool& said : g_said) said = false;
}

const WatchRow& PutWatch() {
    static const WatchRow kRow{kRowUsedOn, kTagUsedOn, &PreUsedOn, nullptr};
    return kRow;
}

// The granted put: the same verb runs again under the latch if it still names the record the host reserved room for,
// and the commit says where the container stored it. The container's records are read before and after; the commit is
// done exactly when one record was inserted, and carries it as the container stored it (its canvas placement added).
void ReplayPut(const Pending& p) {
    void* const container = p.container.Get();
    std::vector<II::Item> before;
    std::vector<uint64_t> beforeHashes;
    const bool containerLive = ReadRecordHashes(container, before, beforeHashes);
    const char* why = "";
    void* const player = coop::players::Registry::Get().Local();
    E::MainPlayerGrabState grab{};
    void* const hand = player && E::ReadMainPlayerGrabState(player, grab) ? grab.mainHand : nullptr;
    if (CheckReplay(p, hand, containerLive, why) == internal::PutCheck::Abort) {
        NotReplayed(p, why);
        return;
    }
    bool ran = false;
    {
        Latched latch;
        ran = PU::CallPlayerUsedOn(container, player, nullptr, ue_wrap::FVector{}, nullptr, hand, R::FName{});
    }
    // A replay on a shared mirror changes it under the latch, where no verb watch marks it, so the host's next truth for
    // this container must apply even when it equals the last one applied, whatever the commit says. Only a verb that
    // dispatched can have changed it.
    if (ran) coop::props::container_target::ForgetApplied(p.eid);
    std::vector<II::Item> after;
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
            UE_LOGI("container_act: put id=%u eid=%u replayed -> done at index %zu", static_cast<unsigned>(p.id),
                    static_cast<unsigned>(p.eid), *at);
            return;
        }
        // Cannot happen under the size gate; the container has the record and the host will not.
        UE_LOGE("container_act: put id=%u eid=%u the stored %ls does not pack into a commit -- aborted",
                static_cast<unsigned>(p.id), static_cast<unsigned>(p.eid), II::ClassLeaf(done.record).c_str());
    }
    // The verb did not run, or the container stored nothing: the fit failed, the game's own gate swallowed the call, or
    // the player let go of run.
    UE_LOGI("container_act: put id=%u eid=%u replayed ran=%d -> aborted (the container stored nothing)",
            static_cast<unsigned>(p.id), static_cast<unsigned>(p.eid), ran ? 1 : 0);
    SendCommit(p.id, p.eid, 0);
}

}  // namespace coop::props::container_act_request::detail

namespace coop::props::container_act_request::internal {

PutCheck CheckPutR3(bool containerLive, bool handIsAsked, bool hashMatches) {
    return containerLive && handIsAsked && hashMatches ? PutCheck::Replay : PutCheck::Abort;
}

uint64_t PutHandHash(const ue_wrap::inventory_item::Item& record) {
    std::vector<uint8_t> b;
    namespace W = coop::save_record_wire;
    W::AppWStr(b, record.classPath);
    W::AppWStr(b, record.name);
    W::AppWStr(b, record.key);
    return coop::props::container_slice_wire::HashBytes(b);
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
