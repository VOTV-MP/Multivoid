// coop/world/cover_sync.cpp -- see coop/world/cover_sync.h.
//
// analogDScreenTest_C's plates, from its bytecode (0.9.0n). `panels` holds the four plate meshes (0 top left, 1 bottom
// left, 2 top right, which "does not move", 3 bottom right), `panelsState` whether each is on and `panelsUnscrew`
// whether each is unbolted. playerUsedOn with a tool on a plate starts a timed unbolt (the ui_linearProgress widget,
// the sawSound timer), whose end runs `unscrewPanel`: the looked-at plate's bolt flipped, `updPanels`, `resetUnscrew`.
// actionOptionIndex on an unbolted plate that is on, while no plate is being set back (`panelsProcess`): that plate
// off, `panelTakeoff`, its prop class spawned at the plate's transform and handed to the player's pickupObjectDirect,
// `updPanels`, `soundDetachPanel`. ReceiveActorBeginOverlap, while neither `panelTakeoff` nor `panelsProcess`: a plate
// prop of the top left, bottom left or bottom right class touching its lid sets that plate on, `updPanels`, destroys
// the prop and slides the lid home (`panelsProcess` while it moves). ReceiveActorEndOverlap ends `panelTakeoff`.
// `updPanels` shows each plate by its state, sets its collision, and tilts an unbolted one 5 degrees. gatherData and
// setData keep both arrays in the desk's save record.

// generator_C's door: actionOptionIndex, action 4 while `lookAtDoor`: `openDoor(!opened)`, which, unless the door
// already `moving`, plays or reverses doorAnim_TL; its end writes `opened` and the repair panel's collision.

#include "coop/world/cover_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load runs natively
#include "coop/text/utf8_codec.h"
#include "coop/world/cover_judge.h"
#include "coop/world/cover_shared.h"
#include "l10n/l10n.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/analog_desk.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/votv_lib.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace coop::cover_sync {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace OI  = ue_wrap::object_index;
namespace sg  = ue_wrap::script_gate;
namespace CC  = ue_wrap::component_calls;
namespace GEN = ue_wrap::generator;
namespace AD  = ue_wrap::analog_desk;
namespace EL  = coop::element;
namespace N   = coop::net;

