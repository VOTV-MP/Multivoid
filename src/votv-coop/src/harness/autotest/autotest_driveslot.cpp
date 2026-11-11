// harness/autotest/autotest_driveslot.cpp -- a drive taken out of the desk's play slot by one peer, three ways
// (VOTVCOOP_RUN_DRIVESLOT=grab|hold|pocket on both peers; "1" is grab). The entry points are in
// harness/autotest/driveslot.h.
//
// What a player does with a drive in the play slot, from the 0.10 bytecode (capture-0.10.0-b284.txt):
//   grab    prop_drive_C.playerTryToGrab only calls slot.drivePulledOut and keeps the actor: a physics grab, the drive
//           carried on the handle and dropped where the player lets go.
//   hold    playerTryToHold -> driveAction(player, collect=false): pulls the drive out (drivePulledOut, slot=None,
//           simulate physics) and then calls player.Add Object to Free Hotbar Slot(Self, ...), which DESTROYS the world
//           actor on success; the hand's actor is respawned from the slot's own data.
//   pocket  playerTryToCollect -> driveAction(collect=true), then addObjectToInventory: the same, into the bag.
// So hold and pocket end with no world drive at all; a prop_C standing where the drive was is the hotbar's own, not a
// stub, and this test no longer reads the drive's destruction as a fault (the 0.9 version did, and judged the host's
// destroyed copy INCONCLUSIVE).
//
// In every arm the HOST births a drive, writes a recognisable signal into its data_0 (name, id and size; the row lane
// carries it to the client), and seats it in the play slot. The CLIENT, once the drive is in its slot with the signal
// on it, drives the action as a player's press does: grab runs the drive's playerTryToGrab and playerGrabbed_pre and a
// physics-handle carry; hold and pocket call the drive's own playerTryToHold / playerTryToCollect, whose inner
// hotbar / inventory call is the one the mod's act watch cancels (coop/props/prop_act_request): the client asks the
// host, the host grants and the client replays under the latch and commits, the host destroys its copy. Each peer
// judges its own copy and says one "driveslot: VERDICT <host|client> <arm> PASS|FAIL" line:
//   grab    slot empty; the same drive (eid) is live, unfrozen and off the port (the host's following the client's
//           carry; the client's carried by its own hand).
//   hold    slot empty; the client's main hand holds a drive whose data is the seeded signal; the host's puppet for
//           slot 1 shows a drive in the hand mirror; no stray drive is left at the port on either copy; exactly one
//           drive with the signal exists across the client's hand and the world (the host's world holds none).
//   pocket  slot empty; the client's bag (canvasInventory) holds a prop_drive_C record whose saved data holds the
//           signal; no world drive is left at the port on either copy; neither hand holds it.
//
// Every wait here is on a state one of the peers reaches: the client's world coming up, the drive standing frozen in
// the port with its signal, the far copy leaving it, the hand or bag taking the drive. The grab's carry is the one
// timed thing, because a carry is a movement over time.

#include "harness/autotest/driveslot.h"

