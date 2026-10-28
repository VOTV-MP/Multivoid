// coop/dev/late_join_drill_host.cpp -- see coop/dev/late_join_drill.h: the host's half. Once the host's own world is up,
// before any client joins, it stages one subject of each converted lane in front of its own player, a step at a time,
// each step bound by 60 s: an open portable PC, a floppy box, a Kerfus switched on, a drive rack with two rows, and a
// drive seated in an empty slot with a row of its own. The client that joins afterwards gets all of them in its join's
// seeds. Each recipe is lifted from the drill that proved it (named at its function), not shared: a drill's helpers
// stay its own.

#include "late_join_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/kerfus_state.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/devices/portable_pc.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::late_join_drill::detail {
namespace {

namespace PPC = ue_wrap::portable_pc;
namespace UK  = ue_wrap::kerfus;
namespace DC  = ue_wrap::drive_chain;
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace EL  = coop::element;
using Clock = std::chrono::steady_clock;

constexpr auto kStepBound = std::chrono::seconds(60);
constexpr float kSideCm = 120.f;  // the staged props stand this far apart, sideways from the player's line
constexpr uint8_t kActionOpen = 0xA;  // the PC's own lid-open action

constexpr const wchar_t* kPcPath = L"/Game/objects/prop_portablePc.prop_portablePc_C";
constexpr const wchar_t* kBoxPath = L"/Game/objects/prop_floppyBox.prop_floppyBox_C";
constexpr const wchar_t* kKerfusPath = L"/Game/objects/p_kerfus.p_kerfus_C";
constexpr const wchar_t* kRackPath = L"/Game/objects/prop_driveRack.prop_driveRack_C";

enum Subject : int { kPc, kBox, kKerfus, kRack, kDrive, kSubjects };
constexpr const char* kSubjectName[kSubjects] = {"pc", "box", "kerfus", "rack", "drive"};

enum class Ph : uint8_t {
    Ready, Pc, PcNamed, Box, BoxNamed, Kerfus, KerfusNamed, KerfusKnown, Rack, RackNamed, Drive, DriveNamed,
    DriveSeat, Done
};

Ph g_ph = Ph::Ready;
Clock::time_point g_stepAt{};
int g_readyTicks = 0;
uint32_t g_world = 0;  // the world generation the leg began in; a new world is staged anew
ue_wrap::CachedObjRef g_actor[kSubjects];
uint32_t g_eid[kSubjects] = {};
int g_slotRole = -1;
bool g_slotWasOccupied = false;  // the drive is the slot's own: every slot held one

void Enter(Ph p) { g_ph = p; g_stepAt = Clock::now(); }
bool Expired() { return Clock::now() - g_stepAt >= kStepBound; }

void Fail(int subject, const char* why) {
    UE_LOGW("[LATE-JOIN] host FAIL: %s -- %s", kSubjectName[subject], why);
    Enter(Ph::Done);
}

// The class at `path`, the way lid_drill loads the PC's; `findName`, when given, is the loaded-class lookup tried
// when the load gives nothing (rack_birth_drill's way). Says which of the two worked.
void* LoadClass(int subject, const wchar_t* path, const wchar_t* findName = nullptr) {
    if (void* cls = ue_wrap::asset_load::LoadObjectByPath(path)) {
        UE_LOGI("[LATE-JOIN] host class %s: loaded by path", kSubjectName[subject]);
        return cls;
    }
    if (findName) {
        if (void* cls = R::FindClass(findName)) {
            UE_LOGI("[LATE-JOIN] host class %s: the path load failed, found among the loaded classes",
                    kSubjectName[subject]);
            return cls;
        }
    }
    return nullptr;
}

// A prop of class `cls` in front of the host's player (lid_drill's SpawnInFrontOfPlayer), 150 cm out and 40 up, the
// subject's own lane of kSideCm sideways so none overlap.
void* SpawnInFront(int subject, void* cls) {
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!player || !cls || !E::TryGetActorLocation(player, at)) return nullptr;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(player);
    const float side = (static_cast<float>(subject) - 2.f) * kSideCm;
    return E::SpawnActor(cls, {at.X + fwd.X * 150.f - fwd.Y * side, at.Y + fwd.Y * 150.f + fwd.X * side, at.Z + 40.f});
}

// Spawns one subject and moves to `next`; a failed load or spawn ends the leg.
bool Stage(int subject, void* cls, Ph next) {
    if (!cls) {
        Fail(subject, "the class could not be loaded");
        return false;
    }
    void* actor = SpawnInFront(subject, cls);
    if (!actor) {
        Fail(subject, "the spawn returned nothing");
        return false;
    }
    g_actor[subject].Set(actor);
    g_eid[subject] = 0;
    Enter(next);
    return true;
}

