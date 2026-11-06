// coop/dev/command_drill.cpp -- see coop/dev/command_drill.h.

#include "coop/dev/command_drill.h"

#include "coop/commands/command_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace coop::dev::command_drill {
namespace {

namespace CS = coop::command_sync;

enum class Mode : uint8_t { Off, On, Red, Grant, GrantRed, HostDeny };

Mode ModeNow() {
    static const Mode mode = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::command_drill);
        return v == "on"         ? Mode::On
               : v == "red"      ? Mode::Red
               : v == "grant"    ? Mode::Grant
               : v == "grantred" ? Mode::GrantRed
               : v == "hostdeny" ? Mode::HostDeny
                                 : Mode::Off;
    }();
    return mode;
}

bool IsGrantMode() { return ModeNow() == Mode::Grant || ModeNow() == Mode::GrantRed; }

enum class Phase : uint8_t {
    Ready, WaitA, SendLong, WaitLong, SendControl, WaitControl, SendB, WaitB, WaitFirst, Done
};

Phase g_phase = Phase::Ready;
bool g_isHost = false;
int g_replies = 0;   // phase A lines seen
int g_accepted = 0;  // phase B unknown-command lines seen before the rate notice

constexpr const char* kHelpHeader = "Commands you can use:";
constexpr const char* kHelpHeaderRed = "Commands you may use:";
constexpr const char* kHelpLine =
    "/help [command...] -- Lists the commands you can use, or shows how to use one.";
constexpr const char* kUnknownLine = "Unknown command '/nosuchcommand'. Type /help for the commands.";
constexpr const char* kRateNotice = "Too many commands -- wait a moment.";
constexpr int kBurstLines = 6;

// The longest line a request carries (protocol.h). A line one
// byte past it is refused where it is typed, on either role, and spends no command token; a line
// of exactly this length is dispatched.
constexpr size_t kLineMax = sizeof(coop::net::CommandRequestPayload::text);
constexpr const char* kTooLongLine = "That line is too long.";

// Phase A's lines after the header, by role: what `/help` and an unknown command answer. The host
// is the console and may use every command, listed in registry order (roots by name); a client
// holds only /help.
constexpr const char* kHostLines[] = {
    "/ban <who> [reason...] -- Bans a player by id and, on a direct link, by address.",
    "/banid <who> [reason...] -- Bans a player by id only.",
    "/cremate <who> -- Sends a player to the crematorium and kicks them after the burn; multivoid.cremate.exempt "
    "protects, kick.exempt does not.",
    kHelpLine,
    "/kick <who> [reason...] -- Disconnects a player.",
    "/mv user <who> permission set <node> [value] [contexts...] -- Sets a permission on a player.",
    "/mv user <who> permission unset <node> [contexts...] -- Removes a permission from a player.",
    "/mv user <who> permission settemp <node> [value] <for> [contexts...] -- Sets a permission on a player for a "
    "time.",
    "/mv user <who> permission unsettemp <node> [contexts...] -- Removes a player's timed permission.",
    "/mv user <who> parent add <group> -- Puts a player in a group.",
    "/mv user <who> parent remove <group> -- Takes a player out of a group.",
    "/mv user <who> info -- Shows a player's groups and permissions.",
    "/mv group <name> permission set <node> [value] [contexts...] -- Sets a permission on a group.",
    "/mv group <name> permission unset <node> [contexts...] -- Removes a permission from a group.",
    "/mv group <name> permission settemp <node> [value] <for> [contexts...] -- Sets a permission on a group for a "
    "time.",
    "/mv group <name> permission unsettemp <node> [contexts...] -- Removes a group's timed permission.",
    "/mv group <name> parent add <group> -- Makes a group inherit another.",
    "/mv group <name> parent remove <group> -- Stops a group inheriting another.",
    "/mv group <name> setweight <weight> -- Sets a group's weight (the highest wins).",
    "/mv group <name> info -- Shows a group's parents, weight and permissions.",
    "/mv creategroup <name> -- Creates a group.",
    "/mv deletegroup <name> -- Deletes a group no one names.",
    "/mv listgroups -- Lists the groups.",
    "/mv reload -- Reloads the permission files from disk.",
    "/reset <setting> -- Puts a server setting back to its default.",
    "/set <setting> <value...> -- Changes a server setting.",
    "/tphere <who> -- Brings a player to you (the host).",
    "/unban <id> -- Lifts a ban.",
    kUnknownLine};
constexpr const char* kClientLines[] = {kHelpLine, kUnknownLine};

// grant / grantred / hostdeny send one line and read its first reply.
constexpr const char* kUnbanLine = "unban 00000000";
constexpr const char* kBanidLine = "banid 00000000000000000000000000000000";
constexpr const char* kNoBanReply = "No ban matches 00000000.";
constexpr const char* kRefusedPrefix = "You do not have permission for /";
constexpr const char* kUnbanRefusedPrefix = "You do not have permission for /unban";

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

void Finish() {
    g_phase = Phase::Done;
    CS::SetReplyObserver(nullptr);
}

void Fail(int n, std::string_view got, const char* want) {
    UE_LOGE("[CMD-DRILL] FAIL: reply %d was '%s', expected '%s'", n, std::string(got).c_str(), want);
    Finish();
}

void OnPhaseA(std::string_view line) {
    const char* const* lines = g_isHost ? kHostLines : kClientLines;
    const int count = 1 + static_cast<int>(g_isHost ? std::size(kHostLines) : std::size(kClientLines));
    const int i = g_replies;
    const char* want = i == 0 ? (ModeNow() == Mode::Red ? kHelpHeaderRed : kHelpHeader) : lines[i - 1];
    if (line != want) { Fail(i + 1, line, want); return; }
    if (++g_replies < count) return;
    g_phase = Phase::SendLong;
}