UE_FACT_FN(kRowUnscrew, "cover_sync", L"analogDScreenTest_C", L"unscrewPanel", Declared);
UE_FACT_FN(kRowDeskAction, "cover_sync", L"analogDScreenTest_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowDeskOverlap, "cover_sync", L"analogDScreenTest_C", L"ReceiveActorBeginOverlap", Declared);
UE_FACT_FN(kRowStationAction, "cover_sync", L"generator_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowUpdPanels, "cover_sync", L"analogDScreenTest_C", L"updPanels", Declared);
UE_FACT_FN(kRowDoorEnd, "cover_sync", L"generator_C", L"doorAnim_TL__FinishedFunc", Declared);
constexpr int kTagUnscrew = 0x43565201, kTagDeskAction = 0x43565202, kTagDeskOverlap = 0x43565203,
              kTagStationAction = 0x43565204, kTagUpdPanels = 0x43565205, kTagDoorEnd = 0x43565206;  // 'CVR' 1..6

static_assert(N::kCoverDeskPanels == AD::kPlates, "the wire's plate count is the desk's");
constexpr uint8_t  kFixedPlate = AD::kFixedPlate;
constexpr uint8_t  kActionUse = 4;        // EInteractionActions: the E press
constexpr uint8_t  kHintRefusal = 1;      // addHint's type for a refusal, as the game's own refusals use
constexpr uint64_t kAskEveryMs = 250;     // this client's asks of the host, one per this
constexpr uint64_t kPutBackPollMs = 100;  // the put-back's lid touch, looked for this often while a plate is off

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;
bool g_applying = false;  // this lane's own calls into the desk or a station: the gate lets them run

}  // namespace

coop::net::Session* Session() { return g_session; }
bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

bool WritePlates(void* desk, uint8_t bits) {
    if (!AD::WritePlateArrays(desk, bits)) return false;
    g_applying = true;
    AD::CallUpdPanels(desk);
    g_applying = false;
    return true;
}

void SoundDetach(void* desk, const ue_wrap::FVector& at) {
    g_applying = true;
    AD::CallSoundDetach(desk, at);
    g_applying = false;
}

void OpenDoor(void* gen, bool open) {
    g_applying = true;
    GEN::CallOpenDoor(gen, open);
    g_applying = false;
}

bool Applying() { return g_applying; }

bool OpenedOr(void* gen, bool fallback) {
    bool v = fallback;
    return GEN::ReadOpened(gen, v) ? v : fallback;
}

bool MovingOr(void* gen, bool fallback) {
    bool v = fallback;
    return GEN::ReadMoving(gen, v) ? v : fallback;
}

bool AwayFromPlate(void* desk, uint8_t plate, void* prop) {
    constexpr float kArmAwayCm = 80.f;  // a held prop is armed once this far from its plate, past its fall off the lid
    void* const mesh = AD::PlateMesh(desk, plate);
    ue_wrap::FVector at{};
    return mesh && E::TryGetActorLocation(prop, at) &&
           Dist2(at, E::GetComponentLocation(mesh)) > kArmAwayCm * kArmAwayCm;
}

namespace {

bool LookAtDoor(void* gen) {
    bool v = false;
    return GEN::ReadLookAtDoor(gen, v) && v;
}

// ---- the client ----------------------------------------------------------------------------------------------------
void Ask(uint8_t op, uint8_t what, uint8_t index, uint8_t bits, uint32_t eid);

uint64_t g_lastPutBackAskMs = 0;
// The put-back last asked and not yet answered: its plate and prop, and the prop whose refusal the player was last shown
// (a prop left on its lid is asked again every second; its refusal is shown once, not each time).
bool g_putBackPending = false;
uint8_t g_putBackPlate = 0;
uint32_t g_putBackEid = 0;
uint32_t g_putBackHintedEid = 0;
uint32_t g_answers[8] = {};   // the drill's read: op 5 answers seen, by CoverReason
uint32_t g_lastTakeOffEid = 0;
uint32_t g_clientPlateDestroys = 0;
uint32_t g_deskStatesApplied = 0;

// Whether this player's hand grabs or holds one of `props`.
bool HeldHere(const std::vector<void*>& props) {
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(coop::players::Registry::Get().Local(), gs)) return false;
    for (void* p : props)
        if (p && (p == gs.grabbingActor || p == gs.holdingActor)) return true;
    return false;
}
bool g_armed[N::kCoverDeskPanels] = {};  // a plate's prop has been seen off its lid since the plate came off

// A plate prop touching the lid of a plate that is off, here: asked of the host, which sets it back on its desk. The
// desk's own set-back never runs on a client (its overlap is cancelled at the gate), so a copy born or carried there
// is never eaten.
// This is a poll because nothing announces the touch: measured, a held plate prop overlaps the desk once, at its body, and
// never again at the lid, whose overlap the engine reports to a native delegate no seam sees. It runs only while a plate is
// off (TickClient) and, for a plate, in two steps: a prop seen off its lid in this player's hand arms the plate (as the
// desk's panelTakeoff waits for the prop's end-overlap), and only an armed plate looks for the touch. The arming ends
// with the plate's return (OnState) or the prop's end. The class is one lookup by name (no object walk, a miss too).
void AskPutBacks(void* desk, uint8_t bits) {
    const uint64_t now = NowMs();
    for (int i = 0; i < N::kCoverDeskPanels; ++i) {
        if (PlateOn(bits, i) || i == kFixedPlate) continue;
        void* const lid = AD::Lid(desk, i);
        void* const cls = OI::ClassByName(AD::PlatePropClass(i));
        if (!lid || !cls) continue;
        std::vector<void*> props;
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &props);
        if (props.empty()) {
            g_armed[i] = false;
            continue;
        }
        if (g_armed[i]) {
            if (now - g_lastPutBackAskMs < 1000) continue;
            for (void* prop : props) {
                const EL::ElementId eid = EL::Registry::Get().EidForActor(prop);
                if (eid == EL::kInvalidId || !E::IsOverlappingActor(lid, prop)) continue;
                g_lastPutBackAskMs = now;
                g_putBackPending = true;
                g_putBackPlate = static_cast<uint8_t>(i);
                g_putBackEid = static_cast<uint32_t>(eid);
                Ask(N::kCoverOpPutBack, N::kCoverDesk, static_cast<uint8_t>(i), 0, static_cast<uint32_t>(eid));
                return;
            }
            continue;
        }
        // Armed off its lid in this player's own hand, as on the host: a copy that falls off the lid before the player
        // grabs it is lifted back through it by that grab.
        if (!HeldHere(props)) continue;
        for (void* prop : props) {
            if (!AwayFromPlate(desk, static_cast<uint8_t>(i), prop)) continue;
            g_armed[i] = true;
            UE_LOGI("cover_sync: CLIENT plate %d's prop is in this player's hand, off its lid -- a touch sets it back", i);
            break;
        }
    }
}
bool g_deskPending = false;  // the host's plates arrived with no desk to write: the desk's own updPanels at its load applies them
bool g_haveDesk = false;     // the host's plates are known: the desk is kept at them
uint8_t g_deskBits = 0;
uint64_t g_nextPutBackMs = 0;
std::vector<int8_t> g_doorWanted;  // by station: -1 none, else the host's opened

