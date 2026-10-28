// coop/dev/late_join_drill.cpp -- see coop/dev/late_join_drill.h. The client's idle edge, its two readings, its check
// that the world the host staged is in its world and its verdict on the second reading (the tallies are bind_wait's
// own); the host's staging is late_join_drill_host.cpp.

#include "coop/dev/late_join_drill.h"

#include "late_join_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/bind_wait.h"
#include "coop/element/registry.h"
#include "coop/interactables/floppybox_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"
#include "coop/session/join_progress.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/devices/floppybox.h"
#include "ue_wrap/devices/portable_pc.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::late_join_drill {
namespace {

namespace BW = coop::element::bind_wait;

constexpr uint64_t kSecondReadingMs = 10000;
constexpr size_t kJudgedKinds = 7;  // the first seven of kKinds are the lanes the verdict judges; PropDriveEnd is not

struct Kind {
    const char* name;
    coop::net::ReliableKind kind;
};

constexpr Kind kKinds[] = {
    {"ContainerContents", coop::net::ReliableKind::ContainerContents},
    {"FloppyBoxState", coop::net::ReliableKind::FloppyBoxState},
    {"LaptopState", coop::net::ReliableKind::LaptopState},
    {"KerfusState", coop::net::ReliableKind::KerfusState},
    {"DrivePayload", coop::net::ReliableKind::DrivePayload},
    {"RackState", coop::net::ReliableKind::RackState},
    {"DriveSlotState", coop::net::ReliableKind::DriveSlotState},
    {"PropDriveEnd", coop::net::ReliableKind::PropDriveEnd},
};

bool g_leftIdle = false;   // the join phase has been non-idle this session
bool g_firstDone = false;  // the at=idle reading is printed
bool g_done = false;       // the DONE line is printed
bool g_stagedOk = false;   // the at=idle reading found the staged world in this client's world
uint64_t g_idleAtMs = 0;

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::late_join_drill);
    return s;
}

// The arm this client runs: its own bind wait marks unknown eids (the host's gate red) or it does not.
bool HoldArm() { return coop::config::ResolveFlag(::coop::config_registry::rows::bind_wait_mark_unknown); }

void Print(const char* at) {
    for (const Kind& k : kKinds) {
        const uint16_t n = static_cast<uint16_t>(k.kind);
        UE_LOGI("[LATE-JOIN] tally at=%s kind=%s(%u) held=%u coalesced=%u replayed=%u dropped=%u givenUp=%u neverBound=%u",
                at, k.name, static_cast<unsigned>(n), BW::HeldOf(n), BW::CoalescedOf(n), BW::ReplayedOf(n),
                BW::DroppedOf(n), BW::GivenUpOf(n), BW::NeverBoundOf(n));
    }
    UE_LOGI("[LATE-JOIN] totals at=%s deferred=%u held=%u replayed=%u givenUp=%u refused=%u neverBound=%u holdsNow=%d",
            at, BW::DeferredCount(), BW::HeldCount(), BW::ReplayedCount(), BW::GivenUpCount(), BW::RefusedCount(),
            BW::DropsNeverBound(), BW::HasHolds() ? 1 : 0);
}

// ---- the verdict -------------------------------------------------------------------------------------------------

// One verdict's checks: every failing one prints its own line, the arm's name leading, then Done says the result.
struct Verdict {
    const char* arm;
    int passed = 0;
    int failed = 0;

