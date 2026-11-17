// coop/interactables/woodchipper_gate.cpp -- see coop/interactables/woodchipper_gate.h.

#include "coop/interactables/woodchipper_gate.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/remote_prop.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/woodchipper.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::woodchipper_gate {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace W  = ue_wrap::woodchipper;
using Payload = coop::net::WoodchipperPayload;

// The tick is the burn (fuel falls, and at 0 the machine switches itself off); the eat box's begin-overlap is the intake
// (the fed prop is destroyed, the clump spawns). The pour and the press are the two verbs a player's hands have on it.
UE_FACT_FN(kRowTick, "woodchipper_gate", L"woodchipper_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowEat, "woodchipper_gate", L"woodchipper_C",
           L"BndEvt__eat_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowPour, "woodchipper_gate", L"woodchipper_C", L"gascanFuel", Declared);
UE_FACT_FN(kRowPress, "woodchipper_gate", L"woodchipper_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowGraph, "woodchipper_gate", L"woodchipper_C", L"ExecuteUbergraph_woodchipper", Declared);

enum Row { kTick, kEat, kPour, kPress, kGraph, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowTick, &kRowEat, &kRowPour, &kRowPress, &kRowGraph};
const char* const kRowTag[kRowCount] = {"ReceiveTick", "eat.BeginOverlap", "gascanFuel", "actionOptionIndex", "graph"};
constexpr int kTagBase = 0x57470000;   // 'WG', then the row

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_watched{false};
std::atomic<uint32_t> g_registered{0};   // bit i: row i's watch registered
bool g_settled = false;                  // game thread: every registered row settled, and any dead one said

// The drill's red arms: the tick and intake refusals off (a client's copy burns and eats on its own), and the asks off
// (the client's pour and press reach no host).
bool GateOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::woodchipper_gate_off);
    return off;
}
bool IntentOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::woodchipper_intent_off);
    return off;
}
// The red arm of the chipper feed drill, on the host: no copy is set simulating for the resume, so the game's own canChip
// refuses the driven copy as it does without the gate.
bool FlipOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::chipper_driven_flip_off);
    return off;
}

// Game thread, as the gate's callbacks are.
Counts g_counts;

// The ask of a client's player, sent to the host as the machine's intent. False when nothing was sent: the red arm, no
// running session, or a session that refused the message.
bool Ask(uint8_t op, const std::wstring& key, float amount, bool wanted) {
    if (IntentOff()) {
        ++g_counts.notSent;
        return false;
    }
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return false;
    Payload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = op;
    p.fuel = amount;
    p.active = wanted ? 1 : 0;
    if (!s->SendReliable(coop::net::ReliableKind::WoodchipperIntent, &p, sizeof(p))) {
        ++g_counts.notSent;
        UE_LOGW("woodchipper_gate: the session refused the %s on '%ls'",
                op == coop::net::kWoodchipperOpPour ? "pour" : "press", key.c_str());
        return false;
    }
    return true;
}

bool WireKeyable(const std::wstring& key) { return !key.empty() && key.size() <= sizeof(coop::net::WireKey::data); }

// ---- the tick and the intake: refused on a client ----

// The host's overlap handler, seen and not changed here (the resume that follows it is the one the gate acts on, below): what
// entered the eat box and what canChip will read of it. One line per (chipper, other actor) pair, the pairs capped, so a body
// resting in the box says nothing more.
struct EatPair {
    void* chipper;
    void* other;
};
std::vector<EatPair> g_eatPairs;
constexpr size_t kMaxEatPairs = 64;

// The key a prop carries in its own field, read without a dispatch (a script-gate callback may call it); empty for an
// actor without a key field, or a dead one.
std::wstring KeyOfActor(void* actor) {
    if (!actor || !R::IsLive(actor)) return {};
    const ue_wrap::prop::KeyField where = ue_wrap::prop::KeyFieldOf(R::ClassOf(actor));
    return where == ue_wrap::prop::KeyField::None ? std::wstring() : R::ToString(ue_wrap::prop::ReadKeyField(actor, where));
}

// The actor and the component of each chipper's latest begin-overlap, kept to the resume of its Delay(0). The resume reads
// the event's parameters, which the latest overlap wrote, while the chipper's own `ac` and `comp` fields still hold the
// previous overlap's until the resume writes them; so this is the same last-wins record, one per chipper, and the resume
// takes it.
struct Entering {
    void* chipper;
    ue_wrap::CachedObjRef actor, comp;
};
std::vector<Entering> g_entering;
constexpr size_t kMaxEntering = 8;