// The desk written to the host's plates when it differs; `arrived` when they have just come from the host, else the desk
// moved here and is set back.
void ConvergeDesk(void* desk, bool arrived) {
    uint8_t bits = 0;
    const bool read = AD::ReadPlates(desk, bits);
    if (!read || bits != g_deskBits) {
        if (!WritePlates(desk, g_deskBits)) {
            UE_LOGW("cover_sync: CLIENT the desk's plate arrays did not take the host's on=0x%x unbolted=0x%x",
                    g_deskBits & 0xF, g_deskBits >> 4);
            return;
        }
        if (arrived)
            UE_LOGI("cover_sync: CLIENT the desk's plates on=0x%x unbolted=0x%x, as the host's", g_deskBits & 0xF,
                    g_deskBits >> 4);
        else
            UE_LOGW("cover_sync: CLIENT the desk's plates moved here to on=0x%x unbolted=0x%x -- set back to the "
                    "host's on=0x%x unbolted=0x%x", bits & 0xF, bits >> 4, g_deskBits & 0xF, g_deskBits >> 4);
    }
    if (arrived) ++g_deskStatesApplied;
}

// The one thing a client reads on a clock: the put-back's lid touch (AskPutBacks), and only while the host's plates have a
// plate off. Everything else it keeps is written when the game announces it (OnUpdPanelsPost, OnDoorFinishedPost).
void TickClient() {
    bool anyOff = false;
    for (int i = 0; i < N::kCoverDeskPanels; ++i) anyOff = anyOff || (i != kFixedPlate && !PlateOn(g_deskBits, i));
    const uint64_t now = NowMs();
    if (!g_haveDesk || !anyOff || now < g_nextPutBackMs) return;
    g_nextPutBackMs = now + kPutBackPollMs;
    if (void* desk = AD::Find()) AskPutBacks(desk, g_deskBits);
}

// Station `index`'s door swung to the host's when it differs. A door still moving is left: the timeline's end runs this
// again (OnDoorFinishedPost).
void ConvergeDoor(void* gen, size_t index) {
    bool open = false;
    if (index >= g_doorWanted.size() || g_doorWanted[index] < 0 || !GEN::ReadOpened(gen, open) || MovingOr(gen, true)) return;
    const bool want = g_doorWanted[index] == 1;
    if (open == want) return;
    OpenDoor(gen, want);
    UE_LOGI("cover_sync: CLIENT station %zu's door %hs, as the host's", index, want ? "opens" : "shuts");
}

// ---- the gate ------------------------------------------------------------------------------------------------------
bool Paced() {
    static uint64_t s_lastMs = 0;
    const uint64_t now = NowMs();
    if (now - s_lastMs < kAskEveryMs) return false;
    s_lastMs = now;
    return true;
}

void Ask(uint8_t op, uint8_t what, uint8_t index, uint8_t bits, uint32_t eid) {
    N::CoverPayload p{};
    p.op = op;
    p.what = what;
    p.index = index;
    p.bits = bits;
    p.eid = eid;
    const bool ok = g_session->SendReliable(N::ReliableKind::CoverIntent, &p, sizeof(p));
    UE_LOGI("cover_sync: CLIENT asked the host op %u of %hs %u (ok=%d)", op, what == N::kCoverDesk ? "plate" : "station",
            index, ok ? 1 : 0);
}

const char* ReasonName(uint8_t bits) {
    switch (static_cast<N::CoverReason>(bits)) {
    case N::CoverReason::Ok: return "taken";
    case N::CoverReason::OutOfReach: return "out of reach";
    case N::CoverReason::Busy: return "refused by the desk or station";
    case N::CoverReason::Already: return "already so";
    case N::CoverReason::NotHeld: return "the plate is not in this player's hold";
    case N::CoverReason::NoStation: return "no such desk or station";
    case N::CoverReason::Rate: return "asked too fast";
    }
    return "unknown";
}

