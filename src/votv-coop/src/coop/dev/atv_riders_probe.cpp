// coop/dev/atv_riders_probe.cpp -- the sequencer of the ATV riders probe; see coop/dev/atv_riders_probe.h. The gate rows are
// atv_riders_probe_rows.cpp, the reads atv_riders_probe_acts.cpp; the walk, the drive and the dismount are the ATV gate drill's
// acts (atv_gate_drill_detail.h), the seat is atv_seat's.
//
// The peers order themselves with peer_signal marks, each phase ending on the event the next waits for:
//   host: waits both clients' world -> names the ATV -> a kerfur beside it -> (client walks, sits) -> `rider-sat` (its kerfur's
//         sitOnCar POST) -> samples the ATV's applied blocks until the ATV moved 6 m -> M15 -> waits the getOffCar PRE
//   client (slot 1): walks, sits (M3 at the sit's return), drives on `host/observer_m2_ready`, stops on `drive_done`, leaves
//   observer (slot 2): waits the body and the kerfur seated, samples the applied blocks until `drive_off`
// A bound on any wait is INVALID, said once by the peer that hit it and marked for the others.

#include "coop/dev/atv_riders_probe.h"

#include "atv_gate_drill_detail.h"   // co-located private header: the walk, the drive and the dismount acts
#include "atv_riders_probe_detail.h"
#include "atv_seat.h"                // co-located private header: the game's own seat verb

