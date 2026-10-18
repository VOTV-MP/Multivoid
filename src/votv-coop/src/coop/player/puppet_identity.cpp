// coop/player/puppet_identity.cpp -- see coop/player/puppet_identity.h.

#include "coop/player/puppet_identity.h"

#include "puppet_identity_internal.h"  // the rows and seams shared with the scan file (src-local)

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"  // FindParamOffset
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::puppet_identity {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace OI = ue_wrap::object_index;
namespace PL = coop::players;

// Every stub below is a generated delegate handler that carries its toucher as a parameter named OtherActor,
// read by name. wisp_o and wisp_p, dream_climb and dream_jump, and the three Box stubs share a name across
// classes, which a Declared row tells apart.
UE_FACT_FN(kRowPuppetWispW, "puppet_identity", L"wisp_w_C",
           L"BndEvt__wisp_red_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetWispO, "puppet_identity", L"wisp_o_C",
           L"BndEvt__wisp_p_Sphere1_K2Node_ComponentBoundEvent_3_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetWispP, "puppet_identity", L"wisp_p_C",
           L"BndEvt__wisp_p_Sphere1_K2Node_ComponentBoundEvent_3_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetKelura, "puppet_identity", L"kelura_C",
           L"BndEvt__kelura_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetOrborb, "puppet_identity", L"npc_orborb_C",
           L"BndEvt__blackBall_Sphere1_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetWellTpBegin, "puppet_identity", L"wellTp_C",
           L"BndEvt__wellTp_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetWellTpEnd, "puppet_identity", L"wellTp_C",
           L"BndEvt__wellTp_Sphere_K2Node_ComponentBoundEvent_1_ComponentEndOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetDreamBoulder, "puppet_identity", L"dreamBoulder_C",
           L"BndEvt__dreamBoulder_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetMapTransition, "puppet_identity", L"trigger_box_openMapTransition_C",
           L"BndEvt__Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowPuppetBoxMark, "puppet_identity", L"trigger_box_C",
           L"BndEvt__Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowPuppetRunTriggerOpenLevel, "puppet_identity", L"trigger_openLevel_C", L"runTrigger", Declared);
UE_FACT_FN(kRowPuppetRunTriggerSpikes, "puppet_identity", L"tutorWall_spikes_C", L"runTrigger", Declared);
UE_FACT_FN(kRowPuppetDreamClimb, "puppet_identity", L"dream_climb_C",
           L"BndEvt__dream_climb_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetDreamJump, "puppet_identity", L"dream_jump_C",
           L"BndEvt__dream_climb_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetDreamRunFin, "puppet_identity", L"dream_run_C",
           L"BndEvt__fin_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowPuppetDreamRunWok, "puppet_identity", L"dream_run_C",
           L"BndEvt__wok_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowPuppetDreamfillDoor, "puppet_identity", L"dreamfillDoor_C",
           L"BndEvt__Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowPuppetGruntMark, "puppet_identity", L"prop_grunt_C",
           L"BndEvt__prop_grunt_damageSphere_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowPuppetSkullzone, "puppet_identity", L"skullzone_C",
           L"BndEvt__dest_K2Node_ComponentBoundEvent_2_ComponentBeginOverlapSignature__DelegateSignature", Declared);

sg::Verdict OnWholePre(const sg::Call& call);
sg::Verdict OnBoxMarkPre(const sg::Call& call);
sg::Verdict OnRunTriggerPre(const sg::Call& call);
sg::Verdict OnGruntPre(const sg::Call& call);
void OnGruntPost(const sg::Call& call);
sg::Verdict OnWispPPre(const sg::Call& call);
void OnWispPPost(const sg::Call& call);

// One name watch. OnWholePre cancels the stub for a puppet toucher. OnBoxMarkPre, OnGruntPre and OnWispPPre mark
// the stub's scope for a puppet toucher (the grunt's opens the SetControlRotation window, the wisp_p's the
// PlaySound2D and AddToViewport window). OnRunTriggerPre cancels the body while the box stub's mark is on the chain.
struct Watch {
    const ue_wrap::facts::Function* row;
    Row                             counter;  // kRowCount: the marking stub, which refuses nothing itself
    sg::PreFn                       pre;
    sg::PostFn                      post;
};

const Watch kWatches[] = {
    {&kRowPuppetWispW, kWispW, &OnWholePre, nullptr},
    {&kRowPuppetWispO, kWispO, &OnWholePre, nullptr},
    {&kRowPuppetWispP, kWispP, &OnWispPPre, &OnWispPPost},
    {&kRowPuppetKelura, kKelura, &OnWholePre, nullptr},
    {&kRowPuppetOrborb, kOrborb, &OnWholePre, nullptr},
    {&kRowPuppetWellTpBegin, kWellTpBegin, &OnWholePre, nullptr},
    {&kRowPuppetWellTpEnd, kWellTpEnd, &OnWholePre, nullptr},
    {&kRowPuppetDreamBoulder, kDreamBoulder, &OnWholePre, nullptr},
    {&kRowPuppetMapTransition, kMapTransition, &OnWholePre, nullptr},
    {&kRowPuppetBoxMark, kRowCount, &OnBoxMarkPre, nullptr},
    {&kRowPuppetRunTriggerOpenLevel, kTriggerOpenLevel, &OnRunTriggerPre, nullptr},
    {&kRowPuppetRunTriggerSpikes, kTriggerSpikes, &OnRunTriggerPre, nullptr},
    {&kRowPuppetDreamClimb, kDreamClimb, &OnWholePre, nullptr},
    {&kRowPuppetDreamJump, kDreamJump, &OnWholePre, nullptr},
    {&kRowPuppetDreamRunFin, kDreamRunFin, &OnWholePre, nullptr},
    {&kRowPuppetDreamRunWok, kDreamRunWok, &OnWholePre, nullptr},
    {&kRowPuppetDreamfillDoor, kDreamfillDoor, &OnWholePre, nullptr},
    {&kRowPuppetGruntMark, kRowCount, &OnGruntPre, &OnGruntPost},
    {&kRowPuppetSkullzone, kSkullzone, &OnWholePre, nullptr},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
static_assert(kWatchCount == 19, "nineteen name watches: seventeen stubs and two runTrigger bodies");

constexpr int kPiTagBase = 0x50490000;  // 'PI' and the watch's index

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_watched = false;
bool g_settled = false;
int  g_live = 0;

RowCounter g_counters[kRowCount] = {
    {"wisp_w", 0}, {"wisp_o", 0}, {"wisp_p", 0}, {"kelura", 0}, {"orborb", 0}, {"wellTp.begin", 0},
    {"wellTp.end", 0}, {"dreamBoulder", 0}, {"mapTransition", 0}, {"trigger.openLevel", 0}, {"trigger.spikes", 0},
    {"dream_climb", 0}, {"dream_jump", 0}, {"dream_run.fin", 0}, {"dream_run.wok", 0}, {"dreamfillDoor", 0},
    {"grunt", 0}, {"shower", 0}, {"birch", 0}, {"erie", 0}, {"skullzone", 0},
};
uint32_t g_logged = 0;  // a bit per row: said since the last resolve

// Each stub's OtherActor parameter, found by name in the stub's own frame at its first call: kUnread until then,
// -1 when the stub has none, said once.
constexpr int32_t kUnread = INT32_MIN;
int32_t g_actorOff[kWatchCount] = {};

// The scope mark rows 3, 9 and 11 put on a puppet's touch and read downstream. Its address is the owner.
int       g_markOwner = 0;
uintptr_t g_bit = 0;

// The registered puppets, from the registry's listener.
int g_count = 0;

// The session, for the role: row 3 is narrow on the host and whole on a client.
std::atomic<coop::net::Session*> g_session{nullptr};

bool OnHost() {
    const coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// What the last resolve described: the three marking stubs' classes, each class's listing identity
// (ClassListingId) and the stub functions, and the class-unlist number. A stub is looked at again only when its
// own class changed. A callback that acts on a stub pointer first checks its class's listing still stands
// (ClassStands: a class unloaded since frees the stub); the number only tells the resolve that no class was
// unloaded since, so it can skip its lookups.
constexpr uint64_t kNoVersion = UINT64_MAX;
uint64_t g_resolvedVer = kNoVersion;
void*    g_boxCls = nullptr;
void*    g_gruntCls = nullptr;
void*    g_wispPCls = nullptr;
uint64_t g_boxListing = 0;
uint64_t g_gruntListing = 0;
uint64_t g_wispPListing = 0;
void*    g_boxStub = nullptr;
void*    g_gruntStub = nullptr;
void*    g_wispPStub = nullptr;
int      g_gen = 0;  // moves with each resolve that looked at anything

// The stub's toucher, or null.
void* Toucher(int index, const sg::Call& call) {
    int32_t& off = g_actorOff[index];
    if (off == kUnread) {
        off = R::FindParamOffset(call.function, L"OtherActor");
        if (off < 0)
            UE_LOGE("[PI] %ls::%ls has no OtherActor parameter -- this stub refuses nothing",
                    kWatches[index].row->Class(), kWatches[index].row->Name());
    }
    if (off < 0 || !call.locals) return nullptr;
    return *reinterpret_cast<void* const*>(call.locals + off);
}

// Is the scope of `stub` (the box, grunt or wisp_p stub, of `cls` as listed at `listing`) marked on this
// thread's chain: the bit exists, the stub is resolved, a puppet toucher marked it, and the stub's class was not
// unloaded since the resolve. The class test comes last, after the cheap ones.
bool InScope(void* stub, void* cls, uint64_t listing) {
    return g_bit && stub && (sg::ScopeBits(stub) & g_bit) != 0 && ClassStands(cls, listing);
}

// Every stub below tests IsPuppetBody (a deny-list), not a positive "is not this machine's player": Local() is
// null for a moment across a travel or a respawn, and a positive test would refuse this machine's own player's
// legitimate touch then. The cost is a known gap: an unregistered mainPlayer_C (the F1 skin-preview mannequin)
// is served as SP would serve it.
sg::Verdict OnWholePre(const sg::Call& call) {
    const int index = call.tag - kPiTagBase;
    if (index < 0 || index >= kWatchCount) return sg::Verdict::Run;
    void* actor = Toucher(index, call);
    if (!actor || !PL::Registry::Get().IsPuppetBody(actor)) return sg::Verdict::Run;
    NoteRefused(kWatches[index].counter, actor);
    return sg::Verdict::Cancel;
}

sg::Verdict OnBoxMarkPre(const sg::Call& call) {
    const int index = call.tag - kPiTagBase;
    if (!g_bit || index < 0 || index >= kWatchCount) return sg::Verdict::Run;
    void* actor = Toucher(index, call);
    if (actor && PL::Registry::Get().IsPuppetBody(actor)) sg::MarkScope(call, g_bit);
    return sg::Verdict::Run;
}

// The two targets of the box that leave the level or the game. The box's other targets run on.
sg::Verdict OnRunTriggerPre(const sg::Call& call) {
    if (!InScope(g_boxStub, g_boxCls, g_boxListing)) return sg::Verdict::Run;
    const int index = call.tag - kPiTagBase;
    if (index < 0 || index >= kWatchCount) return sg::Verdict::Run;
    NoteRefused(kWatches[index].counter, nullptr);
    return sg::Verdict::Cancel;
}

sg::Verdict OnGruntPre(const sg::Call& call) {
    const int index = call.tag - kPiTagBase;
    if (!g_bit || !g_gruntStub || index < 0 || index >= kWatchCount) return sg::Verdict::Run;
    void* actor = Toucher(index, call);
    if (!actor || !PL::Registry::Get().IsPuppetBody(actor)) return sg::Verdict::Run;
    sg::MarkScope(call, g_bit);
    scan::ArmWindow(scan::kGruntWindow, true);
    return sg::Verdict::Run;
}

// The window closes when no other grunt body is still open on the chain. IsBodyActive compares the pointer and
// reads nothing through it, so closing needs no class test: a window left open is the worse failure.
void OnGruntPost(const sg::Call&) {
    if (!sg::IsBodyActive(g_gruntStub)) scan::ArmWindow(scan::kGruntWindow, false);
}

// wisp_p (row 3). On a client the wisp is a mirror and the clock the host's, so the stub is cancelled whole. On
// the host the stub runs (its rewind is the host's world clock, carried to every peer by time_sync) and a puppet
// toucher only marks its scope and opens the window in which its 2D sound and flashbang are refused.
sg::Verdict OnWispPPre(const sg::Call& call) {
    const int index = call.tag - kPiTagBase;
    if (index < 0 || index >= kWatchCount) return sg::Verdict::Run;
    void* actor = Toucher(index, call);
    if (!actor || !PL::Registry::Get().IsPuppetBody(actor)) return sg::Verdict::Run;
    if (!OnHost()) {
        NoteRefused(kWispP, actor);
        return sg::Verdict::Cancel;
    }
    // No scope bit (logged at Install) or no resolved stub: the stub runs whole, sound and flashbang with it, the one
    // window row 3 has that a WHOLE cancel did not.
    if (!g_bit || !g_wispPStub) return sg::Verdict::Run;
    sg::MarkScope(call, g_bit);
    scan::ArmWindow(scan::kWispPWindow, true);
    return sg::Verdict::Run;
}

// As OnGruntPost: closes unless another wisp_p body is still open on the chain.
void OnWispPPost(const sg::Call&) {
    if (!sg::IsBodyActive(g_wispPStub)) scan::ArmWindow(scan::kWispPWindow, false);
}

void OnPuppetCount(int count) {
    g_count = count;
    scan::Rearm(count);
}

void Settle() {
    sg::ResolvePendingNames();
    // It waits on the process-wide pending count; that only delays the settle line, never a row's behaviour.
    // At no pending name every watch is live or dead for good (script_gate.h), so the walk below waits for that
    // instead of running each frame a name still waits for its FName.
    if (sg::PendingNameCount() != 0) return;
    int live = 0, settled = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Refused) { ++settled; continue; }
        if (sg::NameWatchLive(*kWatches[i].row, kPiTagBase + i)) { ++live; ++settled; continue; }
        if (sg::NameWatchSettled(*kWatches[i].row, kPiTagBase + i)) ++settled;
    }
    if (settled < kWatchCount) return;
    g_live = live;
    g_settled = true;
    UE_LOGI("[PI] watches live=%d unresolved=%d", WatchesLive(), kWatchCount - WatchesLive());
    for (int i = 0; i < kWatchCount; ++i)
        if (g_reg[i] == Reg::Refused || !sg::NameWatchLive(*kWatches[i].row, kPiTagBase + i))
            UE_LOGE("[PI] %ls::%ls is unwatched -- a puppet's touch there acts on this machine's player",
                    kWatches[i].row->Class(), kWatches[i].row->Name());
}

// A marking stub's function on its class, null when the class lacks it (said once per resolve).
void* StubOn(const ue_wrap::facts::Function& row, void* cls) {
    if (!cls) return nullptr;
    void* fn = ue_wrap::facts::ResolveOn(row, cls).Fn();
    if (!fn)
        UE_LOGE("[PI] %ls has no %ls -- the rows that read its mark refuse nothing in this world", row.Class(),
                row.Name());
    return fn;
}

// One marking stub looked at again when its class is another or was listed again. True when it was.
bool ResolveStub(const wchar_t* clsName, const ue_wrap::facts::Function& row, void*& cls, uint64_t& listing,
                 void*& stub) {
    void* const now = OI::ClassByName(clsName);
    const uint64_t id = OI::ClassListingId(now);
    if (now == cls && id == listing) return false;
    cls = now;
    listing = id;
    stub = StubOn(row, now);
    return true;
}

// The per-world resolve. Class names are unique, so a class pointer held non-null changes only through an
// unload, which moves the unlist number: while the number stands and the three stub classes are held, their
// lookups are skipped, and the scan rows' own lookups too (scan::Resolve). When it moved, each class is compared
// on its own listing: an unrelated class's unload only renews the number.
void ResolveWorld() {
    const uint64_t ver = OI::ClassUnlistVersion();
    bool moved = false;
    if (ver != g_resolvedVer || !g_boxCls || !g_gruntCls || !g_wispPCls) {
        moved |= ResolveStub(L"trigger_box_C", kRowPuppetBoxMark, g_boxCls, g_boxListing, g_boxStub);
        moved |= ResolveStub(L"prop_grunt_C", kRowPuppetGruntMark, g_gruntCls, g_gruntListing, g_gruntStub);
        moved |= ResolveStub(L"wisp_p_C", kRowPuppetWispP, g_wispPCls, g_wispPListing, g_wispPStub);
        g_resolvedVer = ver;
    }
    moved |= scan::Resolve(ver);
    if (!moved) return;
    ++g_gen;
    g_logged = 0;
}

}  // namespace

bool IsPuppetBodyFn(void*, void* actor) { return PL::Registry::Get().IsPuppetBody(actor); }

void NoteRefused(Row row, void* actor) {
    if (row >= kRowCount) return;
    ++g_counters[row].refused;
    const uint32_t bit = uint32_t{1} << row;
    if (g_logged & bit) return;
    g_logged |= bit;
    // A native seam's refusal and a scan's removal have no toucher in hand; a stub's does.
    if (!actor) {
        UE_LOGI("[PI] refused %s for a puppet body", g_counters[row].row);
        return;
    }
    const uint8_t slot = PL::Registry::Get().PeerIdOfBody(actor);
    if (slot == PL::kPeerIdUnknown)
        UE_LOGI("[PI] refused %s toucher=puppet slot=unknown", g_counters[row].row);
    else
        UE_LOGI("[PI] refused %s toucher=puppet slot=%u", g_counters[row].row, static_cast<unsigned>(slot));
}

bool InGruntScope() { return InScope(g_gruntStub, g_gruntCls, g_gruntListing); }

bool InWispPScope() { return InScope(g_wispPStub, g_wispPCls, g_wispPListing); }

bool ClassStands(void* cls, uint64_t listing) { return cls && OI::ClassListingId(cls) == listing; }

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watched) {
        g_watched = true;
        for (int32_t& off : g_actorOff) off = kUnread;
        g_bit = sg::ScopeBitFor(&g_markOwner);
        if (!g_bit)
            UE_LOGE("[PI] no scope-mark bit is free -- trigger.openLevel, trigger.spikes, grunt and wisp_p (on the "
                    "host) refuse nothing");
        for (int i = 0; i < kWatchCount; ++i)
            g_reg[i] = sg::WatchName(*kWatches[i].row, kPiTagBase + i, kWatches[i].pre, kWatches[i].post)
                           ? Reg::Registered : Reg::Refused;
        PL::Registry::Get().SetPuppetCountListener(&OnPuppetCount);
    }
    scan::InstallPump(g_gen, g_count);
}