// The reply to the 204-byte line: the local refusal, the same text on both roles.
void OnTooLong(std::string_view line) {
    if (line != kTooLongLine) { Fail(g_replies + 1, line, kTooLongLine); return; }
    ++g_replies;
    g_phase = g_isHost ? Phase::SendControl : Phase::SendB;
}

// The host's reply to its own 203-byte line: not cut, so it carries the whole 203-byte word.
std::string HostControlReply() {
    return "Unknown command '/" + std::string(kLineMax, 'x') + "'. Type /help for the commands.";
}

void OnControl(std::string_view line) {
    const std::string want = HostControlReply();
    if (line != want) { Fail(g_replies + 1, line, want.c_str()); return; }
    ++g_replies;
    UE_LOGI("[CMD-DRILL] host DONE");
    Finish();
}

void OnPhaseB(std::string_view line) {
    if (line == kUnknownLine) {
        // All kBurstLines answered and none limited: the notice will never come, so end here.
        if (++g_accepted < kBurstLines) return;
    } else if (line != kRateNotice) {
        Fail(3 + g_accepted + 1, line, kRateNotice);
        return;
    }
    if (g_accepted < 1 || g_accepted > kBurstLines - 1) {
        UE_LOGE("[CMD-DRILL] FAIL: %d of %d lines were answered before the rate notice, expected 1 to %d",
                g_accepted, kBurstLines, kBurstLines - 1);
        Finish();
        return;
    }
    UE_LOGI("[CMD-DRILL] client DONE (accepted %d of %d, told once)", g_accepted, kBurstLines);
    Finish();
}

// The client's one line of grant / grantred: the first reply says whether the host's permissions
// let it through (`unban` with nothing banned answers kNoBanReply).
void OnGrantReply(std::string_view line) {
    if (line == kNoBanReply) UE_LOGI("[CMD-DRILL] grant PASS");
    else if (StartsWith(line, kRefusedPrefix)) UE_LOGE("[CMD-DRILL] FAIL: refused");
    else UE_LOGE("[CMD-DRILL] FAIL: reply '%s'", std::string(line).c_str());
    Finish();
}

// The host's one line of hostdeny: its own permission file denies it /unban.
void OnHostDenyReply(std::string_view line) {
    if (StartsWith(line, kUnbanRefusedPrefix)) UE_LOGI("[CMD-DRILL] hostdeny PASS");
    else UE_LOGE("[CMD-DRILL] FAIL: hostdeny reply '%s'", std::string(line).c_str());
    Finish();
}

// Every delivered reply line of this peer, in order.
void Observe(std::string_view line) {
    if (g_phase == Phase::WaitA) OnPhaseA(line);
    else if (g_phase == Phase::WaitLong) OnTooLong(line);
    else if (g_phase == Phase::WaitControl) OnControl(line);
    else if (g_phase == Phase::WaitB) OnPhaseB(line);
    else if (g_phase == Phase::WaitFirst && ModeNow() == Mode::HostDeny) OnHostDenyReply(line);
    else if (g_phase == Phase::WaitFirst) OnGrantReply(line);
}

}  // namespace

bool ClientReady(coop::net::Session* s) {
    return s->connected() && coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

void Tick(coop::net::Session* session) {
    if (ModeNow() == Mode::Off || !session || !session->running()) return;
    switch (g_phase) {
    case Phase::Ready:
        g_isHost = session->role() == coop::net::Role::Host;
        // grant and grantred are a client's, hostdeny the host's: the other role has nothing to do.
        if ((ModeNow() == Mode::HostDeny && !g_isHost) || (IsGrantMode() && g_isHost)) {
            g_phase = Phase::Done;
            return;
        }
        if (!g_isHost && !ClientReady(session)) return;
        g_replies = 0;
        g_accepted = 0;
        CS::SetReplyObserver(&Observe);
        if (ModeNow() == Mode::HostDeny || IsGrantMode()) {
            g_phase = Phase::WaitFirst;
            CS::Submit(ModeNow() == Mode::GrantRed ? kBanidLine : kUnbanLine);
            UE_LOGI("[CMD-DRILL] %s sent its line", g_isHost ? "host" : "client");
            return;
        }
        g_phase = Phase::WaitA;
        CS::Submit("help");
        CS::Submit("nosuchcommand");
        UE_LOGI("[CMD-DRILL] %s sent phase A", g_isHost ? "host" : "client");
        return;
    case Phase::SendLong:
        g_phase = Phase::WaitLong;
        CS::Submit(std::string(kLineMax + 1, 'x'));
        UE_LOGI("[CMD-DRILL] %s sent a %zu-byte line", g_isHost ? "host" : "client", kLineMax + 1);
        return;
    case Phase::SendControl:
        g_phase = Phase::WaitControl;
        CS::Submit(std::string(kLineMax, 'x'));
        UE_LOGI("[CMD-DRILL] host sent a %zu-byte line", kLineMax);
        return;
    case Phase::SendB:
        g_phase = Phase::WaitB;
        for (int i = 0; i < kBurstLines; ++i) CS::Submit("nosuchcommand");
        UE_LOGI("[CMD-DRILL] client sent phase B (%d lines)", kBurstLines);
        return;
    case Phase::WaitA:
    case Phase::WaitLong:
    case Phase::WaitControl:
    case Phase::WaitB:
    case Phase::WaitFirst:
    case Phase::Done:
        return;
    }
}

void OnDisconnect() {
    CS::SetReplyObserver(nullptr);
    g_phase = Phase::Ready;
    g_replies = 0;
    g_accepted = 0;
}

}  // namespace coop::dev::command_drill