// The player's own line for a refusal, in its language; null for an answer that is no refusal (taken, or already so,
// which the state the host sends already shows).
const char* RefusalLine(uint8_t bits) {
    switch (static_cast<N::CoverReason>(bits)) {
    case N::CoverReason::OutOfReach: return l10n::T("You are too far from that.");
    case N::CoverReason::Busy: return l10n::T("That is busy right now.");
    case N::CoverReason::NotHeld: return l10n::T("You are not holding that plate.");
    case N::CoverReason::NoStation: return l10n::T("That is not available right now.");
    case N::CoverReason::Rate: return l10n::T("Too many requests, try again in a moment.");
    case N::CoverReason::Ok:
    case N::CoverReason::Already: return nullptr;
    }
    return nullptr;
}

// The refusal as the game's own hint on this machine's player.
void ShowRefusal(uint8_t bits) {
    const char* const line = RefusalLine(bits);
    void* const local = coop::players::Registry::Get().Local();
    std::wstring text;
    if (line && local && coop::text::FromUtf8Strict(line, std::strlen(line), &text))
        ue_wrap::votv_lib::AddHint(text, kHintRefusal, local);
}

// The end of a client's timed unbolt: asked of the host, and the unbolt's widget taken down here.
sg::Verdict OnUnscrewPre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object) return sg::Verdict::Run;
    const int32_t plate = AD::LookingAtPanel(c.object);
    uint8_t bits = 0;
    if (plate >= 0 && plate < N::kCoverDeskPanels) {
        // The game's verb flips the plate's bolt; the ask says the state that flip makes, so the host leaves a plate that
        // is already there.
        if (AD::ReadPlates(c.object, bits))
            Ask(N::kCoverOpUnscrew, N::kCoverDesk, static_cast<uint8_t>(plate), PlateLoose(bits, plate) ? 0 : 1, 0);
        else
            UE_LOGW("cover_sync: CLIENT the desk's plates did not read -- the unbolt of plate %d is not asked", plate);
    }
    g_applying = true;
    CC::CallParamlessNamed(c.object, L"resetUnscrew");
    g_applying = false;
    return sg::Verdict::Cancel;
}

// A client's E press on an unbolted plate that is on: a take-off, asked of the host. Every other press runs.
sg::Verdict OnDeskActionPre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object) return sg::Verdict::Run;
    const int32_t plate = AD::LookingAtPanel(c.object);
    uint8_t bits = 0;
    if (plate < 0 || plate >= N::kCoverDeskPanels || AD::ProcessOr(c.object, true) ||
        !AD::ReadPlates(c.object, bits) || !PlateLoose(bits, plate) || !PlateOn(bits, plate))
        return sg::Verdict::Run;
    if (Paced()) Ask(N::kCoverOpTakeOff, N::kCoverDesk, static_cast<uint8_t>(plate), 0, 0);
    return sg::Verdict::Cancel;
}

// A plate touching its place on a client's desk: the desk's own set-back never runs here, whatever this lane is applying
// (g_applying exempts only the lane's own writes to the desk, never an overlap: a host prop mirror born on a lid while the
// plates are being written would otherwise be set back and destroyed on this copy). The client's put-back is asked of the
// host by AskPutBacks, once the plate's prop has left its lid and comes back to it. On the host, a granted take-off's prop
// is not set back while it is fresh (TendFresh).
sg::Verdict OnDeskOverlapPre(const sg::Call& c) {
    if (IsClient()) return sg::Verdict::Cancel;
    if (g_applying) return sg::Verdict::Run;
    if (!IsHost() || !cover_judge::AnyFresh() || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"OtherActor");
    void* const other = off >= 0 ? *reinterpret_cast<void* const*>(c.locals + off) : nullptr;
    if (!cover_judge::IsFresh(other)) return sg::Verdict::Run;
    UE_LOGI("cover_sync: HOST a fresh plate prop touched its lid before leaving it -- not set back");
    return sg::Verdict::Cancel;
}

