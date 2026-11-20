// coop/dev/miner_drill.cpp -- the mining rig drill's arm, log, chat and verdict; see coop/dev/miner_drill.h. The reads and the
// game's own use are miner_drill_rig.cpp; the arms are miner_drill_use.cpp, _claim, _record and _join.
//
// The two peers order their steps with drill_chat lines, each line led by the "mind " tag. The host judges every arm and says
// the verdict (rig: VERDICT miner-<arm>); a client reports what it read at its decision point in a chat line, and the host
// decides on it. Every step waits on an event it reads, and a bound only says INVALID.

#include "coop/dev/miner_drill.h"
#include "miner_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"

#include "ue_wrap/core/log.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace coop::dev::miner_drill::detail {

namespace DC = coop::dev::drill_chat;

namespace {

constexpr const char* kPrefix = "mind ";
constexpr size_t kMaxLine = 90;   // a chat line's text is cut past this, so a reason is kept short

bool g_done = false, g_chatArmed = false;

char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }

}  // namespace

int g_stepTicks = 0;
ue_wrap::CachedObjRef g_rig;
uint32_t g_eid = 0;

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::miner_drill);
        return v == "use" ? Arm::Use : v == "claim" ? Arm::Claim : v == "record" ? Arm::Record : v == "join" ? Arm::Join : Arm::Off;
    }();
    return a;
}

const char* TagOf(Arm arm) {
    switch (arm) {
    case Arm::Use:    return "miner-use";
    case Arm::Claim:  return "miner-claim";
    case Arm::Record: return "miner-record";
    case Arm::Join:   return "miner-join";
    default:          return "miner-off";
    }
}

void Fact(const char* fmt, ...) {
    char buf[700];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGI("[MINER-DRILL] [%c] %s", Who(), buf);
}

void Say(const std::string& line) { DC::Say(std::string(kPrefix) + line); }
bool Seen(const std::string& line) { return DC::SeenHas(std::string(kPrefix) + line); }
bool Rest(const std::string& prefix, std::string* out) { return DC::SeenRestOf(std::string(kPrefix) + prefix, out); }

void Go(int& step, int next) {
    step = next;
    g_stepTicks = 0;
}

bool Late(const char* why) {
    if (++g_stepTicks <= kBoundTicks) return false;
    Invalid(why);
    return true;
}

void Invalid(const char* why) {
    if (g_done) return;
    if (coop::roster::LocalIsHost()) {
        Verdict(coop::rig_verdict::Outcome::Invalid, "%s", why);
        return;
    }
    UE_LOGW("[MINER-DRILL] [C] INVALID %s", why);
    std::string line = std::string("c-invalid ") + why;
    if (line.size() > kMaxLine) line.resize(kMaxLine);
    Say(line);
    g_done = true;
}

void Verdict(coop::rig_verdict::Outcome outcome, const char* fmt, ...) {
    if (g_done) return;
    char reason[420];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);
    coop::rig_verdict::Say(TagOf(ArmOf()), outcome, "%s", reason);
    Say("end");
    g_done = true;
}

namespace {

void TickImpl(coop::net::Session& session) {
    const Arm arm = ArmOf();
    if (arm == Arm::Off || g_done) return;
    if (!g_chatArmed) {
        DC::SeenHas(kPrefix);   // the first read starts the chat history
        g_chatArmed = true;
    }
    if (coop::roster::LocalIsHost()) {
        std::string why;
        if (Rest("c-invalid ", &why)) {
            Verdict(coop::rig_verdict::Outcome::Invalid, "the client: %s", why.c_str());
            return;
        }
        switch (arm) {
        case Arm::Use:    UseHost(session); break;
        case Arm::Claim:  ClaimHost(session); break;
        case Arm::Record: RecordHost(session); break;
        case Arm::Join:   JoinHost(session); break;
        default: break;
        }
        return;
    }
    if (Seen("end")) {
        Fact("the host ended the drill");
        g_done = true;
        return;
    }
    switch (arm) {
    case Arm::Use:    UseClient(session); break;
    case Arm::Claim:  ClaimClient(session); break;
    case Arm::Record: RecordClient(session); break;
    case Arm::Join:   JoinClient(session); break;
    default: break;
    }
}

void Reset() {
    g_done = g_chatArmed = false;
    g_stepTicks = 0;
    g_rig.Reset();
    g_eid = 0;
    UseReset();
    ClaimReset();
    RecordReset();
    JoinReset();
}

}  // namespace

}  // namespace coop::dev::miner_drill::detail

namespace coop::dev::miner_drill {

void Tick(coop::net::Session* session) {
    if (session) detail::TickImpl(*session);
}

void OnDisconnect() { detail::Reset(); }

}  // namespace coop::dev::miner_drill