// True once the element lane has named the subject's actor (its eid kept); a lost actor or a spent bound ends the leg.
bool Named(int subject) {
    void* actor = g_actor[subject].Get();
    if (!actor) {
        Fail(subject, "the spawned actor went away");
        return false;
    }
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    if (eid == EL::kInvalidId || eid == 0) {
        if (Expired()) Fail(subject, "the element lane did not name it within 60 s");
        return false;
    }
    g_eid[subject] = static_cast<uint32_t>(eid);
    return true;
}

// lid_drill's UseAction: the PC's own use verb with one of its lid actions, as a player's selection runs it.
bool UseAction(void* pc, void* player, uint8_t action) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pc), L"actionOptionIndex");
    if (!fn || !player) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && f.Set<uint8_t>(L"action", action) &&
           ue_wrap::Call(pc, f);
}

// rack_birth_drill's MakeRow: fixed content, a non-zero date (no receiver stamps one).
DC::RackRow MakeRackRow(int k) {
    DC::RackRow r;
    r.has = true;
    r.row.name = std::wstring(kRackRowPrefix) + std::to_wstring(k);
    r.row.id = L"LJ-" + std::to_wstring(k);
    r.row.object = L"latejoin";
    r.row.signal = L"latejoin";
    r.row.size = 10.f;
    r.row.decoded = 10.f;
    r.row.date = 638000000000000000LL;
    r.row.frequency = static_cast<uint8_t>(k);
    r.row.quality = 3;
    return r;
}

// drive_drill's WriteRow with a name of the drill's own: a row that differs from the class default, so its
// payload ships.
bool WriteDriveName(void* drive) {
    ue_wrap::signal_dynamic::Row row;
    if (!drive || !DC::ReadDriveRow(drive, row)) return false;
    row.name = kDriveName;
    row.id = std::wstring(kDriveName) + L"-ID";
    row.size = 10.f;
    row.decoded = 10.f;
    row.level = 2;
    if (!DC::WriteDriveRow(drive, row)) return false;
    DC::CallDriveUpd(drive);
    return true;
}

// desk_verb_drill_desk's ArmDrive, split into this drill's steps: an empty slot is chosen, else the first slot's own
// drive stands in.
void ChooseSlot() {
    DC::EnsureResolved();
    void* occupied = nullptr;
    for (int r = 0; r < DC::kRoleCount; ++r) {
        void* slot = DC::SlotActor(r);
        if (!slot) continue;
        void* held = DC::SlotDrive(slot);
        if (!held) {
            g_slotRole = r;
            g_slotWasOccupied = false;
            return;
        }
        if (!occupied) {
            occupied = held;
            g_slotRole = r;
        }
    }
    if (!occupied) {
        g_slotRole = -1;
        return;
    }
    g_slotWasOccupied = true;
    g_actor[kDrive].Set(occupied);
}

}  // namespace

