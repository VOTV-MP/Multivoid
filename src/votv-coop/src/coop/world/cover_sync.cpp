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

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/world/cover_judge.h"
#include "coop/world/cover_shared.h"
#include "ue_wrap/core/cached_obj_ref.h"
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

#include <chrono>
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
constexpr int kTagUnscrew = 0x43565201, kTagDeskAction = 0x43565202, kTagDeskOverlap = 0x43565203,
              kTagStationAction = 0x43565204;  // 'CVR' 1..4

static_assert(N::kCoverDeskPanels == AD::kPlates, "the wire's plate count is the desk's");
constexpr uint8_t  kFixedPlate = AD::kFixedPlate;
constexpr uint8_t  kActionUse = 4;        // EInteractionActions: the E press

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

void WritePlates(void* desk, uint8_t bits) {
    if (!AD::WritePlateArrays(desk, bits)) return;
    g_applying = true;
    AD::CallUpdPanels(desk);
    g_applying = false;
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
// desk's own set-back never runs on a client (panelTakeoff is held), so a copy born or carried there is never eaten.
void AskPutBacks(void* desk, uint8_t bits) {
    const uint64_t now = NowMs();
    for (int i = 0; i < N::kCoverDeskPanels; ++i) {
        if (PlateOn(bits, i) || i == kFixedPlate) {
            g_armed[i] = false;  // as the desk's panelTakeoff: a prop born on its lid is not set back until it has left
            continue;
        }
        void* const lid = AD::Lid(desk, i);
        void* const cls = R::FindClass(AD::PlatePropClass(i));
        if (!lid || !cls) continue;
        std::vector<void*> props;
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &props);
        bool touching = false;
        for (void* prop : props) {
            const EL::ElementId eid = EL::Registry::Get().EidForActor(prop);
            if (eid == EL::kInvalidId || !E::IsOverlappingActor(lid, prop)) continue;
            touching = true;
            if (!g_armed[i] || now - g_lastPutBackAskMs < 1000) break;
            g_lastPutBackAskMs = now;
            Ask(N::kCoverOpPutBack, N::kCoverDesk, static_cast<uint8_t>(i), 0, static_cast<uint32_t>(eid));
            return;
        }
        // Armed off its lid in this player's own hand, as on the host: a copy that falls off the lid before the player
        // grabs it is lifted back through it by that grab.
        bool away = false;
        for (void* prop : props) away = away || AwayFromPlate(desk, static_cast<uint8_t>(i), prop);
        if (!touching && away && HeldHere(props) && !g_armed[i]) {
            g_armed[i] = true;
            UE_LOGI("cover_sync: CLIENT plate %d's prop is in this player's hand, off its lid -- a touch sets it back", i);
        }
    }
}
bool g_deskWanted = false;   // the host's plates, held until the desk is read
bool g_haveDesk = false;     // the host's plates are known: the desk is kept at them
uint8_t g_deskBits = 0;
uint64_t g_nextDeskCheckMs = 0;
std::vector<int8_t> g_doorWanted;  // by station: -1 none, else the host's opened

