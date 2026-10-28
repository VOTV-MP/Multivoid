// coop/interactables/eraser_press_intent.cpp -- see coop/interactables/eraser_press_intent.h.

#include "coop/interactables/eraser_press_intent.h"

#include "coop/element/bind_wait.h"       // Holds: an insert still waiting here for its drive
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/active_drive.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/drive_eraser.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace coop::eraser_press_intent {
namespace {

namespace DC = ue_wrap::drive_chain;
namespace DE = ue_wrap::drive_eraser;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowActionOptionIndex, "eraser_press_intent", L"signalDriveEraser_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowUbergraph, "eraser_press_intent", L"signalDriveEraser_C", L"ExecuteUbergraph_signalDriveEraser",
           Declared);
constexpr int kTagEraser = 0x45524153;  // 'ERAS'
constexpr int kTagResume = 0x45524152;  // 'ERAR'

// An arm's-length button, like the drone console's.
constexpr float kEraserReachUU = 400.0f;

// A press is one wipe.
constexpr coop::net::IntentBudget kPressBudget{2.0f, 1.0f};
constexpr size_t   kMaxPending     = 4;
constexpr uint64_t kSayEveryMs     = 10000;

// EraserPressIntentPayload::event, client to host: my player pressed delete with this drive seated. The host's
// events are DE::Show's values.
constexpr uint8_t kEvIntent = 0;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_actionWatched = false, g_resumeWatched = false;
bool g_settled = false;
uint64_t g_sent = 0, g_pressed = 0, g_denied = 0, g_shown = 0;
uint64_t g_nextUnboundSayMs = 0;

// HOST: whether the eraser was busy as the running delete press entered, for its POST to tell a start from a click.
bool g_pressWasBusy = false;

// HOST, one sender: the drive eids its presses named, queued with their bucket, and the latches that clear with its
// occupant.
struct Slot {
    coop::net::IntentQueue<uint32_t> queue;
    uint64_t nextSayMs = 0;      // the full queue's refusal, said once in kSayEveryMs
    bool     waitSaid  = false;  // a wait for the body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

coop::net::Session* HostSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Host ? s : nullptr;
}
coop::net::Session* ClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Client ? s : nullptr;
}

void* SeatedDrive() {
    void* slot = DC::SlotActor(DC::kRoleEraser);
    return slot ? DC::SlotDrive(slot) : nullptr;
}

bool Send(coop::net::Session* s, int toSlot, uint32_t driveEid, uint8_t event) {
    coop::net::EraserPressIntentPayload p{};
    p.driveEid = driveEid;
    p.event = event;
    return toSlot < 0 ? s->SendReliable(coop::net::ReliableKind::EraserPressIntent, &p, sizeof(p))
                      : s->SendReliableToSlot(toSlot, coop::net::ReliableKind::EraserPressIntent, &p, sizeof(p));
}

// HOST: a press the host did not run, told to its presser as that press sounds: the button and the deny.
void Refuse(coop::net::Session& s, uint8_t slot, uint32_t driveEid, const char* why) {
    ++g_denied;
    Send(&s, slot, driveEid, static_cast<uint8_t>(DE::Show::Refused));
    UE_LOGI("[ERASER] DENY slot=%u drive eid=%u -- %s", static_cast<unsigned>(slot), driveEid, why);
}

