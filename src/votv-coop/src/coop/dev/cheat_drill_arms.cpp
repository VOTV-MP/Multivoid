// coop/dev/cheat_drill_arms.cpp -- the cheat drill's hostile arms G..L (see coop/dev/cheat_drill.h): raw requests no panel
// makes, the request bucket, the removal clamp, two requests in flight, a withheld right, Unstuck. The host's half stages
// and judges each arm from its own counts; the client's half sends the requests and judges each answer.

#include "coop/cheats/cheat_request.h"
#include "coop/cheats/self_teleport.h"
#include "coop/commands/command_sync.h"
#include "coop/dev/cheat_drill_raw.h"
#include "cheat_drill_shared.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/permissions/grants_core.h"
#include "coop/permissions/permission_host.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <string>

namespace coop::dev::cheat_drill::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace CH = coop::cheats;
using coop::net::CheatReason;

// ---- the hostile arms, G..L: the host's half ---------------------------------------------------------------------
//
// G  raw requests no panel makes (a bad op, a name with no end, counts and amounts past the wire's bounds, a target nobody
//    holds, an unsafe event, an explosive, an unknown event): each answered with its own reason, the world unmoved.
// H  a request for ten props drains the bucket (ten tokens), and the next ten-prop request is answered Rate.
// I  eight removals of 100000 against a balance of 50000 in one burst: one is Ok, seven Failed, the balance 0, never below.
// J  two requests in flight: the status line stays with the later one while the earlier one's answer arrives.
// K  the host withholds the right from the client: its request is answered Off and its projected right is clear.
// L  the client's body is lifted off the mesh through the game's teleport, and its Unstuck puts it back on the
//    NavMesh, at least 100 cm below where it was lifted to.

constexpr const char* kArmTag[] = {"G", "H", "I", "J", "K", "L"};
constexpr int kArms = 6;
constexpr int32_t kStake = 50000;
constexpr float kLiftCm = 150.f;      // arm L: how far the body is lifted; within the reach of Unstuck's NavMesh query
constexpr float kMinDropCm = 100.f;   // arm L: how far below the lifted body the landing must be

uint32_t Answered(CheatReason r) { return CH::AnsweredFor(1, r); }
uint32_t Delta(int i, CheatReason r) { return Answered(r) - g.answered[i]; }

const coop::permissions::grants::Entry& RightEntry() {
    return coop::permissions::grants::kProjected[static_cast<size_t>(coop::permissions::grants::Projected::Cheats)];
}
std::string ClientGuid(coop::net::Session& s) {
    return s.ProvedGuidForSlotWithToken(1, coop::roster_ledger::Get(1).bornGeneration);
}
bool ClientHoldsRight(coop::net::Session& s) {
    return coop::permissions::host::Allows(ClientGuid(s), RightEntry().node, RightEntry().defaultGranted, false);
}

// Stage arm `arm`: its baselines and, where it needs one, its setup. True when the client may be told to go.
bool ArmPrep(int arm, coop::net::Session& s) {
    int32_t p = 0;
    switch (arm) {
    case 0:
        Points(&g.points);
        g.crowbars = Crowbars();
        g.answered[0] = Answered(CheatReason::BadRequest);
        g.answered[1] = Answered(CheatReason::NoTarget);
        g.answered[2] = Answered(CheatReason::Refused);
        g.answered[3] = Answered(CheatReason::UnknownEvent);
        return true;
    case 1:
        g.crowbars = Crowbars();
        g.answered[0] = Answered(CheatReason::Rate);
        return true;
    case 2:   // the balance to 0 (a removal stops there), then to the stake
        if (g.stage == 0) { CH::AddPoints(-2000000000); g.stage = 1; g.stepAt = Now(); return false; }
        if (g.stage == 1) {
            if (Points(&p) && p == 0) { CH::AddPoints(kStake); g.stage = 2; g.stepAt = Now(); }
            else if (Timed(kEffectBoundMs)) Invalid("host", "I: the host's balance did not reach 0");
            return false;
        }
        if (Points(&p) && p == kStake) {
            g.answered[0] = Answered(CheatReason::Ok);
            g.answered[1] = Answered(CheatReason::Failed);
            return true;
        }
        if (Timed(kEffectBoundMs)) Invalid("host", "I: the host's balance did not reach the stake");
        return false;
    case 3:
        g.answered[0] = Answered(CheatReason::UnknownItem);
        g.answered[1] = Answered(CheatReason::Ok);
        return true;
    case 4:   // withhold the node from the client through the host's own permission command
        if (g.stage == 0) {
            const std::string guid = ClientGuid(s);
            if (guid.empty()) { Invalid("host", "K: the client's id is not proved"); return false; }
            coop::command_sync::Submit("mv user " + guid + " permission set " + RightEntry().node + " false");
            g.stage = 1;
            g.stepAt = Now();
            return false;
        }
        if (!ClientHoldsRight(s)) {
            Points(&g.points);
            g.answered[0] = Answered(CheatReason::Off);
            return true;
        }
        if (Timed(15000)) Invalid("host", "K: the permission edit did not withhold the right");
        return false;
    default: return true;
    }
}

