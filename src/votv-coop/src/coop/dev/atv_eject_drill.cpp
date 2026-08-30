// coop/dev/atv_eject_drill.cpp -- the FORCED-EJECT acceptance arm (qf rounds 1/3 of the v147
// condition pass). A diagnostic, RULE-2-exempt (probes/diagnostics exemption).
//
// The one moment #4 exists for -- a mask flip landing on a live simulating mirror -- has no
// organic trigger in an autonomous run (17.12: an eject has NEVER been observed; tires stayed
// 0xF through every archived run). This drill performs a REAL eject through the game's own
// path: getTire(index) -> damageWheel(index, 200, component), which drives ubergraph 15210 --
// durability to 0 -> ejectWheel -> wheel prop + tires[i]=false + updTires, i.e. the native
// transaction, not a hand-poked array.
//
// TWO ARMS, selected by env VOTVCOOP_ATV_EJECT_TEST (propagated by mp.py like the weather
// test's env):
//   "host"   -- fires on the HOST for an ATV it owns the tick for. The canonical direction:
//               a host-authored wheel prop broadcasts legally (b143 field log: 7/7 keys
//               adopted), the mask travels in the next AtvState, the mirror's updTires fires
//               once. Acceptance (b).
//   "client" -- fires on a CLIENT while it is the pose AUTHOR (seated via the real sit verb
//               by the probe's drive arm). The KNOWN-BROKEN direction the presence-authority
//               rule refuses: the host must RETAIN the wheel, count presence-skipped, and its
//               wheel-key census must not grow. Acceptance (b2) ASSERTS the divergence.
//
// Fires ONCE per process, kFireDelayMs after the first ELIGIBLE sighting -- late enough for
// the drive arm to be seated and moving, early enough to leave steady-state tail in a 300 s
// smoke.

#include "coop/dev/atv_eject_drill.h"

#include "coop/config/config.h"
#include "coop/player/roster_ledger.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <string>

namespace coop::atv_eject_drill {

namespace R = ue_wrap::reflection;

namespace {

// PER-ARM delay, and the countdown is over CONTINUOUS eligibility (the anchor RESETS on a
// lapse). The first client run proved why: the drive arm banks its 20 s and DISMOUNTS, so a
// 45 s countdown anchored at first-eligibility fired into a lapsed authority and the drill
// never shot -- the run then idled out its whole --duration with the driver standing next
// to the ATV (audit MINOR-7, noted-not-fixed, and it cost the run). The client arm must fit
// INSIDE the drive window; the host arm keeps a longer settle because its eligibility never
// lapses (nobody drives in its run) and the joiner needs to be world-ready.
constexpr uint64_t kFireDelayHostMs   = 45000;
constexpr uint64_t kFireDelayClientMs = 10000;
constexpr int32_t  kWheelIndex  = 2;      // a REAR wheel: index 2 per the uber's own switch
constexpr float    kDamage      = 200.f;  // > any durability -> FMax(x,0)=0 -> ejectWheel

enum class Arm : uint8_t { Off, Host, Client };

Arm ReadArm() {
    static Arm arm = [] {
        const std::string v = coop::config::ReadEnv("VOTVCOOP_ATV_EJECT_TEST");
        if (v == "host") return Arm::Host;
        if (v == "client") return Arm::Client;
        return Arm::Off;
    }();
    return arm;
}

uint64_t g_eligibleSinceMs = 0;
bool     g_fired = false;

}  // namespace

void MaybeFire(void* actor, const wchar_t* key, uint64_t nowMs, bool isHost, bool isAuthority,
               bool ownsTick) {
    const Arm arm = ReadArm();
    if (arm == Arm::Off || g_fired || !actor) return;
    // A REMOTE PEER must be seated in the roster before the countdown starts: a host-arm
    // eject fired pre-join lands in the joiner's SAVE (the (e) shape) instead of on a LIVE
    // mirror (the (b) shape this drill exists to witness). Run A measured exactly that
    // hazard: the host owns its ATV minutes before the client's world is up.
    if (coop::roster_ledger::OccupiedCount() < 2) return;
    const bool eligible = (arm == Arm::Host) ? (isHost && ownsTick)
                                             : (!isHost && isAuthority);
    if (!eligible) {
        // A lapse resets the countdown: firing into lapsed authority is a no-op that
        // silently eats the whole run (measured 19:49 -- armed at seat, dismount at +20 s,
        // "fire" at +45 s hit nothing, windows idled out the timer).
        if (g_eligibleSinceMs != 0) {
            g_eligibleSinceMs = 0;
            UE_LOGI("[ATV-EJECT-DRILL] eligibility lapsed before fire -- countdown reset");
        }
        return;
    }
    const uint64_t delay = arm == Arm::Host ? kFireDelayHostMs : kFireDelayClientMs;
    if (g_eligibleSinceMs == 0) {
        g_eligibleSinceMs = nowMs;
        UE_LOGI("[ATV-EJECT-DRILL] armed (%hs) on '%ls' -- firing in %llu s of CONTINUOUS "
                "eligibility", arm == Arm::Host ? "host" : "client", key ? key : L"?",
                static_cast<unsigned long long>(delay / 1000));
        return;
    }
    if (nowMs - g_eligibleSinceMs < delay) return;
    g_fired = true;

    void* cls = R::FindClass(L"ATV_C");
    void* fnGetTire = cls ? R::FindFunction(cls, L"getTire") : nullptr;
    void* fnDamage  = cls ? R::FindFunction(cls, L"damageWheel") : nullptr;
    if (!fnGetTire || !fnDamage) {
        UE_LOGE("[ATV-EJECT-DRILL] getTire/damageWheel did not resolve -- drill aborted");
        return;
    }
    ue_wrap::ParamFrame tireFrame(fnGetTire);
    tireFrame.Set(L"index", kWheelIndex);
    if (!ue_wrap::Call(actor, tireFrame)) {
        UE_LOGE("[ATV-EJECT-DRILL] getTire call failed");
        return;
    }
    void* component = tireFrame.Get<void*>(L"ReturnValue");
    if (!component) {
        UE_LOGE("[ATV-EJECT-DRILL] getTire(%d) returned null -- drill aborted", kWheelIndex);
        return;
    }
    ue_wrap::ParamFrame dmgFrame(fnDamage);
    dmgFrame.Set(L"index", kWheelIndex);
    dmgFrame.Set(L"damage", kDamage);
    dmgFrame.Set(L"component", component);
    const bool ok = ue_wrap::Call(actor, dmgFrame);
    UE_LOGI("[ATV-EJECT-DRILL] FIRED damageWheel(%d, %.0f) on '%ls' (%hs arm) -> %hs -- the "
            "native path runs durability->0 -> ejectWheel -> wheel prop + mask flip + updTires",
            kWheelIndex, kDamage, key ? key : L"?",
            ReadArm() == Arm::Host ? "host" : "client", ok ? "dispatched" : "CALL FAILED");
}

}  // namespace coop::atv_eject_drill