#include "harness/autotest.h"  // IsClientRole

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"                // IsSlotWorldReady: the host's half of the shared origin
#include "coop/player/hand_item.h"           // the host's puppet's hand mirror, and the hand axis a census skips
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"        // what the host answered and committed
#include "coop/props/prop_act_request.h"     // the client's request being open, refused
#include "coop/session/join_progress.h"      // the client's join being over, not just announced
#include "coop/session/net_pump.h"           // HasAnnouncedWorldReady: the client's half
#include "harness/session_runtime.h"         // Session()
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/inventory_item.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_physics.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest {


bool DriveSlotArmed() {
    const std::string v = coop::config::ReadEnv("VOTVCOOP_RUN_DRIVESLOT");
    return v == "1" || v == "grab" || v == "hold" || v == "pocket";
}

namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace GT  = ue_wrap::game_thread;
namespace DC  = ue_wrap::drive_chain;
namespace SD  = ue_wrap::signal_dynamic;
namespace P   = ue_wrap::profile;
namespace PAR = coop::prop_act_request;
namespace PAH = coop::prop_act_host;
namespace HI  = coop::hand_item;

enum class Arm { Grab, Hold, Pocket };

Arm ArmFromEnv() {
    const std::string v = coop::config::ReadEnv("VOTVCOOP_RUN_DRIVESLOT");
    return v == "hold" ? Arm::Hold : v == "pocket" ? Arm::Pocket : Arm::Grab;
}

const char* ArmName(Arm a) { return a == Arm::Hold ? "hold" : a == Arm::Pocket ? "pocket" : "grab"; }

constexpr float kCarryAsideCm = 150.f;  // how far the client carries the drive (grab)
constexpr float kOffPortCm    = 75.f;   // the host's copy has left the port by at least this (grab)
constexpr float kFollowCm     = 100.f;  // and then moved this much further as the carry goes on (the carry is 150 aside)
constexpr float kNearPortCm   = 100.f;  // a drive this close to the port that was not here before is a stray
constexpr DWORD kReplayWindowMs = 10000;  // the host's connect replay to a fresh joiner
constexpr wchar_t kDriveLeaf[] = L"prop_drive_C";

// The signal the host seeds into data_0, one per arm so two runs' logs differ and no peer mistakes a save's drive for
// it: the name and id are strings the saved data carries, the size a number the row lane compares.
std::wstring SignalName(Arm a) { return std::wstring(L"DRVSLOT-") + (a == Arm::Hold ? L"HOLD" : a == Arm::Pocket ? L"POCKET" : L"GRAB"); }

SD::Row SeedRow(Arm a) {
    SD::Row row;
    row.name = SignalName(a);
    row.id = SignalName(a) + L"-ID";
    row.object = L"selftest";
    row.signal = L"selftest";
    row.size = 10.f;
    row.decoded = 10.f;
    row.level = 2;
    row.date = 638000000000000000LL;   // non-zero: no receiver stamps Now
    row.frequency = 2;
    row.quality = 3;
    row.hasData = true;
    return row;
}

bool RowIsSignal(const SD::Row& r, Arm a) {
    return r.name == SignalName(a) && r.id == SignalName(a) + L"-ID" && r.size == 10.f;
}

// Run a game-thread closure and block until it stores into `done` (1 ok, 2 fail).
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

// Poll `pred` on the game thread every quarter second until it holds or `ms` pass.
template <class Pred>
bool WaitFor(int ms, Pred pred) {
    for (int t = 0; t < ms; t += 250) {
        if (RunGT([pred](std::atomic<int>& d) { d.store(pred() ? 1 : 2); }) == 1) return true;
        ::Sleep(250);
    }
    return false;
}

// The shared origin both peers wait on, the broom drill's: the client's world coming up, which the
// client announces and the host learns as that slot going world-ready. A run reaches it when it
// reaches it -- a fixed wait here measured nothing when the load ran long and burned the rest of
// the window when it did not.
bool WaitForPeerWorld(const char* who) {
    const bool isClient = IsClientRole();
    UE_LOGI("driveslot: %hs -- waiting for the client's world", who);
    for (int i = 0; i < 1800; ++i) {
        // The client's own join being OVER, not merely announced: its phase returns to Idle when
        // the host's snapshot has been applied and the cover is down. The host has no predicate for
        // the other end of its own replay, so it waits out that window -- it is the one thing here
        // that is not a state either peer publishes. Acting inside it loses: the replay's seed
        // carries the slot as it was BEFORE the drill touched it, and it lands after the drill's
        // own edges and puts the old drive back.
        const bool ready = isClient ? (coop::net_pump::HasAnnouncedWorldReady() &&
                                       coop::join_progress::CurrentPhase() ==
                                           coop::join_progress::Phase::Idle)
                                    : harness::session_runtime::Session().IsSlotWorldReady(1);
        if (ready) {
            if (!isClient) ::Sleep(kReplayWindowMs);
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("driveslot: %hs never saw the client's world -- aborting", who);
    return false;
}

uint32_t EidOf(void* actor) {
    return static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor));
}

float Distance(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Call a Blueprint verb on `obj` with the local player as its `player` argument.
bool CallWithPlayer(void* obj, const wchar_t* verb, void* player) {
    void* fn = R::FindDispatchFunction(R::ClassOf(obj), verb, nullptr);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(obj, f);
}

// A verdict being built: each need that does not hold is named, and the verdict is PASS when none failed.
struct Checks {
    std::string failed;
    bool Need(bool ok, const char* what) {
        if (!ok) {
            if (!failed.empty()) failed += "; ";
            failed += what;
        }
        return ok;
    }
    bool Pass() const { return failed.empty(); }
};

// ---- reads shared by both roles -----------------------------------------------------------------------------------

std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    for (wchar_t ch : w) s += ch < 128 ? static_cast<char>(ch) : '?';
    return s;
}

std::vector<void*> LiveDrives() {
    std::vector<void*> out;
    void* const cls = DC::DriveClass();
    if (!cls) return out;
    ue_wrap::object_index::ForEachInstance(
        cls,
        [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        },
        &out);
    return out;
}

bool IsHandAxis(void* actor, void* const* axis, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (axis[i] == actor) return true;
    return false;
}

struct Census {
    int signalWorld = 0;      // world drives carrying the signal (the hand axis is not the world)
    int newNearPort = 0;      // drives that were not here before the action, within kNearPortCm of the port, any row
    std::string detail;
};

// The world's drives, as a stray would be found: the hand axis (this peer's two hands and every puppet's mirrors) is
// player expression and left out; a drive seated in the slot is the slot's.
Census TakeCensus(Arm arm, void* slot, const std::vector<void*>& pre, const ue_wrap::FVector& port) {
    Census c;
    void* axis[HI::kHandAxisCap];
    const size_t axisN = HI::CollectHandAxisActors(axis, HI::kHandAxisCap);
    void* const seated = slot ? DC::SlotDrive(slot) : nullptr;
    char buf[200];
    for (void* d : LiveDrives()) {
        if (IsHandAxis(d, axis, axisN) || d == seated) continue;
        SD::Row row;
        const bool read = DC::ReadDriveRow(d, row);
        const bool sig = read && RowIsSignal(row, arm);
        ue_wrap::FVector at{};
        const bool atRead = E::TryGetActorLocation(d, at);
        const float dist = atRead ? Distance(at, port) : -1.f;
        bool wasHere = false;
        for (void* p : pre) wasHere = wasHere || p == d;
        if (sig) ++c.signalWorld;
        const bool nearNew = !wasHere && atRead && dist <= kNearPortCm;
        if (nearNew) ++c.newNearPort;
        if (sig || nearNew) {
            std::snprintf(buf, sizeof(buf), " [eid=%u sig=%d wasHere=%d %.0f cm from the port]", EidOf(d), sig ? 1 : 0,
                          wasHere ? 1 : 0, dist);
            c.detail += buf;
        }
    }
    return c;
}

// The hand's drive: the main hand's actor when it is a live drive.
void* MainHandDrive(void* player) {
    E::MainPlayerGrabState g{};
    if (!player || !E::ReadMainPlayerGrabState(player, g) || !g.mainHand || !R::IsLive(g.mainHand)) return nullptr;
    return DC::IsDriveClass(R::ClassOf(g.mainHand)) ? g.mainHand : nullptr;
}

// Does the saved stream hold `text`, as an FString stores it: ANSI when it is plain ASCII, UTF-16 otherwise.
bool StreamHas(const std::vector<uint8_t>& bytes, const std::wstring& text) {
    if (text.empty() || bytes.size() < text.size()) return false;
    std::string ansi;
    ansi = NarrowAscii(text);
    for (size_t i = 0; i + ansi.size() <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, ansi.data(), ansi.size()) == 0) return true;
    const size_t wbytes = text.size() * sizeof(wchar_t);
    for (size_t i = 0; i + wbytes <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, text.data(), wbytes) == 0) return true;
    return false;
}

