// coop/commands/command_sync.cpp -- see coop/commands/command_sync.h.
//
// Shapes: chat_sync (the render-thread post, the host check, the slot fan-out), desk_ping_sync (the
// per-slot token bucket and the once-per-10-s notice), the ChatMessage / ChatLine dispatch cases.
// MTA runs a console command for the player the packet's socket names
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CPacketTranslator.cpp:226-246) and Source
// runs a client's command for the client that sent it
// (reference/source-sdk-2013/src/game/server/client.cpp:1549-1625); the sender is never read
// from the payload here either.

#include "coop/commands/command_sync.h"

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_line.h"
#include "coop/commands/moderation_commands.h"
#include "coop/commands/mv_commands.h"
#include "coop/commands/settings_commands.h"

#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/moderation/ban_list.h"
#include "coop/moderation/moderation.h"
#include "coop/moderation/seen_players.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/permissions/action_log.h"
#include "coop/permissions/permission_host.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/player_handshake.h"
#include "coop/text/utf8_codec.h"
#include "l10n/l10n.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace coop::command_sync {
namespace {

namespace GT = ue_wrap::game_thread;
using coop::commands::Caller;
using coop::commands::PlayerView;
using coop::net::kMaxPeers;

std::atomic<coop::net::Session*> g_session{nullptr};

// A sender's budget: a burst of three for a typed correction, then two a second. Deliberate
// divergence: LuckPerms limits per sender with one 500 ms window
// (reference/LuckPerms/common/src/main/java/me/lucko/luckperms/common/command/CommandManager.java:105)
// and MTA's console has no limit (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2406-2415);
// the burst is ours.
constexpr coop::net::IntentBudget kBudget{3.0f, 2.0f};
// An over-budget or unreadable line is dropped; the sender is told at most this often. Deliberate
// divergence: LuckPerms tells the sender nothing, only the log
// (CommandManager.java:158-160); ours tells a person typing, who otherwise sees nothing.
constexpr uint64_t kSayEveryMs = 10000;
// The longest command word a log line carries.
constexpr size_t kLogWordMax = 32;
// The longest line a request carries, and so the longest line a peer may type.
constexpr size_t kLineMax = sizeof(coop::net::CommandRequestPayload::text);
// A `/set <key> <value>` line must be able to carry the longest value a replicated row may hold:
// the registry's value limit is this line less "set ", the longest key and one space.
static_assert(coop::config_registry::kServerSettingTextMax ==
                  kLineMax - (sizeof("set ") - 1) - coop::config_registry::kServerSettingKeyMax - 1,
              "a replicated row's value limit must equal what a /set line carries after its key");

coop::net::IntentBucket g_bucket[kMaxPeers];
uint64_t g_nextSayMs[kMaxPeers] = {};
uint64_t g_nextBadMs[kMaxPeers] = {};

bool g_warnedUnreadableReply = false;
void (*g_observer)(std::string_view line) = nullptr;

// The permission system's check. Outside a running hosted session the store is not consulted: the
// console passes every node and nobody else exists. Inside one, the caller's proved id is checked
// over its own resolved nodes, and the console's owner step answers last: what the chain leaves
// undefined is true for it, an explicit false still denies it. A caller with no proved id gets the
// node's declared default.
bool PermissionCheck(const Caller& caller, std::string_view node, bool defaultGranted) {
    if (!coop::moderation::HostedSessionRunning()) return caller.isOperator ? true : defaultGranted;
    if (caller.playerId.empty()) return defaultGranted;
    return coop::permissions::host::Allows(caller.playerId, node, defaultGranted, caller.isOperator);
}

// A third party's answer, for the notify qualifier: no owner step, only what the chain says.
bool HoldsNode(std::string_view playerId, std::string_view node, bool defaultGranted) {
    return coop::permissions::host::Allows(playerId, node, defaultGranted, false);
}

// The one source of chance (`@r`). Game thread only.
int Pick(int count) {
    static std::mt19937 rng{std::random_device{}()};
    if (count <= 0) return 0;
    return std::uniform_int_distribution<int>(0, count - 1)(rng);
}

// Whether a seen-players record exists for the id, whatever nick it holds.
bool SeenBefore(std::string_view id) {
    coop::seen_players::Entry e;
    return coop::seen_players::FindByGuid(std::string(id).c_str(), e);
}

const coop::commands::Policy g_policy{&PermissionCheck, &Pick, &HoldsNode,
                                      &coop::permissions::host::HoldsExplicitly, &SeenBefore,
                                      &coop::permissions::host::NowSeconds};

// The nick a seen-players record holds for an id; empty when there is none.
std::string RecordNick(std::string_view id) {
    coop::seen_players::Entry e;
    if (!coop::seen_players::FindByGuid(std::string(id).c_str(), e)) return std::string();
    return e.nick;
}

// The admin-action port: appends one record to the action log inside a hosted session. The record is
// the host's account of what an admin did, so a failure to write it is a warning, never a refusal of
// the action that already ran.
void LogAction(const coop::permissions::Action& a) {
    if (!coop::moderation::HostedSessionRunning()) return;
    const auto folder = coop::permissions::host::ActionLogFolder();
    if (folder.empty()) {
        UE_LOGW("action log: not recorded -- the server folder is unknown");
        return;
    }
    if (!coop::permissions::AppendAction(folder, a, coop::permissions::host::NowSeconds()))
        UE_LOGW("action log: could not append to actions.jsonl -- the action stands");
}

// The moderation commands' ports: the verbs of coop/moderation and this transport's ReplyTo.
coop::commands::moderation::Ports RealModerationPorts() {
    coop::commands::moderation::Ports p;
    p.kick = &coop::moderation::KickPlayer;
    p.ban = &coop::moderation::BanPlayer;
    p.banOffline = &coop::moderation::BanOffline;
    p.unban = &coop::moderation::Unban;
    p.teleport = &coop::moderation::TeleportPlayerToMe;
    p.hosted = &coop::moderation::HostedSessionRunning;
    p.idsWithPrefix = &coop::ban_list::IdsWithPrefix;
    p.recordNick = &RecordNick;
    p.notify = &ReplyTo;
    p.log = &LogAction;
    return p;
}

// The settings commands' ports. A word names a row by its key; only a server-scope row that is not
// a credential can be set or reset by name, so a local row is never reachable from a command line.
const coop::config_registry::Row* RowNamed(std::string_view word) {
    return coop::config_registry::FindRow(std::string(word).c_str());
}

const char* CredentialKeyNamed(std::string_view word) {
    const coop::config_registry::Row* row = RowNamed(word);
    return row != nullptr && coop::config_registry::IsCredentialKey(row->key) ? row->key : nullptr;
}

const coop::config_registry::Row* ServerRowNamed(std::string_view word) {
    const coop::config_registry::Row* row = RowNamed(word);
    if (row == nullptr || !coop::config_registry::IsServerScope(row) ||
        coop::config_registry::IsCredentialKey(row->key))
        return nullptr;
    return row;
}

bool ValueValidFor(const coop::config_registry::Row* row, const std::string& value, std::string* why) {
    return coop::config::ValueValidForKey(row->key, value, why);
}

std::string CurrentValueOf(const coop::config_registry::Row* row) {
    return coop::config::EffectiveText(*row);
}

coop::commands::settings::Ports RealSettingsPorts() {
    coop::commands::settings::Ports p;
    p.credentialKey = &CredentialKeyNamed;
    p.findServerRow = &ServerRowNamed;
    p.valid = &ValueValidFor;
    p.set = &coop::config::SetServerRow;
    p.reset = &coop::config::ResetServerRow;
    p.current = &CurrentValueOf;
    p.log = &LogAction;
    return p;
}

// Whether the host holds the permission log's node: the owner passes it unless it denied itself.
bool HostHoldsLog() {
    return coop::permissions::host::Allows(coop::net::peer_identity::LocalGuid(), coop::commands::kAdminLogNode,
                                           false, /*owner=*/true);
}

// The /mv commands' ports: the permission host's edit, reload and live model, and this transport's
// ReplyTo.
coop::commands::mv::Ports RealMvPorts() {
    coop::commands::mv::Ports p;
    p.hosted = &coop::moderation::HostedSessionRunning;
    p.apply = &coop::permissions::host::Apply;
    p.reload = &coop::permissions::host::Reload;
    p.live = &coop::permissions::host::Live;
    p.storeBroken = &coop::permissions::host::StoreBroken;
    p.recordNick = &RecordNick;
    p.hostHoldsLog = &HostHoldsLog;
    p.now = &coop::permissions::host::NowSeconds;
    p.notify = &ReplyTo;
    return p;
}

struct RegistryHolder {
    coop::commands::Registry registry;
    RegistryHolder() {
        if (!coop::commands::RegisterBuiltins(registry))
            UE_LOGE("command_sync: builtin registration refused");
        if (!coop::commands::moderation::Register(registry, RealModerationPorts()))
            UE_LOGE("commands: the moderation commands did not register");
        if (!coop::commands::settings::Register(registry, RealSettingsPorts()))
            UE_LOGE("commands: the settings commands did not register");
        if (!coop::commands::mv::Register(registry, RealMvPorts()))
            UE_LOGE("commands: the /mv commands did not register");
    }
};

// A reply line on this peer's own feed: private, never in the history.
void Deliver(std::string_view utf8, const std::wstring& wide) {
    coop::chat_feed::Push(wide, coop::chat_feed::Keep::Transient);
    UE_LOGI("command_sync: reply '%s'", std::string(utf8).c_str());
    if (g_observer) g_observer(utf8);
}

// The chat form of a line this peer composes for itself, in its language; the log and the
// observer keep the English view. `msgid` views a NUL-terminated literal, so .data() is a C string.
std::wstring Shown(std::string_view msgid) {
    const char* text = l10n::T(msgid.data());
    return coop::text::FromUtf8Lossy(text, std::strlen(text));
}

void ReadPosition(PlayerView& v, void* actor) {
    ue_wrap::FVector p{};
    if (!actor || !ue_wrap::engine::TryGetActorLocation(actor, p)) return;
    v.hasPosition = true;
    v.x = p.X;
    v.y = p.Y;
    v.z = p.Z;
}

// The player record a command sees, built when the command runs, on the game thread: who sits in
// each slot from the roster ledger, each position read now. Nothing is cached. It runs only for a
// line this process dispatches itself, and only a server dispatches: the host's own line or a
// client's request on the host, or solo play. That slot reads the local nickname, guid and player,
// every other slot the ledger and its puppet; a client's player id is the session's proof for the
// ledger row's generation, which exists from admission, before the Join fills the row's guid.
std::vector<PlayerView> BuildPlayers(coop::net::Session* s) {
    std::vector<PlayerView> out;
    coop::players::Registry& reg = coop::players::Registry::Get();
    if (!s || !s->running()) {
        // Out of session: the local player alone, as the roster's lone row.
        PlayerView v;
        v.slot = 0;
        v.nick = coop::text::ToUtf8(coop::player_handshake::LocalNickname());
        v.playerId = coop::net::peer_identity::LocalGuid();
        v.worldReady = true;
        ReadPosition(v, reg.Local());
        out.push_back(std::move(v));
        return out;
    }
    // DispatchLocal and OnRequest run only on their own server, the listen host or solo, whose
    // slot is 0.
    constexpr int ownSlot = 0;
    for (int slot = 0; slot < kMaxPeers; ++slot) {
        const coop::roster_ledger::Row& row = coop::roster_ledger::Get(slot);
        if (!row.occupied()) continue;
        PlayerView v;
        v.slot = slot;
        v.playerNo = row.playerNo;
        if (slot == ownSlot) {
            v.nick = coop::text::ToUtf8(coop::player_handshake::LocalNickname());
            v.playerId = coop::net::peer_identity::LocalGuid();
            v.worldReady = true;
            ReadPosition(v, reg.Local());
        } else {
            v.generation = row.bornGeneration;
            v.worldReady = s->IsSlotWorldReady(slot);
            v.nick = coop::text::ToUtf8(coop::roster_ledger::DisplayName(slot));
            v.playerId = s->ProvedGuidForSlotWithToken(slot, v.generation);
            coop::RemotePlayer* puppet = reg.Puppet(static_cast<uint8_t>(slot));
            ue_wrap::FVector p{};
            if (puppet && puppet->TryGetLocation(p)) {
                v.hasPosition = true;
                v.x = p.X;
                v.y = p.Y;
                v.z = p.Z;
            }
        }
        out.push_back(std::move(v));
    }
    return out;
}

// The local operator's own line: the listen host, or solo play. It is the console: the owner step
// answers last, so what the permission chain leaves undefined passes and an explicit false still
// denies it. Deliberate divergence: Source's listen host is the admin and passes every check
// (reference/source-sdk-2013/src/game/server/util.cpp:622-648); MTA has no listen host, its console
// is an ACL account.
void DispatchLocal(const std::string& line) {
    Caller self{0, 0, true};
    self.playerId = coop::net::peer_identity::LocalGuid();
    const auto result = coop::commands::Dispatch(Commands(), self, line,
                                                 BuildPlayers(g_session.load(std::memory_order_acquire)),
                                                 g_policy);
    for (const std::string& reply : result.replies) ReplyTo(self, reply);
}

// The command word of a line for the log, never its arguments (a /msg carries private text). A
// command name is [a-z0-9], so every byte outside 0x21..0x7E is written as '?': a quoted word may
// hold spaces (they would forge the "-> ran" marker) and U+0085 / U+2028 / U+2029 are multi-byte
// line breaks a reader splits on, none of which a client's word may put in a log line.
std::string LogWord(std::string_view line) {
    const coop::commands::ParsedLine parsed = coop::commands::SplitLine(line);
    std::string word = parsed.words.empty() ? std::string() : parsed.words[0];
    word = coop::text::CapUtf8Bytes(std::move(word), kLogWordMax);
    for (char& c : word) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) c = '?';
    }
    return word;
}