// The game's own updPanels, after it ran: it follows every plate change, the host judge's writes and a client's applied
// ones included, and the game calls it when the desk loads. The host sends the plates it now shows; a client keeps its desk
// at the host's (a state that arrived before the desk existed is written now; a write the lane did not make is undone).
void OnUpdPanelsPost(const sg::Call& c) {
    if (!c.object) return;
    if (IsHost()) {
        cover_judge::OnDeskUpdated(c.object);
        return;
    }
    if (!IsClient() || g_applying || !g_haveDesk) return;
    const bool arrived = g_deskPending;
    g_deskPending = false;
    ConvergeDesk(c.object, arrived);
}

// A station's door timeline ended, where `opened` changes: the host sends the state; a client brings the door to the
// host's if a state came while it moved.
void OnDoorFinishedPost(const sg::Call& c) {
    if (!c.object) return;
    if (IsHost()) {
        cover_judge::OnDoorFinished(c.object);
        return;
    }
    if (!IsClient() || g_applying) return;
    const int32_t index = GEN::IndexOf(c.object);
    if (index >= 0) ConvergeDoor(c.object, static_cast<size_t>(index));
}

// A client's E press on a station's door: asked of the host. Every other press runs.
sg::Verdict OnStationActionPre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"action");
    if (off < 0 || c.locals[off] != kActionUse || !LookAtDoor(c.object)) return sg::Verdict::Run;
    // A client's own world load runs natively: only a press in the announced world is the host's to judge.
    if (!coop::net_pump::IsInAnnouncedWorld(c.object)) return sg::Verdict::Run;
    const int32_t index = GEN::IndexOf(c.object);
    if (index < 0 || index > 255) {  // a station the game mode's list does not name: never a swing no host will follow
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("cover_sync: CLIENT station %ls is not in the game mode's list (index %d) -- its door press is refused",
                    R::ToString(R::NameOf(c.object)).c_str(), index);
        }
        ShowRefusal(static_cast<uint8_t>(N::CoverReason::NoStation));
        return sg::Verdict::Cancel;
    }
    if (Paced()) Ask(N::kCoverOpDoor, N::kCoverStation, static_cast<uint8_t>(index), OpenedOr(c.object, false) ? 0 : 1, 0);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    cover_judge::Install();
    if (g_watched) return;
    g_watched = true;
    const bool u = sg::WatchName(kRowUnscrew, kTagUnscrew, &OnUnscrewPre, nullptr);
    const bool a = sg::WatchName(kRowDeskAction, kTagDeskAction, &OnDeskActionPre, nullptr);
    const bool o = sg::WatchName(kRowDeskOverlap, kTagDeskOverlap, &OnDeskOverlapPre, nullptr);
    const bool s = sg::WatchName(kRowStationAction, kTagStationAction, &OnStationActionPre, nullptr);
    const bool p = sg::WatchName(kRowUpdPanels, kTagUpdPanels, nullptr, &OnUpdPanelsPost);
    const bool d = sg::WatchName(kRowDoorEnd, kTagDoorEnd, nullptr, &OnDoorFinishedPost);
    if (!u || !a || !o || !s || !p || !d)
        UE_LOGE("cover_sync: a watch was refused (unscrew=%d desk=%d overlap=%d station=%d updPanels=%d doorEnd=%d)",
                u ? 1 : 0, a ? 1 : 0, o ? 1 : 0, s ? 1 : 0, p ? 1 : 0, d ? 1 : 0);
}

void Tick() {
    if (IsHost() && g_session->connected()) cover_judge::Tick();
    else if (IsClient()) TickClient();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowUnscrew, kTagUnscrew) || !sg::NameWatchSettled(kRowDeskAction, kTagDeskAction) ||
        !sg::NameWatchSettled(kRowDeskOverlap, kTagDeskOverlap) ||
        !sg::NameWatchSettled(kRowStationAction, kTagStationAction) || !sg::NameWatchSettled(kRowUpdPanels, kTagUpdPanels) ||
        !sg::NameWatchSettled(kRowDoorEnd, kTagDoorEnd))
        return;
    g_settled = true;
    const bool u = sg::NameWatchLive(kRowUnscrew, kTagUnscrew), a = sg::NameWatchLive(kRowDeskAction, kTagDeskAction),
               o = sg::NameWatchLive(kRowDeskOverlap, kTagDeskOverlap),
               s = sg::NameWatchLive(kRowStationAction, kTagStationAction),
               p = sg::NameWatchLive(kRowUpdPanels, kTagUpdPanels), d = sg::NameWatchLive(kRowDoorEnd, kTagDoorEnd);
    if (u && a && o && s && p && d)
        UE_LOGI("cover_sync: the watches on the desk's plates and the stations' door are live");
    else UE_LOGE("cover_sync: a watch is dead (unscrew=%d desk=%d overlap=%d station=%d updPanels=%d doorEnd=%d) -- the "
                 "covers are not kept", u ? 1 : 0, a ? 1 : 0, o ? 1 : 0, s ? 1 : 0, p ? 1 : 0, d ? 1 : 0);
}