// Judge arm `arm` once the client says it sent. True when passed; a FAIL ends the run, a bound says INVALID.
bool ArmJudge(int arm, coop::net::Session& s) {
    int32_t p = 0;
    Points(&p);
    switch (arm) {
    case 0:
        if (Delta(0, CheatReason::BadRequest) != 9 || Delta(1, CheatReason::NoTarget) != 1 ||
            Delta(2, CheatReason::Refused) != 2 || Delta(3, CheatReason::UnknownEvent) != 1 || p != g.points ||
            Crowbars() != g.crowbars) {
            Verdict("host", false, "G: the host's answers or world are not the hostile requests': BadRequest " +
                                       std::to_string(Delta(0, CheatReason::BadRequest)) + " (9), NoTarget " +
                                       std::to_string(Delta(1, CheatReason::NoTarget)) + " (1), Refused " +
                                       std::to_string(Delta(2, CheatReason::Refused)) + " (2), UnknownEvent " +
                                       std::to_string(Delta(3, CheatReason::UnknownEvent)) + " (1)");
            return false;
        }
        UE_LOGI("%s host G: every hostile request was refused with its reason and nothing moved", kTag);
        return true;
    case 1: {
        const int c = Crowbars();
        if (c > g.crowbars + coop::net::kCheatMaxCount) { Verdict("host", false, "H: a second ten-prop request ran: " + std::to_string(c - g.crowbars) + " props"); return false; }
        if (Delta(0, CheatReason::Rate) != 1) { Verdict("host", false, "H: the host answered Rate " + std::to_string(Delta(0, CheatReason::Rate)) + " times, not once"); return false; }
        if (c < g.crowbars + coop::net::kCheatMaxCount) { if (Timed(kEffectBoundMs)) Invalid("host", "H: the first ten props did not appear"); return false; }
        UE_LOGI("%s host H: ten props ran, the next ten-prop request was answered Rate", kTag);
        return true;
    }
    case 2:
        if (p != 0 || Delta(0, CheatReason::Ok) != 1 || Delta(1, CheatReason::Failed) != 7) {
            Verdict("host", false, "I: balance " + std::to_string(p) + " (want 0), Ok " + std::to_string(Delta(0, CheatReason::Ok)) +
                                       " (1), Failed " + std::to_string(Delta(1, CheatReason::Failed)) + " (7)");
            return false;
        }
        UE_LOGI("%s host I: eight removals against %d left the balance at 0, one Ok and seven Failed", kTag, kStake);
        return true;
    case 3:
        if (Delta(0, CheatReason::UnknownItem) != 1 || Delta(1, CheatReason::Ok) != 1) {
            Verdict("host", false, "J: the host's answers to the two requests were not UnknownItem and Ok");
            return false;
        }
        return true;
    case 4:
        if (g.stage == 1) {
            if (Delta(0, CheatReason::Off) != 1 || p != g.points) {
                Verdict("host", false, "K: a request from a client whose right was withheld was not refused Off (Off " +
                                           std::to_string(Delta(0, CheatReason::Off)) + ", balance " + std::to_string(p) + ")");
                return false;
            }
            UE_LOGI("%s host K: the withheld client's request was answered Off", kTag);
            coop::command_sync::Submit("mv user " + ClientGuid(s) + " permission unset " + RightEntry().node);
            g.stage = 2;
            g.stepAt = Now();
            return false;
        }
        if (ClientHoldsRight(s)) return true;
        if (Timed(15000)) Invalid("host", "K: the permission edit did not give the right back");
        return false;
    default: return true;
    }
}