// HOST: one press, run or refused and then consumed (true), or left at the head of its queue (false) while the host has
// no body for the sender, or while a slot line naming that drive waits in bind_wait for the drive to bind (a press
// follows its presser's insert on the wire, and that insert can still be waiting for its drive).
bool Execute(coop::net::Session& s, uint32_t driveEid, uint8_t slot) {
    void* eraser = DE::Instance();
    if (!eraser) {
        Refuse(s, slot, driveEid, "the host's world has no eraser");
        return true;
    }
    void* named = coop::element::LivePropActor(driveEid);
    if (!named || SeatedDrive() != named) {
        if (coop::element::bind_wait::Holds(driveEid, static_cast<uint16_t>(coop::net::ReliableKind::DriveSlotState)))
            return false;
        Refuse(s, slot, driveEid, "the host's eraser does not hold that drive");
        return true;
    }
    const auto token = coop::element::IntentTarget::ForClientIntent(s, slot, kEraserReachUU);
    if (!token.HasBody()) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[ERASER] slot %u's presses wait: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return false;
    }
    if (token.Authorize(eraser).outcome != coop::element::IntentOutcome::Ok) {
        Refuse(s, slot, driveEid, "the eraser is not within its reach here");
        return true;
    }
    bool busy = false;
    if (DE::ReadProcessing(eraser, busy) && busy) {
        Refuse(s, slot, driveEid, "the eraser is still wiping");
        return true;
    }
    // The press body never reads its player; the host's own stands in for the presser.
    if (!DE::PressDelete(eraser, coop::players::Registry::Get().Local())) {
        Refuse(s, slot, driveEid, "the eraser's press could not be called");
        return true;
    }
    ++g_pressed;
    UE_LOGI("[ERASER] PRESSED the eraser for slot=%u, drive eid=%u (#%llu) -- its wipe follows in 3 s",
            static_cast<unsigned>(slot), driveEid, static_cast<unsigned long long>(g_pressed));
    return true;
}

bool IsDeletePress(const sg::Call& call) {
    static void*   sFn = nullptr;
    static int32_t sOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sOff = R::FindParamOffset(call.function, L"action");
    }
    return sOff >= 0 && call.locals && call.locals[sOff] == DE::kActionDelete;
}