#include "coop/dev/spawn_npc.h"
#include "coop/element/registry.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "harness/autotest.h"        // IsClientRole
#include "harness/autotest/peer_signal.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace coop::dev::atv_riders_probe {
namespace {

using namespace detail;
namespace PS = harness::autotest::peer_signal;
namespace DG = coop::dev::atv_gate_drill::detail;
namespace DE = coop::atv_drill_events;

constexpr int kJoinS = 480;     // a peer's boot, join and world load
constexpr int kSignalS = 240;   // a step of another peer
constexpr int kWalkS = 420;     // another peer's director walk
constexpr int kKerfurS = 120;   // the host's kerfur walks to the ATV and sits
constexpr int kStepS = 60;      // a seat, a mirror or a watch settling
constexpr int kDriveS = 180;    // the ATV's drive to its 6 m

enum Role : int { kHost, kClient, kObserver };
const char* RoleName(Role r) { return r == kHost ? "host" : r == kClient ? "client" : "observer"; }

std::wstring g_key;
std::atomic<bool> g_saidEnd{false};

void SleepStep() { PS::detail::SleepStep(); }

// The run ended on another peer's INVALID.
bool Ended() {
    std::string t;
    return PS::Peek("probe_invalid", t);
}

bool Invalid(const char* phase, const std::string& why) {
    if (!g_saidEnd.exchange(true)) {
        UE_LOGI("atv-probe: INVALID %s %s", phase, why.c_str());
        if (!Ended()) PS::Mark("probe_invalid", std::string(phase) + " " + why);
    }
    return false;
}

// Polls `pred` to its event; false when the run ended elsewhere or the bound passed (INVALID said).
template <class Pred>
bool Wait(const char* phase, int boundS, Pred pred) {
    for (int i = 0; i < boundS * 10; ++i) {
        if (Ended()) return Invalid(phase, "the run was ended by another peer");
        if (pred()) return true;
        SleepStep();
    }
    return Invalid(phase, "no event within " + std::to_string(boundS) + " s");
}

bool Await(const char* phase, const char* mark, int boundS, std::string* text = nullptr) {
    std::string t;
    const bool got = Wait(phase, boundS, [&] { return PS::Peek(mark, t); });
    if (got && text) *text = t;
    return got;
}

void* Me() { return coop::players::Registry::Get().Local(); }

// The announced world and the host's puppet exist here.
bool WorldUp() {
    return Wait("world", kJoinS, [] {
               bool ready = false;
               DG::OnGt([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
               return ready;
           }) &&
           Wait("world", 60, [] {
               bool have = false;
               DG::OnGt([&] {
                   coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                   have = p && p->valid();
               });
               return have;
           });
}

// This role's gate rows registered and live.
bool RowsReady(bool host) {
    if (!RegisterRows(host)) return Invalid("rows", "the gate refused a registration");
    int state = 0;
    if (!Wait("rows", kStepS, [&] {
            DG::OnGt([&] { state = RowsLive(host); });
            return state != 0;
        }))
        return false;
    return state == 1 || Invalid("rows", "a watch resolved dead");
}

// Both clients' mark of `suffix` ("<role>_<suffix>").
bool AwaitClients(const char* phase, const char* suffix, int boundS) {
    for (const char* r : {"client", "observer"})
        if (!Await(phase, (std::string(r) + "_" + suffix).c_str(), boundS)) return false;
    return true;
}

// ---- the sampling both watchers run while the client drives ----

// Reads the body (and, on the observer, the kerfur) at each batch of applied ATV blocks, until the client lets the throttle go.
// The host ends the drive when its own copy has moved kDriveCm from `start`.
bool WatchDrive(Role me, uint32_t eid, const ue_wrap::FVector& start, uint32_t cur) {
    const int32_t tag = DE::KeyTag(g_key);
    float bodyMax = 0.f, kerfurMax = 0.f, driveCm = 0.f;
    bool marked = false;
    std::string t;
    for (int i = 0; i < kDriveS * 10; ++i) {
        if (Ended()) return Invalid("drive", "the run was ended by another peer");
        bool blocks = false;
        DE::Rec r;
        while (DE::Next(DE::kPoseApplied, cur, r)) blocks = blocks || r.i0 == tag;
        if (blocks) {
            Sample s;
            ue_wrap::FVector root{};
            DG::OnGt([&] {
                s = TakeSample(g_key, me == kObserver ? eid : 0u);
                ReadRoot(g_key, root);
            });
            const float dx = root.X - start.X, dy = root.Y - start.Y;
            driveCm = std::sqrt(dx * dx + dy * dy);
            if (s.bodyCm >= 0.f) {
                bodyMax = (std::max)(bodyMax, s.bodyCm);
                UE_LOGI("atv-probe: M2 body d_seat=%.0f parent_is_atv=%d", s.bodyCm, s.bodyParent);
            }
            if (s.kerfurCm >= 0.f) {
                kerfurMax = (std::max)(kerfurMax, s.kerfurCm);
                UE_LOGI("atv-probe: M2 kerfur d_sitarrow=%.0f parent_is_atv=%d", s.kerfurCm, s.kerfurParent);
            }
            if (me == kHost && !marked && driveCm >= kDriveCm) {
                marked = true;
                PS::Mark("drive_done");
            }
        }
        if (PS::Peek("drive_off", t)) {
            UE_LOGI("atv-probe: M2 summary body_max=%.0f kerfur_max=%.0f drive_cm=%.0f", bodyMax, kerfurMax, driveCm);
            return true;
        }
        SleepStep();
    }
    return Invalid("drive", "the client never let the throttle go within " + std::to_string(kDriveS) + " s");
}

// ---- the three peers ----

bool RunHost() {
    std::string t;
    if (!AwaitClients("join", "ready", kJoinS)) return false;
    g_key = DG::ChooseAtv();
    if (g_key.empty()) return Invalid("atv", "the world holds no ATV a route from the host reaches");
    UE_LOGI("atv-probe: the host's ATV key='%ls'", g_key.c_str());
    PS::Mark("atv_key", std::string(g_key.begin(), g_key.end()));
    if (!RowsReady(true) || !AwaitClients("atv", "has_atv", kSignalS)) return false;

    // A kerfur beside the ATV: a runtime spawn, so it has no save key.
    void* kerfur = nullptr;
    uint32_t eid = coop::element::kInvalidId;
    ue_wrap::FVector at{};
    bool placed = false;
    DG::OnGt([&] {
        placed = ReadRoot(g_key, at);
        if (placed) kerfur = coop::dev::spawn_npc::SpawnAt(L"kerfurOmega_C", {at.X + 200.f, at.Y, at.Z + 60.f});
    });
    if (!placed || !kerfur) return Invalid("kerfur", "the kerfur was not spawned beside the ATV");
    if (!Wait("kerfur", kStepS, [&] {
            DG::OnGt([&] { eid = coop::element::Registry::Get().EidForActor(kerfur); });
            return eid != coop::element::kInvalidId;
        }))
        return false;
    UE_LOGI("atv-probe: the host's kerfur %p is eid %u", kerfur, eid);
    PS::Mark("kerfur_eid", std::to_string(eid));
    if (!AwaitClients("kerfur", "has_kerfur", kSignalS)) return false;

    if (!Await("walk", "client_at_atv", kWalkS) || !Await("sit", "client_seated", kSignalS)) return false;
    if (!Await("M1", "rider-sat", kKerfurS, &t)) return false;
    eid = static_cast<uint32_t>(std::stoul(t));
    if (!Wait("M2", kStepS, [] {
            bool seated = false;
            DG::OnGt([&] { seated = BodySeated(g_key); });
            return seated;
        }))
        return false;
    ue_wrap::FVector start{};
    DG::OnGt([&] { ReadRoot(g_key, start); });
    const uint32_t cur = DE::Count(DE::kPoseApplied);   // the blocks of the drive only
    PS::Mark("host_m2_ready");
    if (!WatchDrive(kHost, 0, start, cur)) return false;

    // M15, the body still seated and attached; then the client leaves and the kerfur's getOffCar is seen.
    DG::OnGt([] { RunSkinProbe(); });
    const uint32_t offs = GetOffSeen();
    PS::Mark("m15_done");
    if (!Wait("M1", kSignalS, [&] { return GetOffSeen() > offs; })) return false;
    return Await("leave", "left", kSignalS);
}

bool RunClient(Role me) {
    std::string t;
    if (!WorldUp()) return false;
    PS::Mark(me == kClient ? "client_ready" : "observer_ready");
    if (!Await("atv", "atv_key", kSignalS, &t)) return false;
    g_key.assign(t.begin(), t.end());
    if (!Wait("atv", 120, [] {
            bool has = false;
            DG::OnGt([&] { has = AtvOf(g_key) != nullptr; });
            return has;
        }))
        return false;
    if (me == kClient && !RowsReady(false)) return false;
    PS::Mark(me == kClient ? "client_has_atv" : "observer_has_atv");

    if (!Await("kerfur", "kerfur_eid", kSignalS, &t)) return false;
    const uint32_t eid = static_cast<uint32_t>(std::stoul(t));
    if (!Wait("kerfur", kStepS, [&] {
            void* k = nullptr;
            DG::OnGt([&] { k = KerfurMirrorOf(eid); });
            return k != nullptr;
        }))
        return false;
    PS::Mark(me == kClient ? "client_has_kerfur" : "observer_has_kerfur");

    if (me == kObserver) {
        if (!Await("M1", "rider-sat", kKerfurS)) return false;
        if (!Wait("M2", kStepS, [&] {
                bool ready = false;
                DG::OnGt([&] { ready = BodySeated(g_key) && KerfurRides(g_key, eid); });
                return ready;
            }))
            return false;
        ue_wrap::FVector start{};
        DG::OnGt([&] { ReadRoot(g_key, start); });
        const uint32_t cur = DE::Count(DE::kPoseApplied);
        PS::Mark("observer_m2_ready");
        if (!WatchDrive(kObserver, eid, start, cur)) return false;
        return Await("leave", "left", kSignalS);
    }

    std::string why;
    if (!DG::WalkToAtv(g_key, why)) return Invalid("walk", why);
    PS::Mark("client_at_atv");

    // The seat, and M3 in the call the sit finishes in: before any host stamp after the grant can seat a mirror.
    coop::dev::atv_seat::SitTry sit;
    bool seated = false;
    const bool finished = Wait("sit", kStepS, [&] {
        bool done = false;
        DG::OnGt([&] {
            void* atv = AtvOf(g_key);
            void* player = Me();
            done = atv && player && coop::dev::atv_seat::TrySit(player, atv, sit);
            coop::atv_sync::TableView tv;
            if (done && coop::atv_sync::ReadTable(g_key, tv) && tv.localSeated) {
                seated = true;
                LogMirrors(g_key);
            }
        });
        return done;
    });
    if (!finished) return false;
    if (!seated) return Invalid("sit", "the seat verb did not seat the player (see the [ATVP] lines)");
    PS::Mark("client_seated");

    if (!Await("M2", "host_m2_ready", kKerfurS + kSignalS) || !Await("M2", "observer_m2_ready", kSignalS)) return false;
    if (!DG::DriveOn(g_key, why)) return Invalid("drive", why);
    const bool stopped = Await("drive", "drive_done", kDriveS);
    DG::DriveOff(g_key);
    PS::Mark("drive_off");
    if (!stopped) return false;

    if (!Await("M15", "m15_done", kSignalS)) return false;
    if (!DG::Leave(g_key, why)) return Invalid("leave", why);
    PS::Mark("left");
    return true;
}

}  // namespace

DWORD WINAPI Thread(LPVOID /*arg*/) {
    // The probe is the host's, slot 1's (the driver) and slot 2's (the observer). A client learns its slot at its join.
    Role role = kHost;
    if (harness::autotest::IsClientRole()) {
        uint8_t slot = coop::players::kPeerIdUnknown;
        for (int i = 0; i < 2400; ++i) {
            slot = coop::players::Registry::Get().LocalPeerId();
            if (slot != coop::players::kPeerIdUnknown) break;
            ::Sleep(100);
        }
        if (slot != 1 && slot != 2) {
            UE_LOGI("atv-probe: this client is slot %d: it takes no part", static_cast<int>(slot));
            return 0;
        }
        role = slot == 1 ? kClient : kObserver;
    }
    if (!PS::Begin(kTag)) return 0;
    UE_LOGI("atv-probe: the %s starts", RoleName(role));
    const bool ok = role == kHost ? RunHost() : RunClient(role);
    if (ok) UE_LOGI("atv-probe: DONE");
    return 0;
}

}  // namespace coop::dev::atv_riders_probe