void SendJoinSnapshotForSlot(int slot) { cover_judge::SendJoinSnapshotForSlot(slot); }

void OnIntent(int senderSlot, const coop::net::CoverPayload& p) { cover_judge::OnIntent(senderSlot, p); }

void OnState(const coop::net::CoverPayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kCoverOpAnswer) {
        if (p.bits < sizeof(g_answers) / sizeof(g_answers[0])) ++g_answers[p.bits];
        if (p.eid && p.bits == static_cast<uint8_t>(N::CoverReason::Ok)) g_lastTakeOffEid = p.eid;
        if (p.eid && p.index < N::kCoverDeskPanels)
            UE_LOGI("cover_sync: CLIENT the host took plate %u off -- its prop eid %u lies where the plate was", p.index, p.eid);
        else UE_LOGI("cover_sync: CLIENT the host answered this player's ask of %hs %u: %hs",
                     p.what == N::kCoverDesk ? "plate" : "station", p.index, ReasonName(p.bits));
        // The answer to the put-back asked a moment ago: a refusal for a prop already shown it is not shown again.
        const bool putBack = g_putBackPending && p.what == N::kCoverDesk && p.index == g_putBackPlate;
        if (putBack) g_putBackPending = false;
        const bool seen = putBack && RefusalLine(p.bits) && g_putBackHintedEid == g_putBackEid;
        if (!seen) ShowRefusal(p.bits);
        if (putBack && RefusalLine(p.bits)) g_putBackHintedEid = g_putBackEid;
        return;
    }
    if (p.op != N::kCoverOpState) return;
    if (p.what == N::kCoverDesk) {
        g_deskBits = p.bits;
        g_haveDesk = true;
        for (int i = 0; i < N::kCoverDeskPanels; ++i) {
            if (!PlateOn(p.bits, i)) continue;
            g_armed[i] = false;  // a plate set back is armed afresh when it next comes off
            if (g_putBackPending && g_putBackPlate == i) g_putBackPending = false;  // its put-back was taken
        }
        if (void* const desk = AD::Find()) ConvergeDesk(desk, true);
        else g_deskPending = true;
    } else if (p.what == N::kCoverStation) {
        if (g_doorWanted.size() <= p.index) g_doorWanted.resize(static_cast<size_t>(p.index) + 1, -1);
        g_doorWanted[p.index] = (p.bits & 1u) ? 1 : 0;
        if (void* const gen = GEN::At(p.index)) ConvergeDoor(gen, p.index);
    }
}

uint32_t AnswersSeen(uint8_t reason) { return reason < sizeof(g_answers) / sizeof(g_answers[0]) ? g_answers[reason] : 0; }

uint32_t LastTakeOffEid() { return g_lastTakeOffEid; }

uint32_t DeskStatesApplied() { return g_deskStatesApplied; }

bool PlateArmed(uint8_t plate) { return plate < N::kCoverDeskPanels && g_armed[plate]; }

void NoteBroadcastDestroy(void* actor) {
    // The drill's reading only: a production destroy never pays the class-name compare.
    static const bool s_drill = coop::config::ResolveEnum(coop::config_registry::rows::cover_drill) != "off";
    if (s_drill && IsClient() && AD::PlateOfProp(actor) >= 0) {
        ++g_clientPlateDestroys;
        UE_LOGW("cover_sync: CLIENT broadcast the destroy of a plate prop it did not author (#%u)", g_clientPlateDestroys);
    }
}

uint32_t ClientPlateDestroys() { return g_clientPlateDestroys; }

bool DrillAsk(uint8_t op, uint8_t what, uint8_t index, uint8_t bits, uint32_t eid) {
    if (!IsClient()) return false;
    Ask(op, what, index, bits, eid);
    return true;
}

void OnDisconnect() {
    cover_judge::OnDisconnect();
    g_deskPending = false;
    g_haveDesk = false;
    for (bool& a : g_armed) a = false;
    g_doorWanted.clear();
    g_putBackPending = false;
    g_putBackHintedEid = 0;
}

}  // namespace coop::cover_sync
