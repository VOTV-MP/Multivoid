// coop/save/save_notice.cpp -- see coop/save/save_notice.h.

#include "coop/save/save_notice.h"

#include "coop/comms/peer_action_feed.h"  // the line, in the player's language
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/inventory.h"           // ResolveSaveSlot: the hosted world's save object
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/save_capture.h"        // GameEventState: whether this save is refused or gathers
#include "ue_wrap/engine/save_to_slot_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace coop::save_notice {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowSave, "save_notice", L"mainGamemode_C", L"save", Declared);
constexpr int kTag = 0x53415645;  // 'SAVE'

std::atomic<coop::net::Session*> g_session{nullptr};
bool     g_subscribed = false;
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg      g_reg = Reg::Pending;
bool     g_settled = false;
uint32_t g_saves = 0;            // HOST: the save attempts this session
uint32_t g_heard = 0;            // CLIENT: the host's save attempts this session
bool     g_unknownSaid = false;  // HOST: an event state that could not be read is said once

coop::peer_action_feed::Action LineFor(coop::net::WorldSaveOutcome o) {
    return o == coop::net::WorldSaveOutcome::Saved ? coop::peer_action_feed::Action::SavedGame
                                                   : coop::peer_action_feed::Action::SaveBlockedByEvent;
}

// HOST: one save attempt reaches every player.
void Report(coop::net::Session& s, coop::net::WorldSaveOutcome outcome) {
    coop::net::WorldSavedPayload p{};
    p.saveNumber = ++g_saves;
    p.outcome = static_cast<uint8_t>(outcome);
    const bool sent = s.SendReliable(coop::net::ReliableKind::WorldSaved, &p, sizeof(p));
    coop::peer_action_feed::AnnounceDirect(0, LineFor(outcome));
    UE_LOGI("save_notice: the host's save (#%u) %s -- WorldSaved %s", g_saves,
            outcome == coop::net::WorldSaveOutcome::Saved ? "took" : "did not take: an event is running",
            sent ? "sent" : "had no peer");
}

// HOST: the engine wrote a world save. Only THE world counts -- the live save object under a slot of the
// game's own: the join capture writes it to a zcoop_ scratch slot, and the save-slot menu writes save objects
// of its own; neither is the hosted world saved.
void OnWorldSaveWritten(void* saveObject, const wchar_t* slotName) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (std::wcsncmp(slotName, L"zcoop_", 6) == 0) return;
    if (saveObject != ue_wrap::inventory::ResolveSaveSlot()) return;
    // The write is the tail of the game's save, in the same call as its world gather, which skips while the game
    // counts an event as running: the slot is then written with the world as the last gather left it, and the
    // host sees the game's error. The answer now is the one the gather got. One that cannot be read is not taken
    // for an event.
    const ue_wrap::save_capture::EventState ev = ue_wrap::save_capture::GameEventState();
    if (ev == ue_wrap::save_capture::EventState::Unknown && !g_unknownSaid) {
        g_unknownSaid = true;
        UE_LOGW("save_notice: whether an event is running could not be read -- this save is reported as saved "
                "(said once)");
    }
    const coop::net::WorldSaveOutcome outcome = ev == ue_wrap::save_capture::EventState::On
                                                    ? coop::net::WorldSaveOutcome::EventRunning
                                                    : coop::net::WorldSaveOutcome::Saved;
    Report(*s, outcome);
}

// HOST: the game's save was called. Unless told to bypass the event, it shows its error and returns without a
// write while an event is running, so the write above never hears this attempt.
sg::Verdict OnSavePre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return sg::Verdict::Run;
    static R::ParamOffset sBypass{L"bypassEvent"};
    const int32_t off = sBypass.Of(c.function);
    bool bypass = false;
    if (off >= 0 && c.locals) std::memcpy(&bypass, c.locals + off, sizeof(bypass));
    if (!bypass && ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::On)
        Report(*s, coop::net::WorldSaveOutcome::EventRunning);
    return sg::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_subscribed) g_subscribed = ue_wrap::save_to_slot_hook::AddWritten(&OnWorldSaveWritten);
    if (g_reg == Reg::Pending)
        g_reg = sg::WatchName(kRowSave, kTag, &OnSavePre, nullptr) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    if (g_settled || g_reg == Reg::Pending) return;
    if (g_reg == Reg::Registered) sg::ResolvePendingNames();
    if (g_reg == Reg::Registered && sg::NameWatchLive(kRowSave, kTag)) {
        g_settled = true;
    } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowSave, kTag)) {
        g_settled = true;
        UE_LOGE("save_notice: the watch on the game's save is dead -- a save refused during an event reaches no "
                "player");
    }
}

void OnWorldSaved(const coop::net::WorldSavedPayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;
    ++g_heard;
    const auto outcome = static_cast<coop::net::WorldSaveOutcome>(p.outcome);
    coop::peer_action_feed::AnnounceDirect(0, LineFor(outcome));
    UE_LOGI("save_notice: the host %s (its #%u, heard #%u)",
            outcome == coop::net::WorldSaveOutcome::Saved ? "saved the world" : "tried to save during an event",
            p.saveNumber, g_heard);
}

void OnDisconnect() {
    if (g_saves || g_heard) UE_LOGI("save_notice: session end -- saved=%u heard=%u", g_saves, g_heard);
    g_saves = 0;
    g_heard = 0;
    g_unknownSaid = false;
}

}  // namespace coop::save_notice