// A request that cannot be read is dropped and told at most once per kSayEveryMs a sender.
// Deliberate divergence: MTA drops an unreadable command packet silently
// (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CCommandPacket.cpp:15-35,
// CPacketTranslator.cpp:251-256); ours answers, as a person typing would otherwise see nothing.
void BadRequest(const Caller& caller, uint64_t now) {
    const int slot = caller.slot;
    if (now < g_nextBadMs[slot]) return;
    g_nextBadMs[slot] = now + kSayEveryMs;
    ReplyTo(caller, "Could not read that command.");
    UE_LOGW("command_sync: slot %d sent an unreadable command line", slot);
}

}  // namespace

void Install(coop::net::Session* s) { g_session.store(s, std::memory_order_release); }

coop::commands::Registry& Commands() {
    UE_ASSERT_GAME_THREAD("command_sync::Commands");
    static RegistryHolder holder;
    return holder.registry;
}

void Submit(std::string line) {
    GT::Post([line = std::move(line)] {
        coop::net::Session* s = g_session.load(std::memory_order_acquire);
        const bool inSession = s && s->running();
        // A line the request cannot carry whole is refused on every peer, before the role decides
        // where it runs: it never runs cut, and a host and a client answer alike.
        if (line.size() > kLineMax) {
            const std::string_view tooLong = L10N_MARK("That line is too long.");
            Deliver(tooLong, Shown(tooLong));
            return;
        }
        if (!inSession || s->role() == coop::net::Role::Host) {
            DispatchLocal(line);
            return;
        }
        // A client never dispatches: its line runs where its authority is, on the host. A bare `/`
        // has nothing to send; the hint is local text.
        if (line.empty()) {
            const std::string_view hint = coop::commands::kHelpHint;
            Deliver(hint, Shown(hint));
            return;
        }
        coop::net::CommandRequestPayload p{};
        p.len = static_cast<uint8_t>(line.size());
        std::memcpy(p.text, line.data(), line.size());
        if (!s->SendReliable(coop::net::ReliableKind::CommandRequest, &p, sizeof(p))) {
            const std::string_view failed = L10N_MARK("Could not send the command.");
            Deliver(failed, Shown(failed));
        }
    });
}

