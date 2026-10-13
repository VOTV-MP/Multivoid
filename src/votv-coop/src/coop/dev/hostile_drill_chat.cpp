// coop/dev/hostile_drill_chat.cpp -- see coop/dev/hostile_drill_chat.h.

#include "coop/dev/hostile_drill_chat.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_log.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/dispatch/event_feed.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace coop::dev::hostile_drill::chat {
namespace {

using Clock = std::chrono::steady_clock;
using Kind = coop::net::ReliableKind;

constexpr auto kFailBound = std::chrono::seconds(180);  // a failure bound, never a wait

// chat_ill_formed: the forged content, named once and shared by the sender and the host's judge so the two never drift.
constexpr const char* kChatForged   = "hostile-drill forged";    // + one lone 0x80 appended
constexpr const char* kChatSentinel = "hostile-drill sentinel";  // one well-formed line behind it

// chat_flood: every line is the tag, the client's stamp (digits, so the lines widen no alphabet past ASCII), then its
// leg's suffix: an index for the burst, "last" for the recovery, "b<i>" for the bucket. The host never learns the stamp:
// it reads the tag on rows committed after it armed, from the judged slot, and tells the legs apart by the suffix.
constexpr std::string_view kTag = "hostile-drill chat_flood ";
constexpr int kBurst = 10;
// The recovery waits out the debt the burst left: from tier 1 at -2.5, Source's slowed refill passes a line after
// 24.75 s; the margin covers the burst's later lines draining after its first. The bucket leg waits for tier 1 to refill
// to its cap (at most 9 s from a line that talked), then sends four lines 0.9 s apart: three pass, the fourth meets an
// empty tier 1 while the interval still passes. Spacings the rule itself sets, not waits for a milestone: a refused
// line has no answer to wait on.
constexpr auto     kRecovery      = std::chrono::seconds(30);
constexpr auto     kRefill        = std::chrono::seconds(12);
constexpr int      kBucketLines   = 4;
constexpr int      kBucketPassing = 3;
constexpr auto     kBucketEvery   = std::chrono::milliseconds(900);

// ---- client (sender) ----
enum class Send { Idle, IllFormedSent, BurstSent, Recovering, LastSent, Refilling, Bucket, Done };
Send              g_send = Send::Idle;
Clock::time_point g_sendAt{};  // when the current client phase began, or the next send is due
std::string       g_stamp;
int               g_bucketSent = 0;

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool SendLine(coop::net::Session* s, std::string_view text) {
    coop::net::ChatMessagePayload p{};
    const size_t n = std::min(text.size(), sizeof(p.text));
    std::memcpy(p.text, text.data(), n);
    p.len = static_cast<uint8_t>(n);
    return s->SendReliable(Kind::ChatMessage, &p, sizeof(p));
}

std::string FloodLine(const std::string& suffix) { return std::string(kTag) + g_stamp + " " + suffix; }

// The host sends a committed row back to its author, so a line in this client's own feed means the host drained it.
bool FeedHas(const std::string& line) {
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (r.line.find(line) != std::string_view::npos) found = true;
    });
    return found;
}

void ClientEnd(Arm a, bool inconclusive, const char* why) {
    if (inconclusive) UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE -- %s", ArmName(a), why);
    else              UE_LOGW("[HOSTILE-DRILL] %s FAIL: %s", ArmName(a), why);
    g_send = Send::Done;
}

// chat_ill_formed: the forged line now, the sentinel one chat spacing later: the forged line passes the host's rate and
// talks before its content refuses it, so a sentinel inside the interval would be refused too.
void TickClientIllFormed(coop::net::Session* s, Arm a, Clock::time_point now) {
    if (g_send == Send::Idle) {
        // A lone continuation byte: ill-formed UTF-8 unconditionally.
        if (!SendLine(s, std::string(kChatForged) + static_cast<char>(0x80))) {
            ClientEnd(a, false, "the forged line was not sent");  // and no sentinel behind it, so the host cannot pass
            return;
        }
        g_send = Send::IllFormedSent;
        g_sendAt = now + std::chrono::milliseconds(coop::dev::kTalkEveryMs);
        UE_LOGI("[HOSTILE-DRILL] %s sent the forged line; the sentinel follows at the chat spacing", ArmName(a));
        return;
    }
    if (now < g_sendAt) return;
    if (!SendLine(s, kChatSentinel)) { ClientEnd(a, false, "the sentinel was not sent"); return; }
    UE_LOGI("[HOSTILE-DRILL] %s sent the sentinel", ArmName(a));
    g_send = Send::Done;
}