// CLIENT: its own delete press with a drive seated is refused and sent; any other action runs here. HOST: every
// delete press, its own player's or one it runs for a client, noted for its POST.
sg::Verdict OnActionPre(const sg::Call& call) {
    if (!call.object || !IsDeletePress(call)) return sg::Verdict::Run;
    if (HostSession()) {
        g_pressWasBusy = false;
        DE::ReadProcessing(call.object, g_pressWasBusy);
        return sg::Verdict::Run;
    }
    auto* s = ClientSession();
    if (!s) return sg::Verdict::Run;
    void* seated = SeatedDrive();
    if (!seated) return sg::Verdict::Run;  // nothing seated: the game's own click, here
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(seated);
    if (eid == coop::element::kInvalidId) {
        // A drive the host has not bound yet: no press could name it there.
        DE::Present(call.object, DE::Show::Refused);
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= g_nextUnboundSayMs) {
            g_nextUnboundSayMs = now + kSayEveryMs;
            UE_LOGI("[ERASER] a delete press refused here: the seated drive is not known to the host yet");
        }
        return sg::Verdict::Cancel;
    }
    if (!Send(s, 0, static_cast<uint32_t>(eid), kEvIntent)) {
        UE_LOGW("[ERASER] a delete press was not sent (the session refused it); the drive stays as the host has it");
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    UE_LOGI("[ERASER] CLIENT SENT a delete press for drive eid=%u (#%llu)", static_cast<unsigned>(eid),
            static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

// HOST: the press ran; every client shows it, the presser too (its own was refused), as a start or a lone click. The
// call's own frame says it was a delete press, so a POST the gate skipped leaves nothing for the next one.
void OnActionPost(const sg::Call& call) {
    auto* s = HostSession();
    if (!s || !call.object || !IsDeletePress(call)) return;
    bool busy = false;
    const bool started = !g_pressWasBusy && DE::ReadProcessing(call.object, busy) && busy;
    const bool sent = Send(s, -1, 0, static_cast<uint8_t>(started ? DE::Show::Start : DE::Show::Click));
    UE_LOGI("[ERASER] the host's press %s, its show %s", started ? "started a wipe" : "was a click",
            sent ? "sent to every client" : "NOT sent (the session refused it)");
}

int32_t EntryOf(const sg::Call& call) {
    static void*   sFn = nullptr;
    static int32_t sOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sOff = R::FindParamOffset(call.function, L"EntryPoint");
    }
    int32_t entry = -1;
    if (sOff >= 0 && call.locals) std::memcpy(&entry, call.locals + sOff, sizeof(entry));
    return entry;
}

// HOST: the press's resume after its 3 s: a wipe clears `processing`, a deny leaves it set.
void OnResumePost(const sg::Call& call) {
    auto* s = HostSession();
    if (!s || !call.object || EntryOf(call) != DE::kResumeEntry) return;
    bool busy = true;
    DE::ReadProcessing(call.object, busy);
    const bool sent = Send(s, -1, 0, static_cast<uint8_t>(busy ? DE::Show::Deny : DE::Show::Done));
    UE_LOGI("[ERASER] the host's press resumed as %s, its show %s", busy ? "a deny" : "a wipe",
            sent ? "sent to every client" : "NOT sent (the session refused it)");
}

void InstallWatches() {
    if (!g_actionWatched)
        g_actionWatched = sg::WatchName(kRowActionOptionIndex, kTagEraser, &OnActionPre, &OnActionPost);
    if (!g_resumeWatched)
        g_resumeWatched = sg::WatchName(kRowUbergraph, kTagResume, nullptr, &OnResumePost);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallWatches();
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        InstallWatches();
        if (g_actionWatched && g_resumeWatched &&
            sg::NameWatchLive(kRowActionOptionIndex, kTagEraser) &&
            sg::NameWatchLive(kRowUbergraph, kTagResume)) {
            g_settled = true;
            UE_LOGI("[ERASER] the eraser's press and resume gates are live");
        } else if (g_actionWatched && g_resumeWatched &&
                   (sg::NameWatchSettled(kRowActionOptionIndex, kTagEraser) ||
                    sg::NameWatchSettled(kRowUbergraph, kTagResume))) {
            g_settled = true;
            UE_LOGE("[ERASER] an eraser gate is dead -- a client's delete press runs on the client alone");
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kPressBudget, coop::active_drive::NowMs, [&](const uint32_t& driveEid) {
            if (!Execute(session, driveEid, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;
            return coop::net::Served::Done;
        });
    }
}

void OnEraserPressIntent(coop::net::Session& session, const coop::net::EraserPressIntentPayload& payload,
                         uint8_t senderSlot) {
    if (session.role() == coop::net::Role::Client) {
        // The host's press, shown by this peer's own eraser.
        if (senderSlot != 0 || payload.event == kEvIntent || payload.event > static_cast<uint8_t>(DE::Show::Refused))
            return;
        void* eraser = DE::Instance();
        const bool shown = eraser && DE::Present(eraser, static_cast<DE::Show>(payload.event));
        if (shown) ++g_shown;
        UE_LOGI("[ERASER] the host's eraser event %u %s", static_cast<unsigned>(payload.event),
                shown ? "shown by this eraser" : eraser ? "NOT shown: a component or its verb did not resolve"
                                                        : "NOT shown: this world has no eraser");
        return;
    }
    if (payload.event != kEvIntent || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    uint32_t driveEid = payload.driveEid;
    if (!sl.queue.Push(std::move(driveEid), kMaxPending)) {
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kSayEveryMs;
            UE_LOGW("[ERASER] slot %u queue full (%zu) -- refusing presses until it drains",
                    static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        ++g_denied;
    }
}

void OnDisconnect() {
    if (g_sent || g_pressed || g_denied || g_shown)
        UE_LOGI("[ERASER] session end -- sent=%llu pressed=%llu denied=%llu shown=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_pressed),
                static_cast<unsigned long long>(g_denied), static_cast<unsigned long long>(g_shown));
    g_sent = g_pressed = g_denied = g_shown = 0;
    g_pressWasBusy = false;
}

uint64_t SentCount() { return g_sent; }
uint64_t PressedCount() { return g_pressed; }
uint64_t ShownCount() { return g_shown; }

}  // namespace coop::eraser_press_intent