void HostTick(coop::net::Session& s) {
    switch (g_ph) {
    case Ph::Ready: {
        if (!s.running() || ++g_readyTicks < 2) return;
        UE_LOGI("[LATE-JOIN] host ARMED: the session is hosting");
        Enter(Ph::Pc);
        return;
    }
    case Ph::Pc:
        // The host's world may still be loading: the local player resolves later, inside this step's bound.
        if (!coop::players::Registry::Get().Local()) {
            if (Expired()) Fail(kPc, "the local player did not resolve within 60 s");
            return;
        }
        g_world = ue_wrap::world_identity::Generation();
        Stage(kPc, LoadClass(kPc, kPcPath), Ph::PcNamed);
        return;
    case Ph::PcNamed: {
        if (!Named(kPc)) return;
        void* pc = g_actor[kPc].Get();
        bool opened = false;
        if (!PPC::IsPortablePc(pc) || !PPC::ReadOpened(pc, opened)) {
            Fail(kPc, "the spawned actor is no readable portable PC");
            return;
        }
        if (!opened && (!UseAction(pc, coop::players::Registry::Get().Local(), kActionOpen) ||
                        !PPC::ReadOpened(pc, opened) || !opened)) {
            Fail(kPc, "the PC's action 10 did not open its lid");
            return;
        }
        UE_LOGI("[LATE-JOIN] host opened the PC's lid (eid=%u)", g_eid[kPc]);
        Enter(Ph::Box);
        return;
    }
    case Ph::Box:
        Stage(kBox, LoadClass(kBox, kBoxPath), Ph::BoxNamed);
        return;
    case Ph::BoxNamed:
        if (Named(kBox)) Enter(Ph::Kerfus);
        return;
    case Ph::Kerfus:
        Stage(kKerfus, LoadClass(kKerfus, kKerfusPath), Ph::KerfusNamed);
        return;
    case Ph::KerfusNamed: {
        if (!Named(kKerfus)) return;
        void* k = g_actor[kKerfus].Get();
        if (!UK::IsKerfus(k) || !UK::WriteActive(k, true) || !UK::WriteEnergy(k, kKerfusEnergy) ||
            !UK::RunUpd(k, false)) {
            Fail(kKerfus, "the Kerfus's state could not be written");
            return;
        }
        Enter(Ph::KerfusKnown);
        return;
    }
    case Ph::KerfusKnown:
        if (!g_actor[kKerfus].Get()) {
            Fail(kKerfus, "the spawned actor went away");
        } else if (coop::kerfus_state::HostKnows(g_eid[kKerfus])) {
            UE_LOGI("[LATE-JOIN] host switched the Kerfus on (eid=%u), its tick watch has it", g_eid[kKerfus]);
            Enter(Ph::Rack);
        } else if (Expired()) {
            Fail(kKerfus, "its tick watch did not enter it within 60 s");
        }
        return;
    case Ph::Rack:
        Stage(kRack, LoadClass(kRack, kRackPath, L"prop_driveRack_C"), Ph::RackNamed);
        return;
    case Ph::RackNamed: {
        if (!Named(kRack)) return;
        void* rack = g_actor[kRack].Get();
        for (int k = 0; k < kRackRows; ++k) {
            if (!DC::WriteRackRow(rack, k, MakeRackRow(k))) {
                Fail(kRack, "the rack's rows could not be written");
                return;
            }
        }
        DC::CallRackGen(rack);
        UE_LOGI("[LATE-JOIN] host filled the rack's %d rows (eid=%u)", kRackRows, g_eid[kRack]);
        Enter(Ph::Drive);
        return;
    }
    case Ph::Drive: {
        ChooseSlot();
        if (g_slotRole < 0) {
            Fail(kDrive, "no drive slot is resolved");
            return;
        }
        if (g_slotWasOccupied) {
            UE_LOGI("[LATE-JOIN] host: every slot holds a drive, slot %d's own is used", g_slotRole);
            Enter(Ph::DriveNamed);
            return;
        }
        void* slot = DC::SlotActor(g_slotRole);
        ue_wrap::FVector at{};
        if (!slot || !DC::DriveClass() || !E::TryGetActorLocation(slot, at)) {
            Fail(kDrive, "the drive class or the slot's location is not available");
            return;
        }
        at.Z += 40.f;
        void* drive = E::SpawnActor(DC::DriveClass(), at);
        if (!drive) {
            Fail(kDrive, "the spawn returned nothing");
            return;
        }
        g_actor[kDrive].Set(drive);
        Enter(Ph::DriveNamed);
        return;
    }
    case Ph::DriveNamed: {
        if (!Named(kDrive)) return;
        void* drive = g_actor[kDrive].Get();
        void* slot = DC::SlotActor(g_slotRole);
        if (!WriteDriveName(drive)) {
            Fail(kDrive, "the drive's row could not be written");
            return;
        }
        if (g_slotWasOccupied) {
            Enter(Ph::DriveSeat);
            return;
        }
        if (!slot || !DC::CallPutDriveIn(slot, drive)) {
            Fail(kDrive, "putDriveIn did not take the drive");
            return;
        }
        Enter(Ph::DriveSeat);
        return;
    }
    case Ph::DriveSeat: {
        void* slot = DC::SlotActor(g_slotRole);
        if (slot && DC::SlotDrive(slot) == g_actor[kDrive].Get()) {
            UE_LOGI("[LATE-JOIN] host STAGED pc=%u box=%u kerfus=%u rack=%u drive=%u slotRole=%d", g_eid[kPc],
                    g_eid[kBox], g_eid[kKerfus], g_eid[kRack], g_eid[kDrive], g_slotRole);
            Enter(Ph::Done);
        } else if (Expired()) {
            Fail(kDrive, "the slot did not hold the drive within 60 s");
        }
        return;
    }
    case Ph::Done:
        // A new world (the host started over) is staged anew; the same world's later sessions keep what stands.
        if (g_world != ue_wrap::world_identity::Generation()) {
            g_readyTicks = 0;
            Enter(Ph::Ready);
        }
        return;
    }
}

}  // namespace coop::dev::late_join_drill::detail