void OnRequest(const uint8_t* bytes, size_t len, int slot) {
    UE_ASSERT_GAME_THREAD("command_sync::OnRequest");
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (slot < 1 || slot >= kMaxPeers) return;
    // A seated slot whose Join has not landed has no player id yet: its line is dropped, before the
    // bucket. MTA's server ignores a not-yet-joined player's commands
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2410, IsJoined); an unmodified
    // client sends its Join one tick after its slot arrives, so no typed line is lost.
    if (coop::roster_ledger::Get(slot).guid.empty()) return;
    // The generation of the ledger row the game thread reconciled, not the slot's live one: a line
    // its predecessor queued before leaving reads the predecessor's token, finds no proved id for
    // it and is dropped, so it never runs with the successor's grants.
    const uint32_t gen = coop::roster_ledger::Get(slot).bornGeneration;
    const std::string proved = s->ProvedGuidForSlotWithToken(slot, gen);
    if (proved.empty()) return;
    Caller caller{slot, gen, false};
    caller.playerId = proved;

    const uint64_t now = ::GetTickCount64();
    if (!g_bucket[slot].Take(kBudget, now)) {
        if (now >= g_nextSayMs[slot]) {
            g_nextSayMs[slot] = now + kSayEveryMs;
            ReplyTo(caller, "Too many commands -- wait a moment.");
            UE_LOGW("command_sync: slot %d over the command rate; lines dropped", slot);
        }
        return;
    }

    coop::net::CommandRequestPayload p{};
    std::wstring wide;
    if (len != sizeof(p)) { BadRequest(caller, now); return; }
    std::memcpy(&p, bytes, sizeof(p));
    if (p.len < 1 || p.len > sizeof(p.text) || !coop::text::FromUtf8Strict(p.text, p.len, &wide)) {
        BadRequest(caller, now);
        return;
    }

    // MTA strips control codes where a command enters the console, as chat_sync does at its boundary
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CConsole.cpp:44). TAB is kept: MTA's
    // stripControlCodes drops every byte below 32
    // (reference/mtasa-blue/Shared/mods/deathmatch/logic/Utils.cpp:277-293), SanitizeUtf8 keeps TAB
    // as chat does, and SplitLine splits on ' ' only.
    const std::string line = coop::text::SanitizeUtf8(p.text, p.len);
    const auto result = coop::commands::Dispatch(Commands(), caller, line, BuildPlayers(s), g_policy);
    UE_LOGI("command_sync: slot %d /%s -> %s", slot, LogWord(line).c_str(),
            result.ran ? "ran" : "refused");
    for (const std::string& reply : result.replies) ReplyTo(caller, reply);
}