void Step() { ++g.sub; g.sent = false; g.stepAt = Now(); }

// One hostile arm's next move. Sub 0 waits for the host's go; the rest are the arm's own requests and judgements.
void ClientArmTick(coop::net::Session& s) {
    namespace SL = coop::cheats::self_teleport;
    const std::string tag = kArmTag[g.arm];
    const char* const A = tag.c_str();
    Result r{};
    bool finished = false;
    if (g.sub == 0) {
        if (Seen(("cheatdrill " + tag + " go").c_str())) { g_nResults = 0; Step(); }
        else if (Timed(kSyncBoundMs)) ClientInvalid(A, "the host never said go");
        return;
    }
    auto reason = [](CheatReason c) { return static_cast<uint8_t>(c); };
    switch (g.arm) {
    case 0: {   // G: the raw requests, one at a time, each answered with its own reason
        const size_t i = static_cast<size_t>(g.sub - 1);
        if (i >= RawCaseCount()) { finished = true; break; }
        const RawCase& c = RawCaseAt(i);
        // Each case costs the sender a token on the host, so a case goes only when the sender's own bucket allows one;
        // the next goes after this one's answer (Ask).
        if (!g.sent && !CH::CanAsk(1)) {
            if (Timed(kSyncBoundMs)) ClientInvalid(A, "the request mirror never refilled");
            return;
        }
        if (!Ask([&] { SendRawForDrill(s, c.p); CH::ChargeMirrorForDrill(1.f); }, &r, A)) return;
        if (r.reason != reason(c.want)) {
            ClientFail(A, std::string(c.what) + " was answered " + std::to_string(r.reason) + ", not " + std::to_string(reason(c.want)));
            return;
        }
        Step();
        return;
    }
    case 1:   // H: ten props through the panel call, then a second ten-prop request sent raw, which the host must drop
        if (g.sub == 1) {
            if (CH::CanAsk(coop::net::kCheatMaxCount)) Step();
            else if (Timed(kSyncBoundMs)) ClientInvalid(A, "the request mirror never refilled");
            return;
        }
        if (g.sub == 2) {
            if (!Ask([] { CH::SpawnItem("crowbar", coop::net::kCheatMaxCount, coop::net::kCheatInFront); }, &r, A)) return;
            if (r.reason != reason(CheatReason::Ok) || r.done != coop::net::kCheatMaxCount) { ClientFail(A, "the ten-prop request was answered " + std::to_string(r.reason) + " with " + std::to_string(r.done)); return; }
            Step();
            return;
        }
        if (g.sub == 3) {
            const auto p = RawSpawn(0x80001000u, "crowbar", coop::net::kCheatMaxCount);
            if (!Ask([&] { SendRawForDrill(s, p); }, &r, A)) return;
            if (r.reason != reason(CheatReason::Rate)) { ClientFail(A, "the second ten-prop request was answered " + std::to_string(r.reason) + ", not Rate"); return; }
            Step();
            return;
        }
        finished = true;
        break;
    case 2:   // I: eight removals in one burst
        if (g.sub == 1) {
            if (CH::CanAsk(8)) Step();
            else if (Timed(kSyncBoundMs)) ClientInvalid(A, "the request mirror never refilled");
            return;
        }
        if (!g.sent) {
            g.resultIdx = g_nResults;
            for (int i = 0; i < 8; ++i) CH::AddPoints(-100000);
            g.sent = true;
            g.stepAt = Now();
            return;
        }
        if (g_nResults < g.resultIdx + 8) { if (Timed(kEffectBoundMs)) ClientInvalid(A, "the host did not answer all eight"); return; }
        {
            int ok = 0, failed = 0;
            for (int i = g.resultIdx; i < g.resultIdx + 8; ++i) {
                if (g_results[i].reason == reason(CheatReason::Ok)) ++ok;
                else if (g_results[i].reason == reason(CheatReason::Failed)) ++failed;
            }
            if (ok != 1 || failed != 7) { ClientFail(A, "the eight removals were answered Ok " + std::to_string(ok) + ", Failed " + std::to_string(failed) + " (want 1, 7)"); return; }
        }
        finished = true;
        break;
    case 3:   // J: two requests in flight; the earlier one's answer must not take the status line
        if (g.sub == 1) {
            if (CH::CanAsk(2)) Step();
            else if (Timed(kSyncBoundMs)) ClientInvalid(A, "the request mirror never refilled");
            return;
        }
        if (!g.sent) {
            g.resultIdx = g_nResults;
            CH::SpawnItem("nosuchitem_xyz", 1, coop::net::kCheatInFront);
            CH::AddPoints(1);
            g.sent = true;
            g.stepAt = Now();
            return;
        }
        if (g_nResults < g.resultIdx + 2) { if (Timed(kEffectBoundMs)) ClientInvalid(A, "the host did not answer both"); return; }
        {
            const Result& a = g_results[g.resultIdx];
            const Result& b = g_results[g.resultIdx + 1];
            if (!a.pending) { ClientFail(A, "the earlier request's answer ended the status line's wait"); return; }
            if (b.pending || b.statusReason != reason(CheatReason::Ok)) { ClientFail(A, "the later request's answer did not take the status line"); return; }
        }
        finished = true;
        break;
    case 4:   // K: the host withheld the right
        if (g.sub == 1) {
            if (!CH::ReadClientRights().mine) {
                if (CH::ReadClientRights().Open()) { ClientFail(A, "the panes offer the cheats while the right is withheld"); return; }
                Step();
            } else if (Timed(20000)) {
                ClientInvalid(A, "the host's withheld right never reached this client");
            }
            return;
        }
        if (g.sub == 2) {
            if (!Ask([] { CH::AddPoints(7); }, &r, A)) return;
            if (r.reason != reason(CheatReason::Off)) { ClientFail(A, "a request with the right withheld was answered " + std::to_string(r.reason) + ", not Off"); return; }
            Step();
            return;
        }
        finished = true;
        break;
    case 5:   // L: Unstuck puts the body on the NavMesh
        if (g.sub == 1) {   // lift the body 150 cm straight up, off the mesh, through the game's own teleport
            void* const local = coop::players::Registry::Get().Local();
            ue_wrap::FVector at{};
            if (!local || !ue_wrap::engine::TryGetActorLocation(local, at)) { ClientInvalid(A, "this player's place does not read"); return; }
            ue_wrap::FRotator view{};
            if (void* const ctrl = ue_wrap::engine::GetController(local)) view = ue_wrap::engine::GetControlRotation(ctrl);
            coop::teleport_client::ApplyArgs lift{at.X, at.Y, at.Z + kLiftCm, view.Pitch, view.Yaw, view.Roll};
            if (!coop::teleport_client::ApplyLocally(lift)) { ClientInvalid(A, "the lift's teleport reported failure"); return; }
            g.liftStartZ = at.Z;
            Step();
            return;
        }
        if (g.sub == 2) {   // the lift read back, then Unstuck on the next game-thread task
            void* const local = coop::players::Registry::Get().Local();
            ue_wrap::FVector at{};
            if (!local || !ue_wrap::engine::TryGetActorLocation(local, at)) { ClientInvalid(A, "this player's place does not read"); return; }
            // Measured at the read: a body that has already fallen some of the way is judged on where Unstuck lands it,
            // and one that fell back to within the minimum drop of its start was never lifted high enough to prove it.
            if (at.Z < g.liftStartZ + kMinDropCm) { ClientInvalid(A, "the lift did not hold the body 100 cm above its start"); return; }
            g.liftedZ = at.Z;
            g.crowbars = static_cast<int>(SL::Outcomes().landed);
            g.fires = SL::Outcomes().refused;
            SL::Unstuck();
            Step();
            return;
        }
        if (g.sub == 3) {
            const auto o = SL::Outcomes();
            if (o.landed == static_cast<unsigned>(g.crowbars) && o.refused == g.fires) {
                if (Timed(kEffectBoundMs)) ClientInvalid(A, "Unstuck did not run");
                return;
            }
            // No NavMesh near the start (or a teleport the game reported failed) is the tool working as designed, not a
            // mechanism that failed: the arm has nothing to prove here.
            if (o.refused > g.fires) { ClientInvalid(A, "Unstuck landed nowhere: no NavMesh near the start, or the game's teleport failed"); return; }
            void* const local = coop::players::Registry::Get().Local();
            ue_wrap::FVector at{};
            if (!local || !ue_wrap::engine::TryGetActorLocation(local, at)) { ClientInvalid(A, "this player's place does not read"); return; }
            // The landing is below where the body was lifted to, and the body is read where it stands after the move and
            // must be at the landing point, within its capsule.
            if (o.target.Z > g.liftedZ - kMinDropCm) {
                ClientFail(A, "Unstuck landed " + std::to_string(static_cast<int>(g.liftedZ - o.target.Z)) + " cm below the lifted body, not " + std::to_string(static_cast<int>(kMinDropCm)));
                return;
            }
            const float half = ue_wrap::engine::GetActorCharacterHalfHeight(local);
            const float off = std::sqrt((at.X - o.target.X) * (at.X - o.target.X) + (at.Y - o.target.Y) * (at.Y - o.target.Y) +
                                        (at.Z - o.target.Z) * (at.Z - o.target.Z));
            if (!(half > 0.f)) { ClientInvalid(A, "the body's capsule does not read"); return; }
            if (off > half) {
                ClientFail(A, "after Unstuck the body stands " + std::to_string(static_cast<int>(off)) + " cm from the landing point");
                return;
            }
            UE_LOGI("%s client L: Unstuck put the body %.0f cm from its landing point (%.0f, %.0f, %.0f)", kTag, off, o.target.X,
                    o.target.Y, o.target.Z);
        }
        finished = true;
        break;
    }
    if (!finished) return;
    UE_LOGI("%s client %s PASS", kTag, A);
    DC::Say("cheatdrill " + tag + " sent");
    ++g.arm;
    g.sub = 0;
}


}  // namespace

