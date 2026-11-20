// coop/dev/drive_box_drill.cpp -- see coop/dev/drive_box_drill.h.

#include "coop/dev/drive_box_drill.h"

#include "drive_drill_verbs.h"  // Pocket, TakeOut: the player's own verbs (co-located private header)

#include "harness/autotest.h"  // IsClientRole
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/interactables/drive_rack_sync.h"
#include "coop/interactables/signal_wire.h"  // the row texts' caps
#include "coop/net/session.h"
#include "coop/player/hand_record_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_synth_key.h"  // ResolveSetKeyFn: the reuse arm gives a new box the pocketed key
#include "coop/session/host_world_settle.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/item_spawn.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::drive_box_drill {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace DC  = ue_wrap::drive_chain;
namespace EL  = coop::element;
namespace PS  = harness::autotest::peer_signal;
namespace RV  = coop::rig_verdict;
namespace DV  = coop::dev::drive_drill_verbs;
namespace DPR = coop::dev::drill_props;
namespace DRS = coop::drive_rack_sync;

constexpr const char* kTag = "drive-box";
constexpr const char* kTagReuse = "drive-box-reuse";
constexpr int kStepSec = 120;                     // a bound on the other peer's step or a copy's arrival: INVALID only
constexpr int kRows = DC::kRackSlots;             // every row of the box: its record is then as heavy as the lane must carry

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::drive_box_drill); }
constexpr const wchar_t* kBoxCls = L"prop_driveRack_C";

template <class Fn>
void OnGT(Fn&& fn) {
    if (ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); }) != 1)
        UE_LOGE("drive-box: a game-thread task faulted -- what it was to read stays unread");
}

// Poll `pred` on the game thread every 100 ms for at most `seconds`; a bound passing is an INVALID verdict (named).
template <class Pred>
bool WaitGT(const char* what, int seconds, Pred pred) {
    return PS::WaitFor(what, seconds, [&] {
        bool ok = false;
        OnGT([&] { ok = pred(); });
        return ok;
    });
}

// HOST: its player stands and its world has settled, so what the drill spawns is not taken back.
bool HostWorldUp() {
    return WaitGT("the host's world settled and the object index caught up", 360, [] {
        return coop::players::Registry::Get().Local() && coop::host_world_settle::Settled() &&
               ue_wrap::object_index::IsSeeded() && ue_wrap::object_index::Backlog() == 0;
    });
}

// CLIENT: its join is over and the host's puppet stands here.
bool ClientJoined() {
    return WaitGT("this client's join complete", 480, [] {
        coop::RemotePlayer* const host = coop::players::Registry::Get().Puppet(0);
        return harness::session_runtime::Session().connected() && coop::net_pump::HasAnnouncedWorldReady() &&
               coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle && host && host->valid();
    });
}

// `text` repeated to `chars` characters, so a row's four texts sit at signal_wire's caps (96, 48, 64, 64): the heaviest
// row the lane carries, and 16 of them are what the hand lane's byte ceiling has to hold.
std::wstring Padded(const std::wstring& text, size_t chars) {
    std::wstring out = text;
    while (out.size() < chars) out += L'x';
    return out.substr(0, chars);
}

DC::RackRow MakeRow(int k) {
    DC::RackRow r;
    r.has = true;
    r.row.name = Padded(L"DRIVE-BOX-" + std::to_wstring(k) + L"-", coop::signal_wire::kNameCap);
    r.row.id = Padded(L"DB-" + std::to_wstring(k) + L"-", coop::signal_wire::kIdCap);
    r.row.object = Padded(L"drivebox", coop::signal_wire::kObjectCap);
    r.row.signal = Padded(L"drivebox", coop::signal_wire::kSignalCap);
    r.row.size = 10.f;
    r.row.decoded = 10.f;
    r.row.date = 638000000000000000LL;   // non-zero: no receiver stamps one
    r.row.frequency = static_cast<uint8_t>(k);
    r.row.quality = 3;
    return r;
}

// What a box holds, as one text per filled row (its slot, name, id and size); false when its rows do not read.
bool RowsOf(void* rack, std::vector<std::string>& out) {
    DC::RackRow rows[DC::kRackSlots];
    int n = 0;
    out.clear();
    if (!rack || !DC::ReadRack(rack, rows, n)) return false;
    for (int i = 0; i < DC::kRackSlots; ++i) {
        if (!rows[i].has) continue;
        char b[256];
        std::snprintf(b, sizeof b, "%d|%.96ls|%.48ls|%.1f", i, rows[i].row.name.c_str(), rows[i].row.id.c_str(), rows[i].row.size);
        out.push_back(b);
    }
    return true;
}