void TickClient() {
    // The desk's own set-back returns at once while panelTakeoff is set: held every frame on a client, so a plate
    // prop born or carried onto its lid here is never eaten by this copy.
    static ue_wrap::CachedObjRef s_desk;
    void* held = s_desk.Get();
    if (!held) {
        held = AD::Find();
        s_desk.Set(held);
    }
    if (held) AD::WriteTakeoff(held, true);
    // The desk is kept at the host's plates: a write when they arrive, and a check every 100 ms after, which rewrites
    // anything that moved them here.
    const uint64_t now = NowMs();
    if ((g_deskWanted || (g_haveDesk && now >= g_nextDeskCheckMs))) {
        g_nextDeskCheckMs = now + kScanEveryMs;
        if (void* desk = AD::Find()) {
            uint8_t bits = 0;
            const bool read = AD::ReadPlates(desk, bits);
            if (!read || bits != g_deskBits) {
                WritePlates(desk, g_deskBits);
                if (g_deskWanted)
                    UE_LOGI("cover_sync: CLIENT the desk's plates on=0x%x unbolted=0x%x, as the host's", g_deskBits & 0xF,
                            g_deskBits >> 4);
                else
                    UE_LOGW("cover_sync: CLIENT the desk's plates moved here to on=0x%x unbolted=0x%x -- set back to the "
                            "host's on=0x%x unbolted=0x%x", bits & 0xF, bits >> 4, g_deskBits & 0xF, g_deskBits >> 4);
            }
            g_deskWanted = false;
            AskPutBacks(desk, g_deskBits);
        }
    }
    for (size_t i = 0; i < g_doorWanted.size(); ++i) {
        if (g_doorWanted[i] < 0) continue;
        void* const gen = GEN::At(static_cast<int>(i));
        bool open = false;
        if (!gen || !GEN::ReadOpened(gen, open) || MovingOr(gen, true)) continue;
        const bool want = g_doorWanted[i] == 1;
        if (open != want) {
            OpenDoor(gen, want);
            UE_LOGI("cover_sync: CLIENT station %zu's door %hs, as the host's", i, want ? "opens" : "shuts");
        }
        g_doorWanted[i] = -1;
    }
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

// A plate touching its place on a client's desk: nothing is set back here. The client's put-back is asked of the host by
// AskPutBacks, once the plate's prop has left its lid and comes back to it, as the desk's own panelTakeoff waits. On the
// host, a granted take-off's prop is not set back while it is fresh (TendFresh).
sg::Verdict OnDeskOverlapPre(const sg::Call& c) {
    if (g_applying) return sg::Verdict::Run;
    if (IsClient()) return sg::Verdict::Cancel;
    if (!IsHost() || !cover_judge::AnyFresh() || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"OtherActor");
    void* const other = off >= 0 ? *reinterpret_cast<void* const*>(c.locals + off) : nullptr;
    if (!cover_judge::IsFresh(other)) return sg::Verdict::Run;
    UE_LOGI("cover_sync: HOST a fresh plate prop touched its lid before leaving it -- not set back");
    return sg::Verdict::Cancel;
}

// A client's E press on a station's door: asked of the host. Every other press runs.
sg::Verdict OnStationActionPre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"action");
    if (off < 0 || c.locals[off] != kActionUse || !LookAtDoor(c.object)) return sg::Verdict::Run;
    const int32_t index = GEN::IndexOf(c.object);
    if (index < 0 || index > 255) return sg::Verdict::Run;
    if (Paced()) Ask(N::kCoverOpDoor, N::kCoverStation, static_cast<uint8_t>(index), OpenedOr(c.object, false) ? 0 : 1, 0);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    const bool u = sg::WatchName(kRowUnscrew, kTagUnscrew, &OnUnscrewPre, nullptr);
    const bool a = sg::WatchName(kRowDeskAction, kTagDeskAction, &OnDeskActionPre, nullptr);
    const bool o = sg::WatchName(kRowDeskOverlap, kTagDeskOverlap, &OnDeskOverlapPre, nullptr);
    const bool s = sg::WatchName(kRowStationAction, kTagStationAction, &OnStationActionPre, nullptr);
    if (!u || !a || !o || !s)
        UE_LOGE("cover_sync: a watch was refused (unscrew=%d desk=%d overlap=%d station=%d)", u ? 1 : 0, a ? 1 : 0,
                o ? 1 : 0, s ? 1 : 0);
}

void Tick() {
    if (IsHost() && g_session->connected()) cover_judge::Tick();
    else if (IsClient()) TickClient();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowUnscrew, kTagUnscrew) || !sg::NameWatchSettled(kRowDeskAction, kTagDeskAction) ||
        !sg::NameWatchSettled(kRowDeskOverlap, kTagDeskOverlap) ||
        !sg::NameWatchSettled(kRowStationAction, kTagStationAction))
        return;
    g_settled = true;
    const bool u = sg::NameWatchLive(kRowUnscrew, kTagUnscrew), a = sg::NameWatchLive(kRowDeskAction, kTagDeskAction),
               o = sg::NameWatchLive(kRowDeskOverlap, kTagDeskOverlap),
               s = sg::NameWatchLive(kRowStationAction, kTagStationAction);
    if (u && a && o && s) UE_LOGI("cover_sync: the watches on the desk's plates and the stations' door press are live");
    else UE_LOGE("cover_sync: a watch is dead (unscrew=%d desk=%d overlap=%d station=%d) -- a client's cover runs its own",
                 u ? 1 : 0, a ? 1 : 0, o ? 1 : 0, s ? 1 : 0);
}

void SendJoinSnapshotForSlot(int slot) { cover_judge::SendJoinSnapshotForSlot(slot); }

void OnIntent(int senderSlot, const coop::net::CoverPayload& p) { cover_judge::OnIntent(senderSlot, p); }

void OnState(const coop::net::CoverPayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kCoverOpAnswer) {
        if (p.eid && p.index < N::kCoverDeskPanels)
            UE_LOGI("cover_sync: CLIENT the host took plate %u off -- its prop eid %u lies where the plate was", p.index, p.eid);
        else UE_LOGI("cover_sync: CLIENT the host answered this player's ask of %hs %u: %hs",
                     p.what == N::kCoverDesk ? "plate" : "station", p.index, ReasonName(p.bits));
        return;
    }
    if (p.op != N::kCoverOpState) return;
    if (p.what == N::kCoverDesk) {
        g_deskBits = p.bits;
        g_deskWanted = true;
        g_haveDesk = true;
    } else if (p.what == N::kCoverStation) {
        if (g_doorWanted.size() <= p.index) g_doorWanted.resize(static_cast<size_t>(p.index) + 1, -1);
        g_doorWanted[p.index] = (p.bits & 1u) ? 1 : 0;
    }
}

void OnDisconnect() {
    cover_judge::OnDisconnect();
    g_deskWanted = false;
    g_haveDesk = false;
    for (bool& a : g_armed) a = false;
    g_doorWanted.clear();
}

}  // namespace coop::cover_sync
