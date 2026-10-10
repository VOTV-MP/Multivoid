// coop/session/server_settings_sync.cpp -- the host's replicated server-scope rows reach every
// client: the snapshot when a slot is ready, a delta after a change, one row per message. A changed
// notify row is announced: the host prints one chat line, and each client prints its own when the
// row arrives with the announced bit.

#include "coop/session/server_settings_sync.h"

#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "l10n/l10n.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::server_settings_sync {

namespace cfg = coop::config;
namespace reg = coop::config_registry;
using coop::net::ReliableKind;
using coop::net::Role;
using coop::net::ServerSettingPayload;
using coop::net::Session;

// The protocol catalog includes nothing of the config, so the wire's two limits are stated in both
// and tied here. The key limit is the wire's own. The registry's value limit is the smaller: a
// `/set` line bounds it (command_sync.cpp). The wire's constant stays the payload capacity: every
// host writer is bounded by the registry's limit and the receiver bounds a value by it too, so
// neither end holds more than the registry lets a row hold.
static_assert(reg::kServerSettingTextMax <= net::kServerSettingTextMax &&
                  reg::kServerSettingKeyMax == net::kServerSettingKeyMax,
              "a replicated row's value must fit the wire, and the two key limits must agree");

namespace {

// The sender's handle: the process's one long-lived Session, set once at boot, before any other
// thread reads it, and never written again. The sender gates on running() instead of clearing it.
Session* g_session = nullptr;

// Whether this slot's snapshot has gone out. A delta goes only to a slot that holds it. Cleared by
// PerSlotState when the slot's occupant changes, on ClearAll and at session bring-up, which is
// exactly when a snapshot must go again (a duplicate after a same-slot re-occupation is harmless:
// the client's put is idempotent).
coop::roster_ledger::PerSlotState<bool> g_snapshotSent;

// The announced rows this client has taken in its session, the proofs' instrument: each client
// numbers its own lines. Zeroed by both session seams, which run off the game thread; counted on
// the game thread.
std::atomic<uint32_t> g_announcedCount{0};

// The value of a notify row as a reader holds it now: on or off for a flag, else its resolved
// text. Notify rows are Flag, Int, Float or Enum, so the text is ASCII.
std::string ShownText(const reg::Row* row) {
    const std::string resolved = cfg::ResolvedText(*row, cfg::EffectiveText(*row));
    if (row->kind == reg::Kind::Flag) return resolved == "1" ? "on" : "off";
    return resolved;
}

// The chat line for a changed notify row: our own label and our own rendering of the value,
// never the sender's text. Source prints the event's cvar name and value string
// (clientmode_shared.cpp:1235-1251); we print our label and our own rendering of the held value.
// The label and a flag's on/off are looked up in this peer's language; an enum or number stays
// its token, the word the player also types in /set. The log lines keep ShownText's English.
void PushAnnouncementLine(const reg::Row* row) {
    const std::string shown = ShownText(row);
    const char* value = shown.c_str();
    if (row->kind == reg::Kind::Flag)
        value = shown == "on" ? l10n::Tc("setting_value", "on") : l10n::Tc("setting_value", "off");
    char line[256];
    if (l10n::Fmt(line, sizeof(line), l10n::T("Server setting changed: %1$s is now %2$s."),
                  l10n::T(reg::RowLabel(row)), value) >= 0)
        coop::chat_feed::Push(std::string(line), coop::chat_feed::Keep::History);
}

// One row to one slot. The value is printed through the registry's one printed form.
// `flags` is kServerSettingAnnounced for an announced delta, 0 for a plain re-send and the snapshot.
// Returns false only when the send failed, so the caller's latch holds; a value the wire cannot
// carry is warned and counts as sent, or it would hold its slot's latch for ever.
bool SendRow(Session& s, int slot, const reg::Row* row, uint8_t flags, const char* why) {
    const std::string v = cfg::EffectiveText(*row);
    const size_t k = std::strlen(row->key);  // at most kServerSettingKeyMax: a build-time check
    if (v.size() > net::kServerSettingTextMax) {
        UE_LOGW("server_settings: %s not sent -- '%s' is longer than the wire carries", row->key,
                reg::ValueForLog(row, v).c_str());
        return true;
    }
    ServerSettingPayload p{};
    p.keyLen = static_cast<uint8_t>(k);
    p.valueLen = static_cast<uint8_t>(v.size());
    p.flags = flags;
    std::memcpy(p.text, row->key, k);
    std::memcpy(p.text + k, v.data(), v.size());
    const bool ok = s.SendReliableToSlot(slot, ReliableKind::ServerSetting, &p,
                                         static_cast<int>(3 + k + v.size()));
    if (ok)
        UE_LOGI("server_settings: sent %s=%s to slot %d (%s)", row->key,
                reg::ValueForLog(row, v).c_str(), slot, why);
    return ok;
}

// The subscriber, game thread: a replicated row changed on the host, so every slot that holds its
// snapshot is sent every replicated row again, each as its own message. A subscriber carries no
// row argument, so a change re-sends all of them (two today; a change is a person's act, so the
// walk is cold). The session layer's own begin and end notify once per row yet send nothing: at
// begin no slot holds a snapshot, at end the session no longer runs.
//
// It first takes the changed notify rows, before the gate, so a mark made while the gate refuses
// dies with its change. The host prints one line per taken row, not per recipient; every send of a
// taken row carries the announced bit, the join snapshot never.
//
// One row per message is ours: a payload is capped at 228 bytes. Source's rules say what is sent and
// when (reference/source-sdk-2013/src/public/tier1/iconvar.h:57-62), not how many to a message (the
// engine's NET_SetConVar format is outside the SDK); MTA's CSyncSettingsPacket sends the whole
// set as one fixed packet
// (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CSyncSettingsPacket.cpp).
void OnReplicatedRowChanged() {
    const std::vector<const reg::Row*> announced = cfg::TakePendingAnnouncements();
    Session* s = g_session;
    if (!s || !s->running() || s->role() != Role::Host) return;
    for (const reg::Row* row : announced) {
        PushAnnouncementLine(row);
        UE_LOGI("server_settings: host announced %s=%s", row->key, ShownText(row).c_str());
    }
    size_t count = 0;
    const reg::Row* rows = reg::Rows(count);
    for (size_t i = 0; i < count; ++i) {
        if (!reg::IsReplicated(&rows[i])) continue;
        const bool isAnnounced =
            std::find(announced.begin(), announced.end(), &rows[i]) != announced.end();
        const uint8_t flags = isAnnounced ? net::kServerSettingAnnounced : uint8_t{0};
        for (int slot = 1; slot < net::kMaxPeers; ++slot) {
            if (g_snapshotSent[slot]) SendRow(*s, slot, &rows[i], flags, "delta");
        }
    }
}

void DropMalformed(int payloadLen) {
    UE_LOGW("server_settings: ServerSetting dropped -- bad length %d", payloadLen);
}

}  // namespace