void Tick() {
    if (!g_watched) return;
    if (!g_settled) Settle();
    ResolveWorld();
    // A window a Cancel or a fault left open: it is armed and no body of its stub is open on the chain.
    if (scan::WindowArmed(scan::kGruntWindow) && !sg::IsBodyActive(g_gruntStub))
        scan::ArmWindow(scan::kGruntWindow, false);
    if (scan::WindowArmed(scan::kWispPWindow) && !sg::IsBodyActive(g_wispPStub))
        scan::ArmWindow(scan::kWispPWindow, false);
}

int WatchesLive() { return g_live; }

const RowCounter* Counters(int& n) {
    n = kRowCount;
    return g_counters;
}

void OnDisconnect() {
    int rows = 0;
    const RowCounter* counters = Counters(rows);
    for (int i = 0; i < rows; ++i)
        if (counters[i].refused)
            UE_LOGI("[PI] session end -- %s refused %u call(s) since the process started", counters[i].row,
                    static_cast<unsigned>(counters[i].refused));
    g_boxCls = g_gruntCls = g_wispPCls = nullptr;
    g_boxListing = g_gruntListing = g_wispPListing = 0;
    g_boxStub = g_gruntStub = g_wispPStub = nullptr;
    g_resolvedVer = kNoVersion;
    g_logged = 0;
    scan::Clear();
}

}  // namespace coop::puppet_identity