void StashEntering(void* chipper, void* actor, void* comp) {
    Entering* e = nullptr;
    for (Entering& x : g_entering)
        if (x.chipper == chipper) e = &x;
    if (!e) {
        if (g_entering.size() >= kMaxEntering) g_entering.erase(g_entering.begin());
        g_entering.push_back(Entering{chipper, {}, {}});
        e = &g_entering.back();
    }
    e->actor.Set(actor);
    e->comp.Set(comp);
}

void NoteHostEat(const sg::Call& call) {
    ++g_counts.eatHost;
    if (!call.locals) return;
    static R::ParamOffset sOther{L"OtherActor"}, sComp{L"OtherComp"};
    const int32_t otherOff = sOther.Of(call.function);
    const int32_t compOff = sComp.Of(call.function);
    if (otherOff < 0) return;
    void* const other = *reinterpret_cast<void* const*>(call.locals + otherOff);
    void* const comp = compOff < 0 ? nullptr : *reinterpret_cast<void* const*>(call.locals + compOff);
    StashEntering(call.object, other, comp);
    for (const EatPair& e : g_eatPairs)
        if (e.chipper == call.object && e.other == other) return;
    if (g_eatPairs.size() >= kMaxEatPairs) return;
    g_eatPairs.push_back({call.object, other});
    float fuel = 0.f;
    bool active = false;
    W::Read(call.object, fuel, active);
    const std::wstring key = KeyOfActor(other);
    UE_LOGI("woodchipper_gate: HOST intake #%llu chipper %p (active %d, fuel %.1f) was entered by %p class '%ls' key '%ls' "
            "via component %p class '%ls' (simulating %d)", static_cast<unsigned long long>(g_counts.eatHost),
            call.object, active ? 1 : 0, static_cast<double>(fuel), other, R::ClassNameOf(other).c_str(), key.c_str(),
            comp, R::ClassNameOf(comp).c_str(), ue_wrap::engine::IsComponentSimulatingPhysics(comp) ? 1 : 0);
}

sg::Verdict OnRefusePre(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    if (!coop::spawn_authority::IsActiveClientSession()) {
        if (row == kEat) NoteHostEat(call);
        return sg::Verdict::Run;
    }
    if (GateOff()) return sg::Verdict::Run;
    if (row != kTick && row != kEat) return sg::Verdict::Run;
    uint64_t& n = row == kTick ? g_counts.tickRefused : g_counts.eatRefused;
    ++n;
    if ((n & (n - 1)) == 0)
        UE_LOGI("woodchipper_gate[%s]: refuse %p (call #%llu) -- the host runs the chipper", kRowTag[row], call.object,
                static_cast<unsigned long long>(n));
    return sg::Verdict::Cancel;
}

// ---- the pour: the copy's own gascanFuel runs; the fuel it gained is asked of the host ----
// Divergence from MTA, whose client waits for the server's answer to a request (Packet_Vehicle_InOut, CGame.cpp:3018): a
// press waits below, but the pour cannot, since the can drains inside the game's own body. The copy is optimistic and the
// host's answer reverts it when the pour is refused (woodchipper_sync's Answer).

void* g_pourChipper = nullptr;
float g_pourBefore = 0.f;
std::vector<void*> g_emptyPours;   // the chippers a zero-gain pour was said for, once each

bool LocalPlayerArg(const sg::Call& call, R::ParamOffset& param) {
    const int32_t off = param.Of(call.function);
    if (off < 0 || !call.locals) return false;
    return coop::players::Registry::Get().IsLocal(*reinterpret_cast<void* const*>(call.locals + off));
}

sg::Verdict OnPourPre(const sg::Call& call) {
    g_pourChipper = nullptr;
    if (!coop::spawn_authority::IsActiveClientSession() || !W::IsChipper(call.object)) return sg::Verdict::Run;
    static R::ParamOffset sPlayer{L"player"};
    float fuel = 0.f;
    bool active = false;
    if (!LocalPlayerArg(call, sPlayer) || !W::Read(call.object, fuel, active)) return sg::Verdict::Run;
    g_pourChipper = call.object;
    g_pourBefore = fuel;
    return sg::Verdict::Run;
}