// How many of `items` are a drive whose saved data holds the signal.
int DriveRecordsWithSignal(const std::vector<ue_wrap::inventory_item::Item>& items, Arm arm) {
    int n = 0;
    for (const auto& it : items)
        if (ue_wrap::inventory_item::ClassLeaf(it) == kDriveLeaf && StreamHas(it.actorData, SignalName(arm))) ++n;
    return n;
}

int DriveRecords(const std::vector<ue_wrap::inventory_item::Item>& items) {
    int n = 0;
    for (const auto& it : items)
        if (ue_wrap::inventory_item::ClassLeaf(it) == kDriveLeaf) ++n;
    return n;
}

struct Subject {
    void* slot = nullptr;
    void* drive = nullptr;
    int32_t driveIdx = 0;
    uint32_t eid = 0;
    ue_wrap::FVector port{};
    std::vector<void*> pre;   // the drives that stood in this world before the action
};

// ---- the host ------------------------------------------------------------------------------------------------------

// The host's copy of the drive as the wait sees it, logged whenever it changes: where it is, what it is attached to,
// whether it simulates and is frozen, and whether the slot still holds it. A drive that breaks or vanishes leaves the
// last state it was seen in.
void TraceDrive(const Subject& sb, std::string& last) {
    char buf[300];
    const bool live = R::IsLiveByIndex(sb.drive, sb.driveIdx);
    const bool empty = DC::SlotDrive(sb.slot) == nullptr;
    if (!live) {
        std::snprintf(buf, sizeof(buf), "slotEmpty=%d live=0", empty ? 1 : 0);
    } else {
        void* parent = nullptr;
        E::GetAttachParentActor(sb.drive, parent);
        ue_wrap::FVector at{};
        const bool read = E::TryGetActorLocation(sb.drive, at);
        std::snprintf(buf, sizeof(buf), "slotEmpty=%d live=1 attachedTo=%ls sim=%d frozen=%d at=(%.0f,%.0f,%.0f)%s",
                      empty ? 1 : 0, parent ? R::ClassNameOf(parent).c_str() : L"none",
                      E::IsActorRootSimulatingPhysics(sb.drive) ? 1 : 0, ue_wrap::prop::IsFrozen(sb.drive) ? 1 : 0,
                      at.X, at.Y, at.Z, read ? "" : " (unread)");
    }
    if (last == buf) return;
    last = buf;
    UE_LOGI("driveslot: HOST trace eid=%u %s", sb.eid, buf);
}

// WaitFor on the host, sampling every 30 ms and tracing the drive, so the moments around the client's press show.
template <class Pred>
bool WaitForTraced(const Subject& sb, int ms, Pred pred) {
    std::string last;
    for (int t = 0; t < ms; t += 17) {
        bool ok = false;
        RunGT([&](std::atomic<int>& d) {
            ok = pred();
            TraceDrive(sb, last);
            d.store(1);
        });
        if (ok) return true;
        ::Sleep(1);
    }
    return false;
}