bool HostArms(coop::net::Session& s) {
    if (g.arm >= kArms) return true;
    const std::string tag = kArmTag[g.arm];
    if (g.sub == 0) {
        // The arms before this one spent the sender's bucket: the arm starts when the host's bucket is whole again.
        if (g.stage == 0 && CH::RequestBucketFullAtMs(1) > coop::net::NowMs()) {
            if (Timed(kEffectBoundMs)) Invalid("host", tag + ": the client's request bucket never refilled");
            return false;
        }
        if (!ArmPrep(g.arm, s)) return false;
        DC::Say("cheatdrill " + tag + " go");
        g.sub = 1;
        g.stepAt = Now();
    } else if (g.sub == 1) {
        if (Seen(("cheatdrill " + tag + " sent").c_str())) { g.sub = 2; g.stepAt = Now(); }
        else if (Timed(kSyncBoundMs)) Invalid("host", tag + ": the client never finished its requests");
    } else if (ArmJudge(g.arm, s)) {
        ++g.arm;
        g.sub = 0;
        g.stage = 0;
        g.stepAt = Now();
    }
    return g.arm >= kArms;
}

bool ClientArms(coop::net::Session& s) {
    if (g.arm < kArms) ClientArmTick(s);
    return g.arm >= kArms;
}

}  // namespace coop::dev::cheat_drill::detail