void OnPourPost(const sg::Call& call) {
    if (!g_pourChipper || call.object != g_pourChipper) return;
    g_pourChipper = nullptr;
    float fuel = 0.f;
    bool active = false;
    if (!W::Read(call.object, fuel, active)) return;
    const float gained = std::fmin(fuel - g_pourBefore, W::kMaxFuel);
    if (!(gained > 0.f)) {   // the can was empty, or the tank full: nothing went in, so nothing is asked
        if (std::find(g_emptyPours.begin(), g_emptyPours.end(), call.object) == g_emptyPours.end()) {
            g_emptyPours.push_back(call.object);
            UE_LOGI("woodchipper_gate: this player's pour into chipper %p gained %.2f (tank %.2f -> %.2f) -- nothing is "
                    "asked of the host (said once per chipper)", call.object, static_cast<double>(gained),
                    static_cast<double>(g_pourBefore), static_cast<double>(fuel));
        }
        return;
    }
    const std::wstring key = W::KeyOf(call.object);
    if (!WireKeyable(key)) {
        UE_LOGW("woodchipper_gate: this player poured %.1f into chipper %p, which holds no key the host could resolve",
                gained, call.object);
        return;
    }
    if (Ask(coop::net::kWoodchipperOpPour, key, gained, false)) {
        ++g_counts.poursAsked;
        UE_LOGI("woodchipper_gate: this player poured %.1f into '%ls' -- the host adds it (#%llu)", gained, key.c_str(),
                static_cast<unsigned long long>(g_counts.poursAsked));
    }
}

// ---- the button: refused, and asked as the state the player wanted ----

sg::Verdict OnPressPre(const sg::Call& call) {
    if (!coop::spawn_authority::IsActiveClientSession() || !W::IsChipper(call.object) || !call.locals)
        return sg::Verdict::Run;
    static R::ParamOffset sAction{L"action"}, sPlayer{L"player"}, sHit{L"hit"};
    const int32_t actionOff = sAction.Of(call.function);
    const int32_t hitOff = sHit.Of(call.function);
    static bool sSaid = false;
    if (actionOff < 0 || hitOff < 0) {
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("woodchipper_gate: the press's action or hit parameter did not resolve -- a chipper press runs on "
                    "this copy alone (said once)");
        }
        return sg::Verdict::Run;
    }
    if (call.locals[actionOff] != W::kButtonAction || !LocalPlayerArg(call, sPlayer)) return sg::Verdict::Run;
    // The game toggles only when the hit names the button's box; a press elsewhere on the machine does nothing.
    void* const button = W::ButtonComponent(call.object);
    if (!button || ue_wrap::hit_result::Component(call.locals + hitOff) != button) return sg::Verdict::Run;
    float fuel = 0.f;
    bool active = false;
    const std::wstring key = W::KeyOf(call.object);
    if (!W::Read(call.object, fuel, active) || !WireKeyable(key)) {
        UE_LOGW("woodchipper_gate: a press on chipper %p, which holds no readable key or state -- nothing sent",
                call.object);
        return sg::Verdict::Cancel;
    }
    if (Ask(coop::net::kWoodchipperOpSetActive, key, 0.f, !active)) {
        ++g_counts.pressesAsked;
        UE_LOGI("woodchipper_gate: this player pressed '%ls' %s -- the host decides (#%llu)", key.c_str(),
                active ? "off" : "on", static_cast<unsigned long long>(g_counts.pressesAsked));
    }
    return sg::Verdict::Cancel;
}

// ---- the host's intake, resumed: a copy a client drives is set simulating for the chipper's own check ----

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

// What the host's gate captured at the PRE of one resume of the overlap's Delay(0): the prop's name and identity are read
// there, while the prop lives, because the intake destroys it in that very call. The POST takes the capture by chipper and
// nesting depth, and needs nothing of the destroyed prop.
struct Resume {
    void* chipper = nullptr;
    int depth = 0;
    bool busy = false;            // `process` was already up when the resume began
    std::wstring key;             // the prop's key, or `NameOf`'s name for a prop that holds none
    Intake seen;
    ue_wrap::CachedObjRef comp;   // the copy's component, to set it back
};
std::vector<Resume> g_resumes;    // the captures of the resumes in flight, outermost first