void RunHost(Arm arm) {
    const char* const an = ArmName(arm);
    if (!WaitForPeerWorld("HOST")) return;
    auto sb = std::make_shared<Subject>();
    if (RunGT([sb, arm](std::atomic<int>& d) {
            if (!DC::EnsureResolved()) { UE_LOGW("driveslot: drive chain unresolved"); d.store(2); return; }
            sb->slot = DC::SlotActor(DC::kRoleDeskPlay);
            if (!sb->slot) { UE_LOGW("driveslot: no desk play slot"); d.store(2); return; }
            // A save that already has a drive in the port would otherwise end the run here, and
            // worse, the client's wait for "a frozen drive in the port" would be answered by THAT
            // drive rather than by the insert under test. So the port is emptied first, the way a
            // player empties it -- the drive's own eject, then the grab's unfreeze -- and the drive
            // is left where it lies.
            void* me = coop::players::Registry::Get().Local();
            if (!me) { UE_LOGW("driveslot: no local player"); d.store(2); return; }
            ue_wrap::FVector at{};   // read before the slot is cleared: a host it fails on changes nothing
            if (!E::TryGetActorLocation(me, at)) {
                UE_LOGW("driveslot: the host's location could not be read, so the drive has nowhere to spawn");
                d.store(2);
                return;
            }
            if (void* sitting = DC::SlotDrive(sb->slot)) {
                CallWithPlayer(sitting, L"playerTryToGrab", me);
                CallWithPlayer(sitting, L"playerGrabbed_pre", me);
                UE_LOGI("driveslot: HOST cleared a drive the save left in the play slot (empty=%d)",
                        DC::SlotDrive(sb->slot) == nullptr ? 1 : 0);
                if (DC::SlotDrive(sb->slot)) {
                    UE_LOGW("driveslot: the play slot is taken and did not clear"); d.store(2); return;
                }
            }
            // Born at the host's feet, away from the port: a drive born in the port's reach is taken
            // by the slot's own overlap on every peer, before the insert under test.
            at.Z += 100.f;
            void* drive = E::BeginDeferredSpawn(DC::DriveClass(), at, ue_wrap::FRotator{});
            if (!drive || !E::FinishDeferredSpawn(drive, at, ue_wrap::FRotator{})) {
                UE_LOGW("driveslot: the drive did not spawn"); d.store(2); return;
            }
            // The signal, written as a game writer does (row, then the drive's own upd), before the drive is enrolled:
            // the row lane sends a drive's row at its enrolment when it is not the class default's.
            if (!DC::WriteDriveRow(drive, SeedRow(arm)) || !DC::CallDriveUpd(drive)) {
                UE_LOGW("driveslot: the seeded row could not be written into the drive"); d.store(2); return;
            }
            sb->drive = drive;
            sb->driveIdx = R::InternalIndexOf(drive);
            d.store(1);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT host %hs FAIL -- could not set up the drive", an);
        return;
    }
    // The fresh drive has to be an ENTITY before it goes in: a slot line names its drive by element
    // id, and one sent for a drive that has not been enrolled yet names nothing the far peer can
    // ever resolve. Enrolment happens on the tracker's own pass, a frame or two behind the spawn.
    if (!WaitFor(15000, [sb] {
            const uint32_t e = EidOf(sb->drive);
            return e != 0u && e != static_cast<uint32_t>(coop::element::kInvalidId);
        })) {
        UE_LOGW("driveslot: VERDICT host %hs FAIL -- the fresh drive never took an element id", an);
        return;
    }
    // Straight into the port: an insert that overtakes its drive's birth broadcast is parked by the
    // receiver (a line held in bind_wait until the drive binds), so no wait is owed here.
    const bool portRead = RunGT([sb](std::atomic<int>& d) {
        DC::CallPutDriveIn(sb->slot, sb->drive);
        sb->eid = EidOf(sb->drive);
        const bool read = E::TryGetActorLocation(sb->drive, sb->port);
        sb->pre = LiveDrives();
        UE_LOGI("driveslot: HOST inserted eid=%u in the play slot at (%.0f,%.0f,%.0f)%s frozen=%d",
                sb->eid, sb->port.X, sb->port.Y, sb->port.Z, read ? "" : " (unread)",
                ue_wrap::prop::IsFrozen(sb->drive) ? 1 : 0);
        d.store(read ? 1 : 2);
    }) == 1;
    if (!portRead) {
        UE_LOGW("driveslot: VERDICT host %hs FAIL -- the port's location could not be read, so no carry "
                "off it can be measured", an);
        return;
    }

    // The act lane's counters at the start, for the line that says what the host answered.
    const uint32_t grants0 = PAH::AnsweredFor(1, coop::net::PropActReason::Granted);
    const uint32_t destroys0 = PAH::CommitsOf(1, PAH::internal::CommitStep::Destroy);
    uint32_t tally0[static_cast<int>(PAH::Tally::kCount)] = {};
    for (int t = 0; t < static_cast<int>(PAH::Tally::kCount); ++t) tally0[t] = PAH::TallyOf(static_cast<PAH::Tally>(t));

    const bool out = WaitForTraced(*sb, 90000, [sb] { return DC::SlotDrive(sb->slot) == nullptr; });
    if (!out) {
        UE_LOGW("driveslot: VERDICT host %hs FAIL -- the client's eject never reached the host (slot still holds the drive)", an);
        return;
    }
    UE_LOGI("driveslot: HOST's slot is empty -- waiting for the %hs to land", an);

    auto followed = std::make_shared<float>(0.f);   // grab: the furthest the copy moved from where the first pose put it
    if (arm == Arm::Grab) {
        // The evidence, not a clock: the copy here is unfrozen and has left the port under the client's poses, and
        // then goes on following the carry (it moves a further kFollowCm from where the first pose put it) with the
        // drive still standing -- a copy that breaks on its way is the failure this wait is for. It ends the moment
        // both hold, and the readout below reports whatever is true when the wait returns, pass or timeout.
        auto first = std::make_shared<ue_wrap::FVector>();
        auto haveFirst = std::make_shared<bool>(false);
        WaitForTraced(*sb, 40000, [sb, first, haveFirst, followed] {
            if (!R::IsLiveByIndex(sb->drive, sb->driveIdx)) return true;   // gone: the readout says so
            if (ue_wrap::prop::IsFrozen(sb->drive)) return false;
            ue_wrap::FVector at{};
            if (!E::TryGetActorLocation(sb->drive, at)) return false;
            if (!*haveFirst) {
                if (Distance(at, sb->port) < kOffPortCm) return false;
                *first = at;
                *haveFirst = true;
                return false;
            }
            *followed = std::fmax(*followed, Distance(at, *first));
            return *followed >= kFollowCm;
        });
    } else {
        // This peer's copy destroyed by the grant's commit, and for a hold the client's hand shown here.
        WaitForTraced(*sb, 30000, [sb, arm] {
            if (R::IsLiveByIndex(sb->drive, sb->driveIdx)) return false;
            return arm != Arm::Hold || (HI::HeldClassIs(1, kDriveLeaf) && HI::MirrorActorForSlot(1) != nullptr);
        });
        ::Sleep(3000);   // a stray, or a second copy, would show by now
    }

    RunGT([sb, arm, an, grants0, destroys0, tally0, followed](std::atomic<int>& d) {
        Checks ck;
        const bool live = R::IsLiveByIndex(sb->drive, sb->driveIdx);
        const bool empty = DC::SlotDrive(sb->slot) == nullptr;
        ck.Need(empty, "this slot still holds a drive");
        const uint32_t grants = PAH::AnsweredFor(1, coop::net::PropActReason::Granted) - grants0;
        const uint32_t destroys = PAH::CommitsOf(1, PAH::internal::CommitStep::Destroy) - destroys0;
        std::string tallies;
        for (int t = 0; t < static_cast<int>(PAH::Tally::kCount); ++t) {
            const uint32_t n = PAH::TallyOf(static_cast<PAH::Tally>(t)) - tally0[t];
            if (n) tallies += std::string(" ") + PAH::TallyName(static_cast<PAH::Tally>(t)) + "=" + std::to_string(n);
        }
        char tail[360];
        std::snprintf(tail, sizeof(tail), "act lane: grants=%u destroy-commits=%u tallies:%s", grants, destroys,
                      tallies.empty() ? " none" : tallies.c_str());
        if (arm == Arm::Grab) {
            const bool frozen = live && ue_wrap::prop::IsFrozen(sb->drive);
            ue_wrap::FVector at{};
            const bool atRead = live && E::TryGetActorLocation(sb->drive, at);
            const float moved = atRead ? Distance(at, sb->port) : 0.f;
            ck.Need(live, "the drive was destroyed (a grab keeps the actor)");
            ck.Need(!frozen, "the drive is still frozen");
            ck.Need(atRead && moved >= kOffPortCm, "the drive did not follow the carry off the port");
            ck.Need(*followed >= kFollowCm, "the drive did not keep following the carry");
            UE_LOGI("driveslot: VERDICT host grab %s -- eid=%u live=%d slotEmpty=%d frozen=%d at (%.0f,%.0f,%.0f)%s, "
                    "%.0f cm off the port (needs %.0f), followed the carry %.0f cm (needs %.0f)%s%s; %s",
                    ck.Pass() ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, frozen ? 1 : 0, at.X, at.Y,
                    at.Z, (live && !atRead) ? " (unread)" : "", moved, kOffPortCm, static_cast<double>(*followed),
                    kFollowCm, ck.Pass() ? "" : " FAILED: ",
                    ck.failed.c_str(), tail);
            d.store(1);
            return;
        }
        const Census cen = TakeCensus(arm, sb->slot, sb->pre, sb->port);
        ck.Need(!live, "the host's copy of the drive is still standing");
        ck.Need(cen.signalWorld == 0, "a world drive with the signal is left on the host");
        ck.Need(cen.newNearPort == 0, "a drive that was not here before stands at the port");
        void* const me = coop::players::Registry::Get().Local();
        ck.Need(!MainHandDrive(me), "the host's own hand holds a drive");
        const bool puppetDrive = HI::HeldClassIs(1, kDriveLeaf);
        void* const mirror = HI::MirrorActorForSlot(1);
        const bool mirrorDrive = mirror && R::IsLive(mirror) && DC::IsDriveClass(R::ClassOf(mirror));
        if (arm == Arm::Hold) {
            ck.Need(puppetDrive, "slot 1's hand state is not a drive");
            ck.Need(mirrorDrive, "the host's puppet for slot 1 shows no drive mirror");
        } else {
            ck.Need(!puppetDrive, "slot 1's hand shows a drive after a pocket");
        }
        UE_LOGI("driveslot: VERDICT host %hs %s -- eid=%u copyLive=%d slotEmpty=%d worldSignal=%d newNearPort=%d%s "
                "puppetHand=%d mirror=%d hostHandDrive=%d%s%s; %s",
                an, ck.Pass() ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, cen.signalWorld,
                cen.newNearPort, cen.detail.c_str(), puppetDrive ? 1 : 0, mirrorDrive ? 1 : 0,
                MainHandDrive(me) ? 1 : 0, ck.Pass() ? "" : " FAILED: ", ck.failed.c_str(), tail);
        d.store(1);
    });
}

// ---- the client ----------------------------------------------------------------------------------------------------

// Where the carry goes. The host's copy follows the client's poses as a body that is moved and cannot be pushed back,
// so whatever lies in its way takes an impact the game reads as damage: a drive has little health, and the desk's
// surroundings on the rig save hold a pile of loose drives, one of which broke the carried drive on its first pose
// (impulse 14000 against 20 for a drive at rest). The hand and the far end of the carry are therefore picked, among
// eight directions round the player, as far from every other drive as can be found.
void ChooseCarry(const ue_wrap::FVector& player, const ue_wrap::FVector& fwd, void* subject, ue_wrap::FVector& hand,
                 ue_wrap::FVector& side) {
    std::vector<ue_wrap::FVector> others;
    for (void* d : LiveDrives()) {
        ue_wrap::FVector at{};
        if (d != subject && E::TryGetActorLocation(d, at)) others.push_back(at);
    }
    float best = -1.f;
    for (int k = 0; k < 8; ++k) {
        const float a = static_cast<float>(k) * 0.7853982f;
        const float c = std::cos(a), s = std::sin(a);
        const ue_wrap::FVector dir{fwd.X * c - fwd.Y * s, fwd.X * s + fwd.Y * c, 0.f};
        const ue_wrap::FVector h{player.X + dir.X * 60.f, player.Y + dir.Y * 60.f, player.Z + 80.f};
        const ue_wrap::FVector sd{-dir.Y, dir.X, 0.f};
        float clear = 1e9f;
        for (const auto& o : others)
            for (int step = 0; step <= 3; ++step) {
                const float t = kCarryAsideCm * static_cast<float>(step) / 3.f;
                const ue_wrap::FVector at{h.X + sd.X * t, h.Y + sd.Y * t, h.Z};
                clear = std::fmin(clear, Distance(at, o));
            }
        if (clear > best) {
            best = clear;
            hand = h;
            side = sd;
        }
    }
    UE_LOGI("driveslot: CLIENT carry placed %.0f cm from the nearest other drive (%zu others in the world)", best,
            others.size());
}

void RunClientGrab(Arm arm, const std::shared_ptr<Subject>& sb) {
    struct Grab {
        void* player = nullptr;
        void* handle = nullptr;
        void* mesh = nullptr;
        void* setTargetFn = nullptr;
        void* releaseFn = nullptr;
        ue_wrap::FVector hand{}, side{};
    };
    auto g = std::make_shared<Grab>();
    if (RunGT([sb, g](std::atomic<int>& d) {
            g->player = coop::players::Registry::Get().Local();
            if (!g->player) { UE_LOGW("driveslot: no local player"); d.store(2); return; }
            // Where the hand goes, read before the grab starts: a take that cannot place the drive leaves it in the
            // slot.
            ue_wrap::FVector p{};
            if (!E::TryGetActorLocation(g->player, p)) {
                UE_LOGW("driveslot: the player's location could not be read"); d.store(2); return; }
            // The game's grab of a slotted drive: the drive ejects itself, then the grab unfreezes it.
            const bool took = CallWithPlayer(sb->drive, L"playerTryToGrab", g->player);
            const bool pre = CallWithPlayer(sb->drive, L"playerGrabbed_pre", g->player);
            g->handle = E::ReadMainPlayerGrabHandle(g->player);
            g->mesh = ue_wrap::prop::GetStaticMesh(sb->drive);
            void* phc = R::FindClass(P::name::PhysicsHandleComponentClass);
            void* grabFn = phc ? R::FindFunction(phc, P::name::GrabComponentAtLocationFn) : nullptr;
            g->setTargetFn = phc ? R::FindFunction(phc, P::name::SetTargetLocationFn) : nullptr;
            g->releaseFn = phc ? R::FindFunction(phc, P::name::ReleaseComponentFn) : nullptr;
            if (!g->handle || !g->mesh || !grabFn || !g->setTargetFn || !g->releaseFn) {
                UE_LOGW("driveslot: the physics handle did not resolve"); d.store(2); return;
            }
            const ue_wrap::FVector fwd = E::GetActorForwardVector(g->player);
            ChooseCarry(p, fwd, sb->drive, g->hand, g->side);
            E::SetActorLocation(sb->drive, g->hand);
            ue_wrap::ParamFrame f(grabFn);
            f.Set<void*>(L"Component", g->mesh);
            f.Set<ue_wrap::FVector>(L"GrabLocation", g->hand);
            const bool grabbed = f.valid() && ue_wrap::Call(g->handle, f);
            E::WriteMainPlayerGrabbingPair(g->player, sb->drive, g->mesh);
            UE_LOGI("driveslot: CLIENT took eid=%u out (playerTryToGrab=%d playerGrabbed_pre=%d "
                    "grab=%d) slotEmpty=%d frozen=%d", sb->eid, took ? 1 : 0, pre ? 1 : 0,
                    grabbed ? 1 : 0, DC::SlotDrive(sb->slot) == nullptr ? 1 : 0,
                    ue_wrap::prop::IsFrozen(sb->drive) ? 1 : 0);
            d.store(1);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT client grab FAIL -- could not take the drive");
        return;
    }

    // Carry it aside over three seconds, then let go.
    for (int i = 1; i <= 12; ++i) {
        ::Sleep(250);
        const float k = kCarryAsideCm * static_cast<float>(i) / 12.f;
        RunGT([g, k](std::atomic<int>& d) {
            ue_wrap::ParamFrame f(g->setTargetFn);
            f.Set<ue_wrap::FVector>(L"NewLocation", ue_wrap::FVector{g->hand.X + g->side.X * k,
                                                                      g->hand.Y + g->side.Y * k,
                                                                      g->hand.Z});
            if (f.valid()) ue_wrap::Call(g->handle, f);
            d.store(1);
        });
    }
    RunGT([g](std::atomic<int>& d) {
        ue_wrap::ParamFrame f(g->releaseFn);
        if (f.valid()) ue_wrap::Call(g->handle, f);
        E::WriteMainPlayerGrabbingPair(g->player, nullptr, nullptr);
        d.store(1);
    });
    // Let it come to rest, measured rather than waited out: two samples a quarter second apart
    // within a centimetre of each other is the drop settling. The readout follows either way.
    ue_wrap::FVector last{};
    WaitFor(15000, [sb, &last] {
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(sb->drive, at)) return false;   // unread: not a settled sample
        const bool still = Distance(at, last) < 1.f;
        last = at;
        return still;
    });
    RunGT([sb, arm](std::atomic<int>& d) {
        Checks ck;
        const bool live = R::IsLiveByIndex(sb->drive, sb->driveIdx);
        const bool empty = DC::SlotDrive(sb->slot) == nullptr;
        const bool frozen = live && ue_wrap::prop::IsFrozen(sb->drive);
        ue_wrap::FVector at{};
        const bool atRead = live && E::TryGetActorLocation(sb->drive, at);
        const float moved = atRead ? Distance(at, sb->port) : -1.f;
        ck.Need(empty, "this slot still holds a drive");
        ck.Need(live, "the drive was destroyed (a grab keeps the actor)");
        ck.Need(!frozen, "the drive is still frozen");
        ck.Need(atRead && moved >= kOffPortCm, "the drive is not off the port");
        UE_LOGI("driveslot: VERDICT client %hs %s -- eid=%u live=%d slotEmpty=%d frozen=%d at (%.0f,%.0f,%.0f)%s, "
                "%.0f cm off the port (needs %.0f)%s%s",
                ArmName(arm), ck.Pass() ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, frozen ? 1 : 0,
                at.X, at.Y, at.Z, (live && !atRead) ? " (unread)" : "", moved, kOffPortCm,
                ck.Pass() ? "" : " FAILED: ", ck.failed.c_str());
        d.store(1);
    });
}

// The hold and the pocket: the player's own press verb on the drive, whose hotbar / inventory call the act watch asks
// the host about. The client's hand or bag then carries the drive, respawned or recorded from the slot's data.
void RunClientTake(Arm arm, const std::shared_ptr<Subject>& sb) {
    const char* const an = ArmName(arm);
    struct Base { uint32_t refused = 0; bool opened = false; bool inUse = false; };
    auto base = std::make_shared<Base>();
    auto refusedTotal = [] {
        uint32_t n = 0;
        for (uint8_t r = 1; r < coop::net::kPropActReasons; ++r) n += PAR::dev::RefusedCount(static_cast<coop::net::PropActReason>(r));
        return n;
    };
    if (RunGT([sb, arm, base, refusedTotal](std::atomic<int>& d) {
            void* const player = coop::players::Registry::Get().Local();
            if (!player) { d.store(2); return; }
            sb->pre = LiveDrives();
            base->refused = refusedTotal();
            // The press: the drive's own verb. Its driveAction ejects the drive from the slot here, lifts it and
            // gives it physics, then calls the player's Add Object to Free Hotbar Slot / addObjectToInventory, which
            // the act watch cancels in order to ask the host.
            const bool ran = CallWithPlayer(sb->drive, arm == Arm::Hold ? L"playerTryToHold" : L"playerTryToCollect", player);
            base->opened = PAR::dev::PendingId() != 0;
            UE_LOGI("driveslot: CLIENT pressed %hs on eid=%u (ran=%d) -- slotEmpty=%d requestOpen=%d",
                    arm == Arm::Hold ? "playerTryToHold" : "playerTryToCollect", sb->eid, ran ? 1 : 0,
                    DC::SlotDrive(sb->slot) == nullptr ? 1 : 0, base->opened ? 1 : 0);
            d.store(ran ? 1 : 2);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT client %hs FAIL -- the drive's press verb could not run", an);
        return;
    }
    // The host's answer and the replay: no request pending is the lane's own end, then the verb's result lands.
    WaitFor(20000, [] { return PAR::dev::PendingId() == 0; });
    WaitFor(20000, [sb, arm] {
        void* const player = coop::players::Registry::Get().Local();
        if (arm == Arm::Hold) {
            void* const hand = MainHandDrive(player);
            SD::Row row;
            return hand && DC::ReadDriveRow(hand, row) && RowIsSignal(row, arm);
        }
        std::vector<ue_wrap::inventory_item::Item> bag;
        return ue_wrap::inventory::ReadLiveItems(nullptr, &bag) && DriveRecordsWithSignal(bag, arm) > 0;
    });
    ::Sleep(3000);   // a second copy, a late respawn or a stray would show by now

    RunGT([sb, arm, an, base, refusedTotal](std::atomic<int>& d) {
        Checks ck;
        void* const player = coop::players::Registry::Get().Local();
        const bool empty = DC::SlotDrive(sb->slot) == nullptr;
        const bool live = R::IsLiveByIndex(sb->drive, sb->driveIdx);
        const Census cen = TakeCensus(arm, sb->slot, sb->pre, sb->port);
        const uint32_t refused = refusedTotal() - base->refused;
        ck.Need(base->opened, "the press opened no request (the watch did not ask the host)");
        ck.Need(refused == 0, "the host refused the request");
        ck.Need(empty, "this slot still holds a drive");
        ck.Need(cen.signalWorld == 0, "a world drive with the signal is left on the client");
        ck.Need(cen.newNearPort == 0, "a drive that was not here before stands at the port");
        char what[260];
        what[0] = '\0';
        if (arm == Arm::Hold) {
            void* const hand = MainHandDrive(player);
            SD::Row row;
            const bool handRead = hand && DC::ReadDriveRow(hand, row);
            const bool handSig = handRead && RowIsSignal(row, arm);
            std::vector<ue_wrap::inventory_item::Item> hotbar, bag;
            ue_wrap::inventory::ReadLiveItems(&hotbar, &bag);
            const std::wstring held = ue_wrap::inventory::ReadHeldItemName();
            ck.Need(hand != nullptr, "the main hand holds no drive");
            ck.Need(handSig, "the hand's drive does not carry the seeded signal");
            ck.Need((handSig ? 1 : 0) + cen.signalWorld == 1, "the drive with the signal is not exactly one across hand and world");
            ck.Need(DriveRecordsWithSignal(hotbar, arm) == 1, "the hotbar holds no record of the drive with its data");
            ck.Need(DriveRecords(bag) == 0 || DriveRecordsWithSignal(bag, arm) == 0, "the bag holds a second copy of the drive");
            std::snprintf(what, sizeof(what), "hand=%p row='%ls' size=%.0f holdingObjectName='%ls' hotbarRecords=%d bagCopies=%d",
                          hand, handRead ? row.name.c_str() : L"<unread>", handRead ? row.size : -1.f, held.c_str(),
                          DriveRecordsWithSignal(hotbar, arm), DriveRecordsWithSignal(bag, arm));
        } else {
            std::vector<ue_wrap::inventory_item::Item> hotbar, bag;
            const bool read = ue_wrap::inventory::ReadLiveItems(&hotbar, &bag);
            const int withSig = DriveRecordsWithSignal(bag, arm);
            ck.Need(read, "the inventory could not be read");
            ck.Need(withSig == 1, "the bag does not hold exactly one drive record carrying the signal");
            ck.Need(DriveRecordsWithSignal(hotbar, arm) == 0, "the hotbar holds a copy of the pocketed drive");
            ck.Need(!MainHandDrive(player), "the main hand holds a drive after a pocket");
            std::snprintf(what, sizeof(what), "bagDriveRecords=%d bagWithSignal=%d hotbarWithSignal=%d bagSize=%zu",
                          DriveRecords(bag), withSig, DriveRecordsWithSignal(hotbar, arm), bag.size());
        }
        UE_LOGI("driveslot: VERDICT client %hs %s -- eid=%u mirrorLive=%d slotEmpty=%d requestOpened=%d refused=%u "
                "worldSignal=%d newNearPort=%d%s %s%s%s",
                an, ck.Pass() ? "PASS" : "FAIL", sb->eid, live ? 1 : 0, empty ? 1 : 0, base->opened ? 1 : 0, refused,
                cen.signalWorld, cen.newNearPort, cen.detail.c_str(), what, ck.Pass() ? "" : " FAILED: ",
                ck.failed.c_str());
        d.store(1);
    });
}

void RunClient(Arm arm) {
    const char* const an = ArmName(arm);
    if (!WaitForPeerWorld("CLIENT")) return;
    // A drive carrying the seeded signal, frozen in the play slot. The signal and not "a different drive", because
    // the host empties the port and inserts a fresh drive, and the lane carries idempotent slot STATE rather than
    // edges: the two moves land here as one line naming the new drive, so a peer watching for the port to go empty
    // would wait forever. Frozen, because the slot freezes what it takes, and without that the take below would have
    // nothing to undo.
    auto sb = std::make_shared<Subject>();
    UE_LOGI("driveslot: CLIENT -- waiting for the seeded drive '%ls' frozen in the play slot", SignalName(arm).c_str());
    const bool in = WaitFor(180000, [sb, arm] {
        if (!DC::EnsureResolved()) return false;
        if (!sb->slot) sb->slot = DC::SlotActor(DC::kRoleDeskPlay);
        sb->drive = sb->slot ? DC::SlotDrive(sb->slot) : nullptr;
        if (!sb->drive || !ue_wrap::prop::IsFrozen(sb->drive)) return false;
        SD::Row row;
        return DC::ReadDriveRow(sb->drive, row) && RowIsSignal(row, arm);
    });
    if (!in) {
        std::string why = "no drive reached the play slot";
        RunGT([sb, &why](std::atomic<int>& d) {
            void* const dr = sb->slot ? DC::SlotDrive(sb->slot) : nullptr;
            SD::Row row;
            if (dr) {
                why = DC::ReadDriveRow(dr, row) ? "a drive reached the play slot but its row is not the seeded signal (name '" +
                                                       NarrowAscii(row.name) + "')"
                                                 : "a drive reached the play slot, its row unreadable";
                if (!ue_wrap::prop::IsFrozen(dr)) why += ", not frozen";
            }
            d.store(1);
        });
        UE_LOGW("driveslot: VERDICT client %hs FAIL -- %hs", an, why.c_str());
        return;
    }

    // The player first stands at the port, as a player taking a disc out does. On 0.10 the save's spawn is far from
    // the desk (measured 5.5 km), and a hand carried there in one frame would drag the drive that far in one frame: the
    // host's copy then follows at 1.8e5 cm/s and the game destroys it on impact, which reads as a lost eject. The
    // host's reach test for a hold or a pocket reads this standing too.
    if (RunGT([sb](std::atomic<int>& d) {
            void* const me = coop::players::Registry::Get().Local();
            ue_wrap::FVector port{};
            if (!me || !sb->drive || !E::TryGetActorLocation(sb->drive, port)) { d.store(2); return; }
            sb->port = port;
            sb->eid = EidOf(sb->drive);
            sb->driveIdx = R::InternalIndexOf(sb->drive);
            E::SetActorLocation(me, ue_wrap::FVector{port.X + 150.f, port.Y, port.Z - 60.f});
            d.store(1);
        }) != 1) {
        UE_LOGW("driveslot: VERDICT client %hs FAIL -- could not stand the player at the port", an);
        return;
    }
    ::Sleep(3000);   // the puppet's pose reaches the host before the action
    if (arm == Arm::Grab) {
        RunGT([sb](std::atomic<int>& d) { sb->pre = LiveDrives(); d.store(1); });
        RunClientGrab(arm, sb);
    } else {
        RunClientTake(arm, sb);
    }
}

}  // namespace

void RunDriveSlotTest() {
    const Arm arm = ArmFromEnv();
    UE_LOGI("driveslot: arm=%hs role=%hs", ArmName(arm), IsClientRole() ? "client" : "host");
    if (IsClientRole()) RunClient(arm);
    else RunHost(arm);
    UE_LOGI("driveslot: done");
}

DWORD WINAPI DriveSlotTestThread(LPVOID) {
    RunDriveSlotTest();
    return 0;
}

}  // namespace harness::autotest