void TickClientFlood(coop::net::Session* s, Arm a, Clock::time_point now) {
    switch (g_send) {
    case Send::Idle:
        g_stamp = std::to_string(static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
        for (int i = 0; i < kBurst; ++i) {
            if (!SendLine(s, FloodLine(std::to_string(i)))) {
                ClientEnd(a, true, "a line of the burst was not sent");
                return;
            }
        }
        UE_LOGI("[HOSTILE-DRILL] %s sent %d lines in one tick (stamp %s)", ArmName(a), kBurst, g_stamp.c_str());
        g_send = Send::BurstSent;
        g_sendAt = now;
        return;
    case Send::BurstSent:
        if (FeedHas(FloodLine("0"))) {
            UE_LOGI("[HOSTILE-DRILL] %s the first line came back; the next goes after the recovery", ArmName(a));
            g_send = Send::Recovering;
            g_sendAt = now + kRecovery;
        } else if (now - g_sendAt > kFailBound) {
            ClientEnd(a, true, "the first line never came back");
        }
        return;
    case Send::Recovering:
        if (now < g_sendAt) return;
        if (!SendLine(s, FloodLine("last"))) { ClientEnd(a, true, "the recovery line was not sent"); return; }
        UE_LOGI("[HOSTILE-DRILL] %s sent the recovery line", ArmName(a));
        g_send = Send::LastSent;
        g_sendAt = now;
        return;
    case Send::LastSent:
        if (FeedHas(FloodLine("last"))) {
            UE_LOGI("[HOSTILE-DRILL] %s the recovery line came back; the bucket's lines go after the refill",
                    ArmName(a));
            g_send = Send::Refilling;
            g_sendAt = now + kRefill;
        } else if (now - g_sendAt > kFailBound) {
            ClientEnd(a, true, "the recovery line never came back");
        }
        return;
    case Send::Refilling:
    case Send::Bucket:
        if (now < g_sendAt) return;
        if (!SendLine(s, FloodLine("b" + std::to_string(g_bucketSent)))) {
            ClientEnd(a, true, "a line of the bucket leg was not sent");
            return;
        }
        g_send = Send::Bucket;
        g_sendAt = now + kBucketEvery;
        if (++g_bucketSent == kBucketLines) {
            UE_LOGI("[HOSTILE-DRILL] %s sent the bucket's %d lines", ArmName(a), kBucketLines);
            g_send = Send::Done;
        }
        return;
    default:
        return;
    }
}

// ---- host (judge) ----
// The judge counts only what arrives after it arms: a line the lobby record kept from an earlier run must never read as
// this run's.
enum class Step { WaitArm, Judge, WaitBurst, WaitLast, WaitBucket, Done };
Step              g_step = Step::WaitArm;
int               g_slot = -1;
uint32_t          g_lineSeqBase = 0;  // the last line committed before the arm
uint32_t          g_drainedBase = 0;  // chat_flood: the slot's chat messages drained before the arm
Clock::time_point g_stepAt{};         // when the current host step began

void Verdict(Arm a, bool pass, const std::string& detail) {
    if (pass) UE_LOGI("[HOSTILE-DRILL] %s PASS -- %s", ArmName(a), detail.c_str());
    else      UE_LOGW("[HOSTILE-DRILL] %s FAIL: %s", ArmName(a), detail.c_str());
    g_step = Step::Done;
}

uint32_t LastLineSeq() {
    uint32_t last = 0;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) { if (r.lineSeq > last) last = r.lineSeq; });
    return last;
}

void ArmHost(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    g_lineSeqBase = LastLineSeq();
    g_drainedBase = coop::event_feed::DrainedFrom(g_slot, Kind::ChatMessage);
    g_stepAt = Clock::now();
    g_step = a == Arm::ChatFlood ? Step::WaitBurst : Step::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d (last line %u, chat messages drained %u)", ArmName(a), g_slot,
            g_lineSeqBase, g_drainedBase);
}

void JudgeIllFormed(Arm a) {
    bool found0x80 = false, foundSentinel = false;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (r.lineSeq <= g_lineSeqBase || r.slot != static_cast<uint8_t>(g_slot)) return;
        if (r.text == kChatSentinel) foundSentinel = true;
        if (r.text.find(static_cast<char>(0x80)) != std::string::npos) found0x80 = true;
    });
    // Decide only once the sentinel (sent AFTER the forged line on the same ordered lane) is committed: that is the
    // signal that the forged line has already been judged.
    if (!foundSentinel) return;
    if (found0x80) Verdict(a, false, "an ill-formed line was committed");
    else           Verdict(a, true, "the ill-formed line was refused; the well-formed one was kept");
}

// chat_flood's rows in the lobby record since the arm, from the judged slot, by leg.
struct FloodRows { int burst = 0, last = 0, bucket = 0; };
FloodRows CountFloodRows() {
    FloodRows n;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (r.lineSeq <= g_lineSeqBase || r.slot != static_cast<uint8_t>(g_slot)) return;
        const std::string_view text = r.text;
        if (!text.starts_with(kTag)) return;
        const std::string_view suffix = text.substr(text.rfind(' ') + 1);
        if (suffix == "last") ++n.last;
        else if (suffix.starts_with('b')) ++n.bucket;
        else ++n.burst;
    });
    return n;
}