void BindSession(Session& session) {
    g_session = &session;
    session.SetStopListener(&OnSessionEnd);
}

void SubscribeRows() { cfg::SubscribeServerScope(&OnReplicatedRowChanged); }

void OnSessionStart(bool host) {
    if (g_session->running()) {
        UE_LOGW("server_settings: a session is already running; its layer is kept");
        return;
    }
    g_announcedCount.store(0);
    cfg::SessionLayerBegin(host);
}

// The session-end seam, a divergence from both precedents. MTA ends a client's copy of the
// server's settings by OWNERSHIP: they are members of the session object CClientGame
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.h:908-909) and go with it when
// the mod unloads on disconnect (CPacketHandler.cpp:685). Source ends them by a level-end revert
// (CGameRules::LevelShutdownPostEntity -> RevertSavedConvars,
// reference/source-sdk-2013/src/game/shared/gamerules.cpp:659-663).
// Ours is a process-wide layer, because config's Resolve is process-wide and our one Session
// outlives each session, so the transport's one stop listener ends it.
void OnSessionEnd() {
    g_announcedCount.store(0);
    cfg::SessionLayerEnd();
}

void HostTick(Session& session) {
    if (!session.running() || session.role() != Role::Host) return;
    for (int slot = 1; slot < net::kMaxPeers; ++slot) {
        // READY, the Join loop's own gate: a send before it would ride lane 0.
        if (g_snapshotSent[slot] || !session.IsSlotReady(slot)) continue;
        size_t count = 0;
        const reg::Row* rows = reg::Rows(count);
        bool all = true;
        for (size_t i = 0; i < count; ++i) {
            if (!reg::IsReplicated(&rows[i])) continue;
            if (!SendRow(session, slot, &rows[i], 0, "snapshot")) all = false;
        }
        // The Join latch's rule: set only on success, retried next tick.
        if (all) g_snapshotSent[slot] = true;
    }
}

void HandleServerSetting(Session& session, const Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("server_settings::HandleServerSetting");
    if (msg.payloadLen < 3) {
        DropMalformed(msg.payloadLen);
        return;
    }
    const auto& p = *reinterpret_cast<const ServerSettingPayload*>(msg.payload);
    if (p.keyLen == 0 || p.keyLen > net::kServerSettingKeyMax ||
        p.valueLen > reg::kServerSettingTextMax || msg.payloadLen != 3 + p.keyLen + p.valueLen) {
        DropMalformed(msg.payloadLen);
        return;
    }
    // Host to client only: a host never takes it, and a client takes it from the host alone.
    if (session.role() == Role::Host) {
        UE_LOGW("server_settings: ServerSetting dropped -- a client never sends it (slot %d)",
                msg.senderPeerSlot);
        return;
    }
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("server_settings: ServerSetting dropped -- sender is not the host (slot %d)",
                msg.senderPeerSlot);
        return;
    }
    // A row named on the wire by its key is found by its key, as Source finds a cvar by name; one
    // walk per received message, which is a person's act.
    char key[net::kServerSettingKeyMax + 1];
    std::memcpy(key, p.text, p.keyLen);
    key[p.keyLen] = 0;
    const reg::Row* row = reg::FindRow(key);
    if (!row || !reg::IsReplicated(row)) {
        UE_LOGW("server_settings: ServerSetting dropped -- '%s' names no replicated row", key);
        return;
    }
    const bool accepted = cfg::SessionLayerPut(row, std::string(p.text + p.keyLen, p.valueLen));
    // Only bit 0 is read, and only for a notify row of this client's own registry: a forged bit on
    // any other row reaches no line.
    if (accepted && (p.flags & net::kServerSettingAnnounced) && reg::IsNotify(row)) {
        const uint32_t n = g_announcedCount.fetch_add(1) + 1;
        PushAnnouncementLine(row);
        UE_LOGI("server_settings: announced #%u %s=%s", n, row->key, ShownText(row).c_str());
    }
}

}  // namespace coop::server_settings_sync
