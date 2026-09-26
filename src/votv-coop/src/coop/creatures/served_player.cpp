// coop/creatures/served_player.cpp -- see coop/creatures/served_player.h.

#include "coop/creatures/served_player.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace coop::served_player {
namespace {

namespace GT = ue_wrap::game_thread;
namespace K  = ue_wrap::kerfur;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;

constexpr const wchar_t* kLibClass = L"lib_C";
constexpr const wchar_t* kMainPlayerFn = L"getMainPlayer";
constexpr int kTagMainPlayer = 0x53505001;  // 'SPP' 1

std::atomic<coop::net::Session*> g_session{nullptr};

struct Rec {
    int32_t idx;
    uint8_t slot;
    uint8_t seams;
};
std::unordered_map<void*, Rec> g_records;
unsigned long long g_answered = 0;

// A native player-0 read, post-hooked: the result is rewritten when the calling object is a robot served
// through this seam.
struct NativeSeam {
    const wchar_t*            name;
    uint8_t                   bit;
    UH::PostNativeCallback    cb;
    void*                     fn = nullptr;
    bool                      installed = false;
    bool                      refused = false;   // this build lacks it, or the hook table is full: final
    bool                      armed = false;
};

// The getMainPlayer gate watch, registered and resolved once, at Install, and retired until a record needs
// it: a re-watch re-enables the resolved slot, so the verb that recorded the robot runs with it live. Refused
// for good when it cannot live.
bool g_mainPrimed = false;
bool g_mainWatched = false;
bool g_mainRefused = false;

bool OnHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Host;
}

void* PawnOf(uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return (rp && rp->valid() && rp->HasPose()) ? rp->GetActor() : nullptr;
}

// Whether `robot` is served through `seam`; `pawn` is its client's body, null while that body is absent (a
// puppet respawning, not yet posed). Drops a dead robot's record.
bool Served(void* robot, uint8_t seam, void*& pawn) {
    pawn = nullptr;
    auto it = g_records.find(robot);
    if (it == g_records.end() || !(it->second.seams & seam)) return false;
    if (!R::IsLiveByIndex(robot, it->second.idx)) {
        g_records.erase(it);
        return false;
    }
    pawn = PawnOf(it->second.slot);
    return true;
}

// An absent body leaves the read as written: the Blueprint reads through what these return, and a null
// would fault its next read.
void AnswerNative(void* source, uint8_t seam) {
    if (!source || !GT::IsGameThread()) return;
    void* pawn = nullptr;
    if (!Served(source, seam, pawn) || !pawn) return;
    if (void* r = UH::CurrentResult()) {
        *static_cast<void**>(r) = pawn;
        ++g_answered;
    }
}

void OnCharacterPost(void* /*context*/, void* source, void* /*result*/) { AnswerNative(source, kPlayerCharacter); }
void OnPawnPost(void* /*context*/, void* source, void* /*result*/) { AnswerNative(source, kPlayerPawn); }

NativeSeam g_gpc{L"GetPlayerCharacter", kPlayerCharacter, &OnCharacterPost};
NativeSeam g_gpp{L"GetPlayerPawn", kPlayerPawn, &OnPawnPost};

// lib.getMainPlayer(__WorldContext, out AsMain Player): the library's default object runs the body, so the
// calling robot is the caller frame's object. Of a served robot's calls only the Omega's disc insert reads
// the one it serves (ue_wrap::kerfur::IsInsertPlayerRead, by the variable its out argument writes); murder
// mode's, the petting's and loadHoldItem's (a function nothing calls) read player 0 as written. An insert
// read is answered in full, as the body would: the out parameter is the only thing it writes -- null while
// the client's body is absent, which takes the insert's own refusal rather than the host's hand.
sg::Verdict OnMainPlayerPre(const sg::Call& c) {
    if (g_records.empty() || !c.callerObject) return sg::Verdict::Run;
    void* pawn = nullptr;
    if (!Served(c.callerObject, kMainPlayer, pawn)) return sg::Verdict::Run;
    static void*   sFn = nullptr;
    static int32_t sOutOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sOutOff = R::FindParamOffset(c.function, L"AsMain Player");
    }
    uint8_t* out = sOutOff >= 0 ? sg::OutParamPtr(c, sOutOff) : nullptr;
    if (!out || !K::IsInsertPlayerRead(c.callerFunction, c.callerLocals, out)) return sg::Verdict::Run;
    *reinterpret_cast<void**>(out) = pawn;
    ++g_answered;
    return sg::Verdict::Cancel;
}

uint8_t RefusedSeams() {
    return static_cast<uint8_t>((g_gpc.refused ? kPlayerCharacter : 0) | (g_gpp.refused ? kPlayerPawn : 0) |
                                (g_mainRefused ? kMainPlayer : 0));
}

// A robot one of whose seams will not answer serves no one: answered through some reads and not others, it
// would judge its approach by the client and walk to the host.
void DropRecordsNeeding(uint8_t seams) {
    for (auto it = g_records.begin(); it != g_records.end();) {
        if (it->second.seams & seams) it = g_records.erase(it);
        else ++it;
    }
}

void RefuseMain(const char* why) {
    g_mainRefused = true;
    UE_LOGE("served_player: the getMainPlayer seam is refused (%s) -- an Omega serves no client for the rest of "
            "this process", why);
    DropRecordsNeeding(kMainPlayer);
}