std::string Narrow(const std::wstring& s) { return std::string(s.begin(), s.end()); }

bool IsBox(void* actor) { return actor && R::IsLive(actor) && DC::EnsureResolved() && DC::IsRackClass(R::ClassOf(actor)); }

// ---- the host ----

// HOST: a box dropped on the floor ahead of the host (the client stands beside it), simulating so a pocket can gather it,
// with all its rows written before the lane's first sight of it. False, said INVALID, when it could not be made.
bool DropFilledBox(const char* tag, ue_wrap::CachedObjRef& box, std::vector<std::string>& want, std::wstring& key) {
    bool spawned = false;
    OnGT([&] {
        void* const me = coop::players::Registry::Get().Local();
        void* const cls = ue_wrap::item_spawn::LoadedClass(kBoxCls);
        ue_wrap::FVector at{};
        if (!me || !cls || !E::TryGetActorLocation(me, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        void* rack = nullptr;
        if (DPR::DropOnFloor(me, cls, nullptr, at.X + fwd.X * 250.f, at.Y + fwd.Y * 250.f, at.Z, 400.f, 50.f, 60.f, &rack) !=
                DPR::Drop::Spawned || !rack)
            return;
        E::SetActorSimulatePhysics(rack, true);
        for (int k = 0; k < kRows; ++k)
            if (!DC::WriteRackRow(rack, k, MakeRow(k))) return;
        DC::CallRackGen(rack);
        box.Set(rack);
        key = ue_wrap::prop::GetInteractableKeyString(rack);
        spawned = RowsOf(rack, want) && static_cast<int>(want.size()) == kRows && !key.empty() && key != L"None";
    });
    if (!spawned) RV::Say(tag, RV::Outcome::Invalid, "the host could not drop and fill a drive box");
    return spawned;
}

// HOST: the box's element id, once it is enrolled and the rack lane has met it.
bool WaitMet(const char* what, ue_wrap::CachedObjRef& box, uint32_t& eid) {
    return WaitGT(what, 60, [&] {
        const EL::ElementId e = EL::Registry::Get().EidForActor(box.Get());
        eid = e == EL::kInvalidId ? 0u : static_cast<uint32_t>(e);
        return eid != 0 && DRS::HostSeesRack(eid);
    });
}

void RunHost() {
    if (!HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    if (!WaitGT("the client's slot world-ready", 120, [] { return harness::session_runtime::Session().IsSlotWorldReady(1); }))
        return;
    if (!PS::Await("client_near", kStepSec)) return;

    ue_wrap::CachedObjRef box;
    std::vector<std::string> want;
    std::wstring key;
    if (!DropFilledBox(kTag, box, want, key)) return;
    uint32_t eid = 0;
    if (!WaitMet("the dropped box enrolled and met by the rack lane", box, eid)) return;
    UE_LOGI("drive-box: HOST box eid %u key '%ls' holds %d row(s)", eid, key.c_str(), kRows);
    PS::Mark("box", std::to_string(eid) + " " + Narrow(key));

    std::string placed;
    if (!PS::AwaitText("placed", placed, kStepSec)) return;
    if (placed[0] != 'P') return;   // the client said its own verdict where it was decided

    // The box the host spawned from the client's placement stands under the pocketed key. The rack lane has met it when
    // its first sight is over: the rows are given back there or never, so that is the point to read them.
    uint32_t newEid = 0;
    void* newBox = nullptr;
    if (!WaitGT("the host's box of the placed key met by the rack lane", kStepSec, [&] {
            newBox = coop::prop_key_index::Find(key);
            if (!IsBox(newBox)) return false;
            const EL::ElementId e = EL::Registry::Get().EidForActor(newBox);
            newEid = e == EL::kInvalidId ? 0u : static_cast<uint32_t>(e);
            return newEid != 0 && newEid != eid && DRS::HostSeesRack(newEid);   // not the pocketed box, still standing
        }))
        return;
    std::vector<std::string> got;
    OnGT([&] { RowsOf(newBox, got); });
    UE_LOGI("drive-box: HOST the placed box eid %u holds %zu row(s) at the lane's first sight (pocketed with %zu)", newEid,
            got.size(), want.size());
    if (got != want) {
        PS::Mark("host_judged", "F " + std::to_string(newEid));
        RV::Say(kTag, RV::Outcome::Fail, "the box a client took out of its inventory came back with %zu of the %zu row(s) it was pocketed with, on the host",
                got.size(), want.size());
        return;
    }
    PS::Mark("host_judged", "P " + std::to_string(newEid));
    std::string done;
    if (!PS::AwaitText("client_done", done, kStepSec) || done[0] != 'P') return;
    RV::Say(kTag, RV::Outcome::Pass, "a drive box pocketed with %zu row(s) and taken out again holds them on the host and on the client", want.size());
}

// The reuse arm, host alone: a box that goes holding drives leaves its rows with the lane under its key; a NEW box that
// merely reuses the key (no client placed it) is a new box and keeps its own rows, here none. Decided at that box's first
// sight, where the rows are given back or never.
void RunHostReuse() {
    if (!HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    if (!WaitGT("the client's slot world-ready", 120, [] { return harness::session_runtime::Session().IsSlotWorldReady(1); }))
        return;
    ue_wrap::CachedObjRef box;
    std::vector<std::string> want;
    std::wstring key;
    if (!DropFilledBox(kTagReuse, box, want, key)) return;
    uint32_t eid = 0;
    if (!WaitMet("the dropped box enrolled and met by the rack lane", box, eid)) return;
    // The box goes (as a sale or a level's reset takes one, no client placing anything), and the lane notes it gone.
    OnGT([&] { E::DestroyActor(box.Get()); });
    if (!WaitGT("the lane keeping the rows of the destroyed box", kStepSec, [&] { return DRS::HostKeepsRowsOf(key); })) return;

    ue_wrap::CachedObjRef fresh;
    bool made = false;
    OnGT([&] {
        void* const me = coop::players::Registry::Get().Local();
        void* const cls = ue_wrap::item_spawn::LoadedClass(kBoxCls);
        ue_wrap::FVector at{};
        if (!me || !cls || !E::TryGetActorLocation(me, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        void* rack = E::SpawnActor(cls, {at.X + fwd.X * 250.f, at.Y + fwd.Y * 250.f, at.Z + 60.f});
        void* const setKey = rack ? coop::prop_synth_key::ResolveSetKeyFn(R::ClassOf(rack)) : nullptr;
        const R::FName kf = ue_wrap::fname_utils::StringToFName(key);
        ue_wrap::ParamFrame f(setKey);
        if (!setKey || kf.ComparisonIndex == 0 || !f.valid() || !f.SetRaw(L"Key", &kf, sizeof(kf)) || !ue_wrap::Call(rack, f)) return;
        fresh.Set(rack);
        made = ue_wrap::prop::GetInteractableKeyString(rack) == key;
    });
    if (!made) {
        RV::Say(kTagReuse, RV::Outcome::Invalid, "the host could not spawn a new box under the pocketed key");
        return;
    }
    uint32_t freshEid = 0;
    if (!WaitMet("the new box enrolled and met by the rack lane", fresh, freshEid)) return;
    std::vector<std::string> got;
    OnGT([&] { RowsOf(fresh.Get(), got); });
    UE_LOGI("drive-box: HOST the new box eid %u under the key of a box that went with %zu row(s) holds %zu row(s)", freshEid,
            want.size(), got.size());
    if (!got.empty()) {
        RV::Say(kTagReuse, RV::Outcome::Fail, "a new box that reuses a gone box's key was given its %zu row(s), no client having placed it", got.size());
        return;
    }
    RV::Say(kTagReuse, RV::Outcome::Pass, "a new box that reuses the key of a box that went holding %zu row(s) kept its own (none)", want.size());
}

// ---- the client ----

// Does the carried inventory hold a record under `key`?
bool InventoryHoldsKey(const std::wstring& key) {
    ue_wrap::inventory::LivePersonalStore store;
    if (!ue_wrap::inventory::ReadLivePersonalStore(store)) return false;
    for (const auto& rec : store.records)
        if (rec.key == key) return true;
    return false;
}

void RunClient() {
    if (!ClientJoined()) return;
    PS::Mark("client_ready");
    if (!harness::autotest::meet::WalkToHost(kTag, 300.f)) return;
    PS::Mark("client_near");
    std::string text;
    if (!PS::AwaitText("box", text, kStepSec)) return;
    unsigned eid = 0;
    char keyText[160] = {};
    if (sscanf_s(text.c_str(), "%u %159s", &eid, keyText, static_cast<unsigned>(sizeof keyText)) != 2) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host's box text '%s' does not parse", text.c_str());
        return;
    }
    const std::wstring key(keyText, keyText + std::strlen(keyText));

    // The copy: holds the host's rows, rests gatherable. What it holds is what the take-out must give back.
    std::vector<std::string> want;
    void* copy = nullptr;
    if (!WaitGT("the copy of the host's box holding its rows, gatherable", kStepSec, [&] {
            copy = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop);
            return IsBox(copy) && RowsOf(copy, want) && static_cast<int>(want.size()) == kRows &&
                   ue_wrap::prop_gather::IsGatherable(copy);
        }))
        return;

    // The box's record against the hand lane's bounds and its byte ceiling: a held box's record past either is refused
    // or never sent, and a placed box then carries nothing. The 16 rows are at the wire's caps, the heaviest the lane
    // carries, so the measured size is the one the ceiling has to hold.
    std::string fault;
    size_t bytes = 0;
    OnGT([&] {
        ue_wrap::save_record::SaveRecord rec;
        if (!ue_wrap::save_record::CaptureRecord(copy, rec)) return;
        fault = coop::hand_record_sync::BoundsFault(rec);
        bytes = coop::hand_record_sync::EncodedBytes(rec);
    });
    UE_LOGI("drive-box: CLIENT the box's record is %zu B of the lane's %zu B ceiling", bytes,
            coop::hand_record_sync::RecordCeilingBytes());
    if (fault.empty() && bytes > coop::hand_record_sync::RecordCeilingBytes())
        fault = "its encoded record is " + std::to_string(bytes) + " B, over the lane's " +
                std::to_string(coop::hand_record_sync::RecordCeilingBytes()) + " B ceiling";
    if (bytes == 0) fault = "the box's record could not be read";
    if (!fault.empty()) {
        PS::Mark("placed", "F");
        RV::Say(kTag, RV::Outcome::Fail, "the box's record is past the hand lane's limits: %s", fault.c_str());
        return;
    }

    // The pocket: the game's own verb on the mirror, which is asked of the host; the inventory says whether it ran.
    bool pocketed = false;
    OnGT([&] {
        void* const me = coop::players::Registry::Get().Local();
        pocketed = me && DV::Pocket(me, copy);
    });
    if (!pocketed) {
        if (!WaitGT("the host's answer to the pocket", kStepSec, [] { return coop::prop_act_request::dev::PendingId() == 0; }))
            return;
        OnGT([&] { pocketed = InventoryHoldsKey(key); });
    }
    if (!pocketed) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host refused the pocket of the box");
        return;
    }

    // The take-out: the inventory screen's own, which spawns the box at the player and loads its record. The spawn is the
    // placement: its key was parked at the pocket, so it goes to the host as a drop intent.
    void* out = nullptr;
    std::vector<std::string> own;
    OnGT([&] {
        out = DV::TakeOut(kBoxCls, key);
        if (out) RowsOf(out, own);
    });
    if (!out) {
        RV::Say(kTag, RV::Outcome::Invalid, "the pocketed box did not come back out of the inventory");
        return;
    }
    UE_LOGI("drive-box: CLIENT the box taken out holds %zu row(s) on this copy (pocketed with %zu)", own.size(), want.size());
    PS::Mark("placed", "P");

    // The host judged the box at its first sight; a FAIL there is said by the host.
    std::string judged;
    if (!PS::AwaitText("host_judged", judged, kStepSec) || judged[0] != 'P') return;
    unsigned newEid = 0;
    if (sscanf_s(judged.c_str() + 1, "%u", &newEid) != 1 || newEid == 0) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host's judged text '%s' names no box", judged.c_str());
        return;
    }

    // The host's canonical of THIS box (its eid) is applied here, then this box's rows are the pocketed rows. The client's
    // own take-out already holds them, so only the canonical's arrival makes the reading the host's.
    if (!WaitGT("the host's canonical of the placed box applied here", kStepSec,
                [&] { return DRS::CanonicalsAppliedFor(newEid) > 0; }))
        return;
    std::vector<std::string> got;
    OnGT([&] {
        void* const box = coop::prop_key_index::Find(key);
        if (IsBox(box)) RowsOf(box, got);
    });
    UE_LOGI("drive-box: CLIENT after the host's canonical of eid %u the box holds %zu row(s)", newEid, got.size());
    if (got != want) {
        PS::Mark("client_done", "F");
        RV::Say(kTag, RV::Outcome::Fail, "the box holds %zu of the %zu row(s) it was pocketed with, on the client, after the host's canonical",
                got.size(), want.size());
        return;
    }
    PS::Mark("client_done", "P");
}

// The reuse arm asks nothing of the client but a join: the host's world has a peer and its lane sweeps as in a session.
void RunClientReuse() {
    if (!ClientJoined()) return;
    PS::Mark("client_ready");
}

}  // namespace

DWORD WINAPI Thread(LPVOID /*arg*/) {
    const bool reuse = Arm() == "reuse";
    if (!PS::Begin(reuse ? kTagReuse : kTag)) return 0;
    if (harness::autotest::IsClientRole()) {
        if (reuse) RunClientReuse();
        else RunClient();
    } else if (reuse) {
        RunHostReuse();
    } else {
        RunHost();
    }
    return 0;
}

}  // namespace coop::dev::drive_box_drill