// The drill's record of the resumes (Intake, in the header): the newest resume of each name, sixteen kept. It is read by
// the chipper feed drill alone; nothing in the product reads it, so no behaviour depends on it. Game thread.
struct Recorded {
    std::wstring key;
    Intake intake;
};
std::vector<Recorded> g_recorded;
constexpr size_t kKeepRecorded = 16;

// How many copies each chipper has had flipped this session, so the flip's line is said at the first few and at each doubling.
struct Flips {
    void* chipper;
    uint32_t n;
};
std::vector<Flips> g_flips;
constexpr size_t kMaxFlipCounts = 16;

// What a prop is called in a line and in the record: its key where it holds one, else the element id the registry gives it
// (a client-held clump has no key), else its address.
std::wstring NameOf(void* actor, const std::wstring& key) {
    if (!key.empty() && key != L"None") return key;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid != coop::element::kInvalidId && eid != 0u) return L"eid:" + std::to_wstring(static_cast<unsigned long long>(eid));
    return L"actor@" + std::to_wstring(reinterpret_cast<uintptr_t>(actor));
}

// The chipper as a line names it: its key (the pointer when it holds none).
std::wstring ChipperName(void* chipper) {
    std::wstring key = W::KeyOf(chipper);
    return key.empty() ? L"@" + std::to_wstring(reinterpret_cast<uintptr_t>(chipper)) : key;
}

void Record(const std::wstring& key, const Intake& intake) {
    for (Recorded& r : g_recorded)
        if (r.key == key) {
            r.intake = intake;
            return;
        }
    if (g_recorded.size() >= kKeepRecorded) g_recorded.erase(g_recorded.begin());
    g_recorded.push_back(Recorded{key, intake});
}

// The client slot whose hold or syncing row positions the host's copy of `actor`, -1 when none does.
int DriverOf(void* actor) {
    const int held = coop::remote_prop::DrivingSlotOf(actor);
    if (held >= 0) return held;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId || eid == 0u) return -1;
    const uint8_t syncer = coop::prop_drive_host::ClientSyncerOf(eid);
    return syncer ? static_cast<int>(syncer) : -1;
}

// The copy a resume's PRE set simulating goes back to not simulating, unless the chipper took it: a taken prop is destroyed in
// the call, so a component that still resolves is a copy that was not taken. Nothing else of the copy is touched; its drive
// (the hold's, or the thrown lane's rows) keeps positioning it.
bool RestoreFlipped(Resume& r) {
    if (!r.seen.flipped || r.seen.took) return false;
    void* const comp = r.comp.Get();
    return comp && ue_wrap::engine::SetComponentSimulatePhysics(comp, false);
}

// A capture whose POST never came (a fault the script gate's firewall absorbed unwinds past the post callback, or the
// session ended): the copy it flipped is set back all the same, and it is said once.
void DropCapture(Resume& r, const char* why) {
    if (!RestoreFlipped(r)) return;
    static bool sSaid = false;
    if (sSaid) return;
    sSaid = true;
    UE_LOGW("woodchipper_gate: %s: the copy of '%ls' that chipper '%ls' was set simulating for is set back (said once)", why,
            r.key.c_str(), ChipperName(r.chipper).c_str());
}