bool MainLive() { return sg::ClassNameWatchLive(kLibClass, kMainPlayerFn, kTagMainPlayer); }

// Register the watch and resolve its names now, a game-thread engine call; until it settles, asked again on
// the next call. UpdateArm retires it once no record needs it.
void PrimeMain() {
    if (g_mainPrimed || g_mainRefused) return;
    if (!g_mainWatched &&
        !(g_mainWatched = sg::WatchClassName(kLibClass, kMainPlayerFn, kTagMainPlayer, &OnMainPlayerPre, nullptr))) {
        RefuseMain("its watch was refused");
        return;
    }
    sg::ResolvePendingNames();
    if (MainLive()) {
        g_mainPrimed = true;
        UE_LOGI("served_player: the disc insert's getMainPlayer watch resolved -- enabled while an Omega serves a "
                "client");
    } else if (sg::ClassNameWatchSettled(kLibClass, kMainPlayerFn, kTagMainPlayer)) {
        RefuseMain("its watch died at the name resolve");
    }
}

void InstallSeam(NativeSeam& seam, void* statics) {
    if (seam.installed || seam.refused) return;
    seam.fn = R::FindFunction(statics, seam.name);
    if (!seam.fn || !UH::InstallPostHook(seam.fn, seam.cb, /*armed=*/false)) {
        seam.refused = true;
        UE_LOGE("served_player: the %ls seam did not install -- a robot reading through it serves no client for the "
                "rest of this process", seam.name);
        DropRecordsNeeding(seam.bit);
        return;
    }
    seam.installed = true;
}

void ArmSeam(NativeSeam& seam, bool want) {
    if (!seam.installed || seam.armed == want) return;
    if (UH::SetArmed(seam.fn, seam.cb, want)) seam.armed = want;
}

uint8_t WantedSeams() {
    uint8_t want = 0;
    for (const auto& kv : g_records) want |= kv.second.seams;
    return want;
}

// Each seam follows the records that need it: armed, or watched, while one lives.
void UpdateArm() {
    if (g_mainPrimed) {
        const bool need = (WantedSeams() & kMainPlayer) != 0;
        if (need && !g_mainWatched) {
            g_mainWatched = sg::WatchClassName(kLibClass, kMainPlayerFn, kTagMainPlayer, &OnMainPlayerPre, nullptr);
            if (!g_mainWatched || !MainLive()) RefuseMain("its resolved watch did not come back live");
        } else if (!need && g_mainWatched) {
            sg::UnwatchClassName(kLibClass, kMainPlayerFn, kTagMainPlayer, &OnMainPlayerPre, nullptr);
            g_mainWatched = false;
        }
    }
    const uint8_t want = WantedSeams();
    ArmSeam(g_gpc, (want & kPlayerCharacter) != 0);
    ArmSeam(g_gpp, (want & kPlayerPawn) != 0);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!OnHost()) return;  // a client never records a served robot
    PrimeMain();
    if ((g_gpc.installed || g_gpc.refused) && (g_gpp.installed || g_gpp.refused)) return;
    void* statics = R::FindClass(P::name::GameplayStaticsClass);
    if (!statics) return;  // the engine class resolves at boot; asked again next call
    InstallSeam(g_gpc, statics);
    InstallSeam(g_gpp, statics);
}

bool SeamsReady(uint8_t seams) {
    if ((seams & kPlayerCharacter) && !g_gpc.installed) return false;
    if ((seams & kPlayerPawn) && !g_gpp.installed) return false;
    if ((seams & kMainPlayer) && (!g_mainPrimed || g_mainRefused)) return false;
    return true;
}

bool SeamsRefused(uint8_t seams) { return (seams & RefusedSeams()) != 0; }

bool Serve(void* robot, uint8_t slot, uint8_t seams) {
    if (!robot || !OnHost()) return false;
    if (slot == coop::players::kPeerIdHost || !SeamsReady(seams)) {
        Unserve(robot);
        return false;
    }
    g_records[robot] = Rec{R::InternalIndexOf(robot), slot, seams};
    UpdateArm();
    return g_records.count(robot) != 0;  // a watch refused here drops the record it was for
}

void Unserve(void* robot) {
    if (g_records.erase(robot)) UpdateArm();
}

uint8_t ServedSlot(void* robot) {
    auto it = g_records.find(robot);
    if (it == g_records.end() || !R::IsLiveByIndex(robot, it->second.idx)) return coop::players::kPeerIdHost;
    return it->second.slot;
}

void Tick() {
    if (g_records.empty() && !g_mainWatched && !g_gpc.armed && !g_gpp.armed) return;
    for (auto it = g_records.begin(); it != g_records.end();) {
        if (!R::IsLiveByIndex(it->first, it->second.idx)) it = g_records.erase(it);
        else ++it;
    }
    UpdateArm();
}

void OnPeerLeft(uint8_t slot) {
    bool any = false;
    for (auto it = g_records.begin(); it != g_records.end();) {
        if (it->second.slot == slot) { it = g_records.erase(it); any = true; }
        else ++it;
    }
    if (any) UpdateArm();
}

void OnDisconnect() {
    if (g_answered)
        UE_LOGI("served_player: session end -- %llu player-0 reads answered with a served client", g_answered);
    g_records.clear();
    UpdateArm();
    g_answered = 0;
}

}  // namespace coop::served_player