void ReplyTo(const Caller& to, std::string_view line) {
    UE_ASSERT_GAME_THREAD("command_sync::ReplyTo");
    if (to.slot <= 0) {
        Deliver(line, coop::text::FromUtf8Lossy(line.data(), line.size()));
        return;
    }
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    // The slot may have changed hands since the asker typed: the line belongs to that person only.
    if (s->peerGenerationForSlot(to.slot) != to.generation) return;
    coop::net::CommandReplyPayload p{};
    const std::string cut = coop::text::CapUtf8Bytes(std::string(line), sizeof(p.text));
    p.len = static_cast<uint8_t>(cut.size());
    std::memcpy(p.text, cut.data(), cut.size());
    s->SendReliableToSlot(to.slot, coop::net::ReliableKind::CommandReply, &p, sizeof(p));
}

void OnReply(const coop::net::CommandReplyPayload& p) {
    UE_ASSERT_GAME_THREAD("command_sync::OnReply");
    std::wstring wide;
    // The strict decode of the RAW bytes is the gate: stripping a control byte out of an ill-formed
    // sequence could splice its neighbours into a valid one, a repair nobody sent.
    bool readable = p.len <= sizeof(p.text) && coop::text::FromUtf8Strict(p.text, p.len, &wide);
    // MTA's client strips control codes from every echo it shows
    // (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:1443), as our chat receiver
    // does (coop/comms/chat_sync.cpp, OnChatLine).
    std::string line;
    if (readable) {
        line = coop::text::SanitizeUtf8(p.text, p.len);
        readable = coop::text::FromUtf8Strict(line.data(), line.size(), &wide);
    }
    if (!readable) {
        if (!g_warnedUnreadableReply) {
            g_warnedUnreadableReply = true;
            UE_LOGW("command_sync: an unreadable reply from the host");
        }
        return;
    }
    Deliver(line, wide);
}

void SetReplyObserver(void (*fn)(std::string_view line)) { g_observer = fn; }

void OnSlotDisconnected(int slot) {
    if (slot < 0 || slot >= kMaxPeers) return;
    g_bucket[slot].Reset();
    g_nextSayMs[slot] = 0;
    g_nextBadMs[slot] = 0;
}

void OnDisconnect() {
    for (int slot = 0; slot < kMaxPeers; ++slot) OnSlotDisconnected(slot);
    g_warnedUnreadableReply = false;
}

}  // namespace coop::command_sync