    void Check(bool ok, const char* fmt, ...) {
        if (ok) {
            ++passed;
            return;
        }
        ++failed;
        char text[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        UE_LOGW("[LATE-JOIN] %s FAIL: %s", arm, text);
    }
    void Done() const {
        if (failed == 0) UE_LOGI("[LATE-JOIN] %s PASS (%d checks)", arm, passed);
    }
};

// A Kerfus that is on drains, so its energy at the reading sits a little under what the host wrote and no higher; a
// default Kerfus is not within kEnergyBelow of kKerfusEnergy unless it was set.
constexpr float kEnergyBelow = 10.f;
constexpr float kEnergyAbove = 0.5f;

// What the client's own copies of the staged subjects read. A fresh world holds no floppy box and no open PC of its
// own, so those two are told by count (exactly one of each); the Kerfus is found by being on, the rack by its rows'
// names, the drive by its row name in any slot role.
struct Effects {
    int pcCount = 0, pcOpenCount = 0;
    int boxCount = 0;
    bool boxBound = false;
    bool anyKerfus = false, anyRack = false, driveInRegistry = false;
    bool kerfusFound = false, kerfusOn = false;
    float kerfusEnergy = -1.f;
    bool rackFound = false;
    int rackRows = -1;
    bool driveSeated = false;
};

Effects ReadEffects() {
    namespace PPC = ue_wrap::portable_pc;
    namespace DC = ue_wrap::drive_chain;
    namespace UK = ue_wrap::kerfus;
    namespace R = ue_wrap::reflection;
    Effects e;
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    const std::wstring prefix = detail::kRackRowPrefix;
    for (const auto& pr : pairs) {
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        void* cls = R::ClassOf(pr.actor);
        bool opened = false;
        if (PPC::IsPortablePc(pr.actor)) {
            ++e.pcCount;
            if (PPC::ReadOpened(pr.actor, opened) && opened) ++e.pcOpenCount;
        }
        if (ue_wrap::floppybox::IsFloppyBoxClass(cls)) {
            ++e.boxCount;
            if (coop::element::Registry::Get().EidForActor(pr.actor) != 0) e.boxBound = true;
        }
        if (UK::IsKerfus(pr.actor)) {
            e.anyKerfus = true;
            bool on = false;
            float energy = 0.f;
            if (UK::ReadActive(pr.actor, on) && UK::ReadEnergy(pr.actor, energy) && (on || !e.kerfusFound)) {
                e.kerfusFound = true;
                e.kerfusOn = on;
                e.kerfusEnergy = energy;
            }
        }
        ue_wrap::signal_dynamic::Row driveRow;
        if (DC::IsDriveClass(cls) && DC::ReadDriveRow(pr.actor, driveRow) && driveRow.name == detail::kDriveName)
            e.driveInRegistry = true;
        if (DC::IsRackClass(cls)) {
            e.anyRack = true;
            DC::RackRow rows[DC::kRackSlots];
            int num = 0;
            if (!DC::ReadRack(pr.actor, rows, num)) continue;
            int has = 0;
            bool staged = false;
            for (int i = 0; i < num && i < DC::kRackSlots; ++i) {
                if (!rows[i].has) continue;
                ++has;
                if (rows[i].row.name.compare(0, prefix.size(), prefix) == 0) staged = true;
            }
            if (staged) {
                e.rackFound = true;
                e.rackRows = has;
            }
        }
    }
    DC::EnsureResolved();
    ue_wrap::signal_dynamic::Row row;
    for (int role = 0; role < DC::kRoleCount && !e.driveSeated; ++role) {
        void* slot = DC::SlotActor(role);
        void* drive = slot ? DC::SlotDrive(slot) : nullptr;
        if (drive && DC::ReadDriveRow(drive, row) && row.name == detail::kDriveName) e.driveSeated = true;
    }
    return e;
}

// The staged-world check, once, at the join's idle reading: the five subjects stand in this client's world. It asks
// only that each is here, never what state it holds: the PC's open lid is a seed the verdict judges, so a lost seed
// fails there, beside the count of drops that lost it. The failure line names what was found (the PC and the box by
// count). The verdict is judged only when it holds.
bool CheckStagedWorld(bool hold) {
    const Effects e = ReadEffects();
    if (e.pcCount >= 1 && e.boxCount == 1 && e.anyKerfus && e.anyRack && e.driveInRegistry) return true;
    UE_LOGW("[LATE-JOIN] %s FAIL: the staged world was not in this join (pc=%d box=%d kerfus=%d rack=%d drive=%d)",
            hold ? "hold" : "clean", e.pcCount, e.boxCount, e.anyKerfus ? 1 : 0, e.anyRack ? 1 : 0,
            e.driveInRegistry ? 1 : 0);
    return false;
}

// The joiner's verdict, read at idle+10s in the one life, which joined into the staged world. `hold` is the arm whose
// bind wait marks unknown eids (the host's gate red): every seed that landed before its prop was held and must have
// replayed. `clean` is the gate on: nothing named a prop before it was here, so nothing was held and nothing was
// lost. Both arms: nothing refused or given up, no hold left, and every staged effect stands on this client.
void Judge(bool hold) {
    Verdict v{hold ? "hold" : "clean"};
    v.Check(BW::RefusedCount() == 0, "refused=%u, expected 0", BW::RefusedCount());
    v.Check(BW::GivenUpCount() == 0, "givenUp=%u, expected 0", BW::GivenUpCount());
    v.Check(!BW::HasHolds(), "holds are still pending");

    const Effects e = ReadEffects();
    v.Check(e.pcOpenCount == 1, "open portable PCs=%d, expected exactly 1", e.pcOpenCount);
    v.Check(e.kerfusFound && e.kerfusOn && e.kerfusEnergy > detail::kKerfusEnergy - kEnergyBelow &&
                e.kerfusEnergy <= detail::kKerfusEnergy + kEnergyAbove,
            "Kerfus found=%d on=%d energy=%.2f, expected on with energy in (%.1f, %.1f]", e.kerfusFound ? 1 : 0,
            e.kerfusOn ? 1 : 0, static_cast<double>(e.kerfusEnergy),
            static_cast<double>(detail::kKerfusEnergy - kEnergyBelow),
            static_cast<double>(detail::kKerfusEnergy + kEnergyAbove));
    v.Check(e.rackFound && e.rackRows == detail::kRackRows, "staged rack found=%d rows=%d, expected %d",
            e.rackFound ? 1 : 0, e.rackRows, detail::kRackRows);
    v.Check(e.driveSeated, "no slot role holds a drive named LATE-JOIN-DRIVE");
    v.Check(e.boxCount == 1, "floppy boxes=%d, expected exactly 1", e.boxCount);
    v.Check(e.boxBound, "no floppy box is bound");
    const uint32_t canonicals = coop::floppybox_sync::AppliedCanonicals();
    v.Check(canonicals >= 1, "floppy box canonicals applied=%u, expected >= 1", canonicals);
    const size_t slices = coop::props::container_contents_sync::AppliedEids();
    const size_t heldSlices = static_cast<size_t>(BW::HeldOf(static_cast<uint16_t>(kKinds[0].kind)));
    if (hold)
        v.Check(slices >= heldSlices, "container slices applied=%zu, expected >= held=%zu", slices, heldSlices);
    else
        v.Check(slices > 0, "container slices applied=%zu, expected > 0", slices);

    if (hold) {
        for (size_t i = 0; i < kJudgedKinds; ++i) {
            const uint16_t n = static_cast<uint16_t>(kKinds[i].kind);
            const uint32_t held = BW::HeldOf(n), replayed = BW::ReplayedOf(n), dropped = BW::DroppedOf(n);
            const uint32_t givenUp = BW::GivenUpOf(n), neverBound = BW::NeverBoundOf(n);
            v.Check(held > 0, "%s held=0, expected > 0", kKinds[i].name);
            v.Check(held == replayed + dropped + givenUp, "%s held=%u != replayed=%u + dropped=%u + givenUp=%u",
                    kKinds[i].name, held, replayed, dropped, givenUp);
            v.Check(neverBound == 0, "%s neverBound=%u, expected 0", kKinds[i].name, neverBound);
        }
    } else {
        v.Check(BW::DropsNeverBound() == 0, "neverBound=%u, expected 0 with the gate on", BW::DropsNeverBound());
    }
    v.Done();
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Enabled() || !session) return;
    if (session->role() == coop::net::Role::Host) {
        detail::HostTick(*session);
        return;
    }
    if (session->role() != coop::net::Role::Client || g_done) return;
    const bool idle = coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    if (!g_firstDone) {
        if (!idle) {
            g_leftIdle = true;
            return;
        }
        if (!g_leftIdle) return;
        Print("idle");
        g_stagedOk = CheckStagedWorld(HoldArm());
        g_firstDone = true;
        g_idleAtMs = coop::net::NowMs();
        return;
    }
    if (coop::net::NowMs() - g_idleAtMs < kSecondReadingMs) return;
    Print("idle+10s");
    if (g_stagedOk) Judge(HoldArm());
    UE_LOGI("[LATE-JOIN] client DONE");
    g_done = true;
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_leftIdle = false;
    g_firstDone = false;
    g_done = false;
    g_stagedOk = false;
    g_idleAtMs = 0;
}

}  // namespace coop::dev::late_join_drill
