// coop/save/save_notice.cpp -- see coop/save/save_notice.h.

#include "coop/save/save_notice.h"

#include "coop/comms/peer_action_feed.h"  // the line, in the player's language
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // Local: the pawn whose movement mode the mid-air test reads

#include "ue_wrap/actors/inventory.h"           // ResolveSaveSlot: the hosted world's save object
#include "ue_wrap/actors/puppet.h"              // ReadCharacterIsWalking
#include "ue_wrap/actors/vitals.h"              // Dreaming
#include "ue_wrap/world/game_mode.h"
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
Counts   g_counts;               // HOST: the save attempts this session by outcome; CLIENT: the ones heard
bool     g_refusedSaid = false;  // the watch was refused: said once, retried from Tick

coop::peer_action_feed::Action LineFor(coop::net::WorldSaveOutcome o) {
    using coop::net::WorldSaveOutcome;
    using coop::peer_action_feed::Action;
    switch (o) {
    case WorldSaveOutcome::Saved:   return Action::SavedGame;
    case WorldSaveOutcome::Unknown: return Action::SaveMaybeNotTaken;
    default:                        return Action::SaveBlockedByEvent;
    }
}

uint32_t& CountOf(coop::net::WorldSaveOutcome o, uint32_t& ok, uint32_t& failed, uint32_t& unknown) {
    using coop::net::WorldSaveOutcome;
    return o == WorldSaveOutcome::Saved ? ok : (o == WorldSaveOutcome::Unknown ? unknown : failed);
}

const char* VerdictOf(coop::net::WorldSaveOutcome o) {
    using coop::net::WorldSaveOutcome;
    return o == WorldSaveOutcome::Saved   ? "took"
         : o == WorldSaveOutcome::Unknown ? "was written, but an event could not be ruled out"
                                          : "did not take: an event is running";
}

// HOST: one save attempt reaches every player. The line goes out Direct, past the peer-actions toggle: a save is
// the host's word to every player, and on a client this line is the only sign of it, not one peer's action to mute.
void Report(coop::net::Session& s, coop::net::WorldSaveOutcome outcome) {
    coop::net::WorldSavedPayload p{};
    CountOf(outcome, g_counts.savedOk, g_counts.savedFailed, g_counts.savedUnknown)++;
    const uint32_t saves = g_counts.savedOk + g_counts.savedFailed + g_counts.savedUnknown;
    p.saveNumber = saves;
    p.outcome = static_cast<uint8_t>(outcome);
    const bool sent = s.SendReliable(coop::net::ReliableKind::WorldSaved, &p, sizeof(p));
    coop::peer_action_feed::AnnounceDirect(0, LineFor(outcome));
    UE_LOGI("save_notice: the host's save (#%u) %s -- WorldSaved %s", saves, VerdictOf(outcome),
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
    // host sees the game's error. An event state that cannot be read is neither "saved" nor an event: it is its own
    // outcome, and every player is told the save may not have taken.
    using EventState = ue_wrap::save_capture::EventState;
    const EventState ev = ue_wrap::save_capture::GameEventState();
    if (ev == EventState::Unknown)
        UE_LOGW("save_notice: whether an event is running could not be read -- this save is reported as "
                "possibly not taken");
    Report(*s, ev == EventState::On      ? coop::net::WorldSaveOutcome::EventRunning
               : ev == EventState::Unknown ? coop::net::WorldSaveOutcome::Unknown
                                           : coop::net::WorldSaveOutcome::Saved);
}

// The tests the game's save runs before its event test, in its order (mainGamemode.cpp, the save ubergraph at label
// 67457 with bypassEvent false): dreaming returns silently, the Custom mode shows "invalid mode", a player not walking
// shows "Cannot save in mid air", and only then is the event asked. Null when none of them refuses; else why, so the
// event is blamed only when it is the test that refuses. A test that cannot be read is not a pass.
const char* EarlierRefusal() {
    float dreaming = 0.f;
    if (!ue_wrap::vitals::Read(ue_wrap::vitals::Field::Dreaming, &dreaming)) return "dreaming could not be read";
    if (dreaming != 0.f) return "dreaming";
    const int mode = ue_wrap::game_mode::ReadLocal();
    if (mode < 0) return "the game mode could not be read";
    if (mode == ue_wrap::game_mode::kCustom) return "invalid mode";
    bool walking = false;
    if (!ue_wrap::puppet::ReadCharacterIsWalking(coop::players::Registry::Get().Local(), walking))
        return "the player's movement mode could not be read";
    return walking ? nullptr : "mid air";
}

// HOST: the game's save was called, by a press or by its autosave. Unless told to bypass the event, it shows its
// error and returns without a write while an event is running, so the write above never hears this attempt. Only
// the event's refusal is reported: an earlier test that refuses means no save began and the game shows the host
// its own error, so nothing is sent (said here, once per call).
sg::Verdict OnSavePre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return sg::Verdict::Run;
    static R::ParamOffset sBypass{L"bypassEvent"};
    const int32_t off = sBypass.Of(c.function);
    bool bypass = false;
    if (off >= 0 && c.locals) std::memcpy(&bypass, c.locals + off, sizeof(bypass));
    if (bypass) return sg::Verdict::Run;
    if (const char* why = EarlierRefusal()) {
        UE_LOGI("save_notice: a save call refused before the event test (%s) -- nothing sent", why);
        return sg::Verdict::Run;
    }
    if (ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::On)
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
    if (g_reg == Reg::Refused) {
        // The gate was not installed (or its name table was full) at Install; the gate says why once. Retried
        // from here until it takes, since the consumer, not the gate, owns the retry.
        if (!g_refusedSaid) {
            g_refusedSaid = true;
            UE_LOGE("save_notice: the watch on the game's save was refused -- retried each tick until it takes; "
                    "until then a save refused during an event reaches no player");
        }
        if (!sg::WatchName(kRowSave, kTag, &OnSavePre, nullptr)) return;
        g_reg = Reg::Registered;
        UE_LOGI("save_notice: the watch on the game's save is registered (after a refusal)");
    }
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRowSave, kTag)) {
        g_settled = true;
    } else if (sg::NameWatchSettled(kRowSave, kTag)) {
        g_settled = true;
        UE_LOGE("save_notice: the watch on the game's save is dead -- a save refused during an event reaches no "
                "player");
    }
}

void OnWorldSaved(const coop::net::WorldSavedPayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;
    const auto outcome = static_cast<coop::net::WorldSaveOutcome>(p.outcome);
    CountOf(outcome, g_counts.heardOk, g_counts.heardFailed, g_counts.heardUnknown)++;
    const uint32_t heard = g_counts.heardOk + g_counts.heardFailed + g_counts.heardUnknown;
    coop::peer_action_feed::AnnounceDirect(0, LineFor(outcome));  // Direct for the reason Report gives
    UE_LOGI("save_notice: the host's save %s (its #%u, heard #%u)", VerdictOf(outcome), p.saveNumber, heard);
}

Counts GetCounts() { return g_counts; }

void OnDisconnect() {
    const uint32_t saves = g_counts.savedOk + g_counts.savedFailed + g_counts.savedUnknown;
    const uint32_t heard = g_counts.heardOk + g_counts.heardFailed + g_counts.heardUnknown;
    if (saves || heard) UE_LOGI("save_notice: session end -- saved=%u heard=%u", saves, heard);
    g_counts = Counts{};
}

}  // namespace coop::save_notice