// The leg's messages are drained (`want` in all since the arm), or the leg's bound has passed.
bool Drained(Arm a, uint32_t want, const char* leg) {
    const uint32_t drained = coop::event_feed::DrainedFrom(g_slot, Kind::ChatMessage) - g_drainedBase;
    if (drained >= want) return true;
    if (Clock::now() - g_stepAt > kFailBound)
        Verdict(a, false, std::string("timeout (") + leg + ": drained " + std::to_string(drained) + " of " +
                              std::to_string(want) + ")");
    return false;
}

std::string OfCommitted(int committed, int sent) {
    return std::to_string(committed) + " of " + std::to_string(sent) + " committed";
}

void JudgeFlood(Arm a) {
    if (g_step == Step::WaitBurst) {
        if (!Drained(a, kBurst, "the burst")) return;
        const FloodRows n = CountFloodRows();
        UE_LOGI("[HOSTILE-DRILL] %s drained the burst; %s", ArmName(a), OfCommitted(n.burst, kBurst).c_str());
        if (n.burst == 0)           Verdict(a, false, "muted-first (the burst's first line was not committed)");
        else if (n.burst >= kBurst) Verdict(a, false, "every-line-committed (" + OfCommitted(n.burst, kBurst) + ")");
        else if (n.burst > 1)       Verdict(a, false, "interval-broken (" + OfCommitted(n.burst, kBurst) + ")");
        else { g_step = Step::WaitLast; g_stepAt = Clock::now(); }
        return;
    }
    // A refused line that came back later is a fail at every later read: Source drops it for good.
    const auto returned = [&a](const FloodRows& n) {
        if (n.burst <= 1) return false;
        Verdict(a, false, "refused-line-returned (" + std::to_string(n.burst) + " burst rows)");
        return true;
    };
    if (g_step == Step::WaitLast) {
        if (!Drained(a, kBurst + 1, "the recovery line")) return;
        const FloodRows n = CountFloodRows();
        if (returned(n)) return;
        if (n.last == 0) Verdict(a, false, "muted (the line after the recovery was not committed)");
        else { g_step = Step::WaitBucket; g_stepAt = Clock::now(); }
        return;
    }
    if (!Drained(a, kBurst + 1 + kBucketLines, "the bucket's lines")) return;
    const FloodRows n = CountFloodRows();
    UE_LOGI("[HOSTILE-DRILL] %s drained the bucket's lines; %s", ArmName(a),
            OfCommitted(n.bucket, kBucketLines).c_str());
    if (returned(n)) return;
    if (n.bucket > kBucketPassing)      Verdict(a, false, "bucket-ignored (" + OfCommitted(n.bucket, kBucketLines) + ")");
    else if (n.bucket < kBucketPassing) Verdict(a, false, "bucket-early (" + OfCommitted(n.bucket, kBucketLines) + ")");
    else Verdict(a, true, "the burst's first line committed and the rest dropped for good; the line after the recovery "
                          "committed; three of the bucket's four lines committed");
}

}  // namespace

const char* ArmName(Arm a) {
    return a == Arm::ChatIllFormed ? "chat_ill_formed" : a == Arm::ChatFlood ? "chat_flood" : "off";
}

void TickClient(coop::net::Session* s, Arm a) {
    // Wait for THIS client's own join to be over before sending (its world-ready announce and the host's connect
    // replay are both behind this edge), so the host has armed. A readiness predicate, not a clock.
    if (g_send == Send::Done || !OwnJoinOver()) return;
    const Clock::time_point now = Clock::now();
    if (a == Arm::ChatIllFormed) TickClientIllFormed(s, a, now);
    else                         TickClientFlood(s, a, now);
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (g_step == Step::WaitArm) { ArmHost(s, a); return; }
    if (g_step == Step::Done) return;
    if (!s->HasPeerConn(g_slot)) {
        if (a == Arm::ChatFlood) {
            UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE -- the judged slot left before the verdict", ArmName(a));
            g_step = Step::Done;
        } else {
            Verdict(a, false, "the judged slot left before the verdict");
        }
        return;
    }
    if (a == Arm::ChatFlood) {
        JudgeFlood(a);
        return;
    }
    JudgeIllFormed(a);
    if (g_step == Step::Judge && Clock::now() - g_stepAt > kFailBound)
        Verdict(a, false, "timeout, the sentinel never arrived");
}

void OnDisconnect() {
    g_send = Send::Idle;
    g_sendAt = {};
    g_stamp.clear();
    g_bucketSent = 0;
    g_step = Step::WaitArm;
    g_slot = -1;
    g_lineSeqBase = 0;
    g_drainedBase = 0;
    g_stepAt = {};
}

}  // namespace coop::dev::hostile_drill::chat