// The resume's PRE, on the host: the prop the latest overlap named, kept by the overlap's PRE. canChip, read in this very
// call, wants a simulating body; a copy a client drives is positioned with its simulation off, and the overlap that met it
// does not refire. So when a client's drive is what keeps it still and the chipper is on, idle and fuelled (the game would
// read the body), the copy is set simulating for this call; the POST sets it back unless the chipper took it. A chipper
// that is off, busy or empty leaves the copy alone, as it leaves any prop. The component set is the overlap's own: the
// stash holds the event's OtherComp, which the resume copies into `comp` and canChip reads as its `self2` (woodChipper
// Label_554 and Label_638). The chipper's fuel is read here, at the resume; the game read it at the overlap, so a tank that
// reached 0 between the two frames is eaten by the game's own order and left alone here. A prop is named by its key, or by
// its element id when it holds none, so a keyless client-held clump is flipped like a keyed paper.
// This flip is an interim of the tracker row MIRROR-SIMULATING-GATES (a coop copy's simulation differs from what a game
// check reads); the row's root decision on how a copy simulates retires it, with the same flip on the hacksaw's start.
void BeginResume(const sg::Call& call, bool busy) {
    // A capture of this chipper at this depth or deeper belongs to a call that is over: its POST never came.
    for (size_t i = g_resumes.size(); i-- > 0;)
        if (g_resumes[i].chipper == call.object && g_resumes[i].depth >= call.depth) {
            UE_LOGW("woodchipper_gate: the resume of chipper '%ls' that captured '%ls' left no POST -- its outcome is lost",
                    ChipperName(call.object).c_str(), g_resumes[i].key.c_str());
            DropCapture(g_resumes[i], "a resume left no POST");
            g_resumes.erase(g_resumes.begin() + static_cast<std::ptrdiff_t>(i));
        }
    size_t at = 0;
    while (at < g_entering.size() && g_entering[at].chipper != call.object) ++at;
    if (at == g_entering.size()) return;
    Entering taken = std::move(g_entering[at]);
    g_entering.erase(g_entering.begin() + static_cast<std::ptrdiff_t>(at));
    void* const actor = taken.actor.Get();
    void* const comp = taken.comp.Get();
    if (!actor) return;
    std::wstring key = NameOf(actor, KeyOfActor(actor));
    float fuel = 0.f;
    bool active = false;
    if (!W::Read(call.object, fuel, active)) return;
    Intake seen;
    seen.slot = DriverOf(actor);
    seen.simulating = comp && ue_wrap::engine::IsComponentSimulatingPhysics(comp);
    seen.ready = active && !busy && fuel > 0.f;
    if (seen.slot >= 0 && !seen.simulating && seen.ready && !FlipOff() && ue_wrap::engine::SetComponentSimulatePhysics(comp, true)) {
        seen.flipped = true;
        uint32_t* count = nullptr;   // per chipper: the first few flips are said, then at each doubling
        for (Flips& f : g_flips)
            if (f.chipper == call.object) count = &f.n;
        if (!count) {
            if (g_flips.size() >= kMaxFlipCounts) g_flips.erase(g_flips.begin());
            g_flips.push_back(Flips{call.object, 0});
            count = &g_flips.back().n;
        }
        const uint32_t n = ++*count;
        if (n <= 3 || (n & (n - 1)) == 0)
            UE_LOGI("woodchipper_gate: HOST flipped '%ls' (%ls) simulating for chipper '%ls's intake -- slot %d drives its copy; "
                    "the chipper's own canChip judges it (flip #%u of this chipper, said at 1-3 and at each doubling)",
                    key.c_str(), R::ClassNameOf(actor).c_str(), ChipperName(call.object).c_str(), seen.slot,
                    static_cast<unsigned>(n));
    }
    Resume r;
    r.chipper = call.object;
    r.depth = call.depth;
    r.busy = busy;
    r.key = std::move(key);
    r.seen = seen;
    r.comp.Set(comp);
    g_resumes.push_back(std::move(r));
}

// The resume's POST: the copy the PRE set simulating goes back to not simulating unless the chipper took it (its
// `process` rose and the prop was destroyed in that call), so a refusal leaves the client's drive as it was. The outcome
// is recorded from the PRE's capture and said.
void EndResume(const sg::Call& call) {
    size_t at = g_resumes.size();
    while (at > 0 && !(g_resumes[at - 1].chipper == call.object && g_resumes[at - 1].depth == call.depth)) --at;
    if (at == 0) return;
    Resume r = std::move(g_resumes[at - 1]);
    g_resumes.erase(g_resumes.begin() + static_cast<std::ptrdiff_t>(at - 1));
    bool process = false;
    void* lastActor = nullptr;
    void* lastComp = nullptr;
    const bool took = W::ReadIntake(call.object, process, lastActor, lastComp) && !r.busy && process;
    r.seen.took = took;
    RestoreFlipped(r);
    Record(r.key, r.seen);
    if (r.seen.slot < 0) return;   // a prop no client drives is the game's own business
    static uint64_t sRefused = 0;
    if (took) {
        UE_LOGI("woodchipper_gate: HOST chipper '%ls' took '%ls' -- slot %d drove its copy (flipped %d)",
                ChipperName(call.object).c_str(), r.key.c_str(), r.seen.slot, r.seen.flipped ? 1 : 0);
        return;
    }
    if ((++sRefused & (sRefused - 1)) == 0)
        UE_LOGI("woodchipper_gate: HOST chipper '%ls' did not take '%ls' -- slot %d drives its copy (simulating=%d at the "
                "resume, flipped %d, chipper ready %d), left as it was (said at 1, 2, 4, ... such refusals)",
                ChipperName(call.object).c_str(), r.key.c_str(), r.seen.slot, r.seen.simulating ? 1 : 0,
                r.seen.flipped ? 1 : 0, r.seen.ready ? 1 : 0);
}

sg::Verdict OnGraphPre(const sg::Call& call) {
    if (coop::spawn_authority::IsActiveClientSession() || EntryOf(call) != W::kIntakeResumeEntry) return sg::Verdict::Run;
    bool process = false;
    void* actor = nullptr;
    void* comp = nullptr;
    if (!W::ReadIntake(call.object, process, actor, comp)) return sg::Verdict::Run;   // unreadable: no flip, said by the reader
    BeginResume(call, process);
    return sg::Verdict::Run;
}

// The resume of the overlap's Delay(0) is the intake: `process` rises when canChip passed and the prop is destroyed in
// that call, and stays down when the body refused. The POST finds the call by what its PRE captured, not by what the
// call left behind. The POST does not re-read the entry: the EntryPoint parameter is not the call's value once the body has
// run (measured: a POST that checked it for 554 never took the resume's capture), so the capture is paired by chipper and
// nesting depth alone, which the script gate makes exact (the same depth at the PRE and the POST of one call).
void OnGraphPost(const sg::Call& call) {
    if (!g_resumes.empty()) EndResume(call);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        sg::PreFn pre = i == kPour ? &OnPourPre : i == kPress ? &OnPressPre : i == kGraph ? &OnGraphPre : &OnRefusePre;
        sg::PostFn post = i == kPour ? &OnPourPost : i == kGraph ? &OnGraphPost : nullptr;
        if (sg::WatchName(*kRows[i], kTagBase + i, pre, post)) {
            g_registered.fetch_or(1u << i, std::memory_order_acq_rel);
            ++watched;
        } else {
            UE_LOGE("woodchipper_gate: the watch on %ls::%ls did not register -- a client runs it", kRows[i]->Class(),
                    kRows[i]->Name());
        }
    }
    UE_LOGI("woodchipper_gate: %d/%d chipper watches registered by class and name; the tick and the intake are "
            "refused on an active client session, its pour and press are asked of the host", watched, kRowCount);
}

void SettleWatches() {
    const uint32_t registered = g_registered.load(std::memory_order_acquire);
    if (g_settled || !registered) return;
    for (int i = 0; i < kRowCount; ++i)
        if ((registered & (1u << i)) && !sg::NameWatchSettled(*kRows[i], kTagBase + i)) return;
    g_settled = true;
    for (int i = 0; i < kRowCount; ++i)
        if ((registered & (1u << i)) && !sg::NameWatchLive(*kRows[i], kTagBase + i))
            UE_LOGW("woodchipper_gate: the watch on %ls::%ls settled dead -- a client chipper will run its own %s",
                    kRows[i]->Class(), kRows[i]->Name(), kRowTag[i]);
}

void OnDisconnect() {
    const Counts& c = g_counts;
    if (c.tickRefused || c.eatRefused || c.poursAsked || c.pressesAsked || c.notSent)
        UE_LOGI("woodchipper_gate: this session refused %llu chipper ticks and %llu intakes; asked %llu pours and %llu "
                "presses, %llu not sent", static_cast<unsigned long long>(c.tickRefused),
                static_cast<unsigned long long>(c.eatRefused), static_cast<unsigned long long>(c.poursAsked),
                static_cast<unsigned long long>(c.pressesAsked), static_cast<unsigned long long>(c.notSent));
    g_counts = Counts{};
    g_pourChipper = nullptr;
    g_emptyPours.clear();
    g_eatPairs.clear();
    g_entering.clear();
    for (Resume& r : g_resumes) DropCapture(r, "the session ended inside a resume");
    g_resumes.clear();
    g_flips.clear();
    g_recorded.clear();
}

Counts GetCounts() { return g_counts; }

bool IntakeOf(const std::wstring& key, Intake& out) {
    for (const Recorded& r : g_recorded)
        if (r.key == key) {
            out = r.intake;
            return true;
        }
    return false;
}

}  // namespace coop::woodchipper_gate
