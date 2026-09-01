// coop/player/local_body.cpp -- see coop/player/local_body.h.

#include "coop/player/local_body.h"

#include "coop/comms/chat_feed.h"
#include "coop/player/client_model.h"
#include "coop/player/players_registry.h"
#include "coop/player/skin_registry.h"
#include "coop/session/player_handshake.h"
#include "coop/config/config.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace coop::local_body {

namespace {

namespace Pup = ue_wrap::puppet;
namespace R = ue_wrap::reflection;
using clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// Game-thread-owned state (boot-thread SetInitialSkin runs before the pump
// ticks -- the SetLocalNickname discipline). The UI reads via the mutex copy.
std::string g_skin;             // the DESIRED local skin
std::string g_appliedSkin;      // what's applied to the current pawn generation
bool        g_applied = false;
void*       g_pawn = nullptr;   // current pawn generation
int32_t     g_pawnIdx = -1;
void*       g_native = nullptr; // pristine kel mesh of this generation (pre-swap)
int32_t     g_nativeIdx = -1;
clock::time_point g_lastConverge{};

std::mutex  g_uiMutex;          // guards g_skinUi only
std::string g_skinUi;

void SetSkinInternal(const std::string& name) {
    g_skin = name;
    std::lock_guard<std::mutex> lk(g_uiMutex);
    g_skinUi = name;
}

}  // namespace

void SetInitialSkin(const std::string& name) {
    // A MALFORMED ini value falls back to the STOCK BODY, not to `kDefaultSkinName` -- that
    // constant names a pak skin, so on an install that does not carry it the "default" is
    // one more unresolvable name. `dr_kel` ships with the game and always resolves.
    SetSkinInternal(coop::skins::IsValidSkinName(name) ? name
                                                       : std::string(coop::skins::kNativeSkinName));
}

const std::string& LocalSkinName() { return g_skin; }

std::string LocalSkinNameCopy() {
    std::lock_guard<std::mutex> lk(g_uiMutex);
    return g_skinUi;
}

void* NativeBodyMesh() {
    if (g_native && R::IsLiveByIndex(g_native, g_nativeIdx)) return g_native;
    return nullptr;
}

void RequestSkin(const std::string& name) {
    if (!coop::skins::IsValidSkinName(name)) {
        UE_LOGW("local_body: RequestSkin('%s') rejected (invalid name)", name.c_str());
        return;
    }
    ue_wrap::game_thread::Post([name] {
        // RESOLVE BEFORE COMMITTING -- and this order is the fix, not a nicety.
        //
        // This used to persist and announce FIRST and discover only afterwards, in Tick,
        // that the skin could not be applied. That failure was written off as cosmetic
        // ("only the local view degrades to kel") on the assumption that a name we cannot
        // load is one our PEERS can. The opposite is the common case: the skin list offers
        // the stem of every .pak under LogicMods, so another mod's pak -- `DebugMod`,
        // `FusionPatch_P` -- is offered as a skin and exists as a skin NOWHERE.
        //
        // What the player then got (user, 2026-09-01, measured in both peers' logs): the
        // announce went out with the unusable name, every peer failed the same load and
        // fell back to the native kel, while the local body kept the skin it was ALREADY
        // wearing -- the apply failed, so nothing overwrote it. So the host saw itself as a
        // scientist and everyone else saw dr. kel, permanently. Picking a loadable skin on
        // both machines "fixed" it only by making two wrong sources agree.
        //
        // A value we have just proven we cannot honour must not be persisted, must not be
        // announced, and must not replace the one that works.
        if (!coop::client_model::IsNativeSkin(name) && !coop::client_model::GetSkinMesh(name)) {
            UE_LOGW("local_body: skin '%s' does NOT resolve on this machine -- keeping '%s'; "
                    "not persisted and not announced", name.c_str(), g_skin.c_str());
            coop::chat_feed::Push(
                L"Skin '" + std::wstring(name.begin(), name.end()) +
                    L"' is not installed here -- keeping your current one",
                coop::chat_feed::Keep::Transient);
            return;
        }
        // Persist only what resolved. (File IO from the game thread is what the ordering
        // costs; it is one ini write per deliberate skin change, not a hot path.)
        coop::config::WriteIniValue(coop::config_registry::rows::player_skin, name.c_str());
        SetSkinInternal(name);
        g_applied = false;  // Tick re-applies to the local pawn (also un-latches a pak-missing skip)
        UE_LOGI("local_body: skin -> '%s' (persisted; applying to local body + announcing)", name.c_str());
        if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
            coop::player_handshake::AnnounceLocalSkin(*s, name);
        // Transient: this player's own UI confirmation. The line other peers see about
        // this change ("<nick> changed skin to X", player_handshake_prefs) IS History.
        coop::chat_feed::Push(L"Skin: " + std::wstring(name.begin(), name.end()),
                              coop::chat_feed::Keep::Transient);
    });
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    UE_ASSERT_GAME_THREAD("local_body::Tick");
    // A FEW PER TICK, because this is the only game-thread pass the skin picker has and
    // the picker draws off-thread. Self-limiting: each entry is asked once per scan.
    coop::skins::ResolvePending(2);
    void* local = coop::players::Registry::Get().Local();
    if (!local) {
        g_pawn = nullptr;  // level transition/menu: next pawn is a fresh generation
        return;
    }
    if (local != g_pawn || !R::IsLiveByIndex(g_pawn, g_pawnIdx)) {
        // New pawn generation (first world, level change, respawn): re-capture the
        // pristine kel mesh BEFORE any swap and re-apply the desired skin.
        g_pawn = local;
        g_pawnIdx = R::InternalIndexOf(local);
        g_native = nullptr;
        g_nativeIdx = -1;
        g_applied = false;
    }
    if (!g_native) {
        void* mesh = Pup::GetMeshPlayerVisibleAsset(local);
        if (!mesh) return;  // save-load hasn't dressed the player yet -- wait
        g_native = mesh;
        g_nativeIdx = R::InternalIndexOf(mesh);
        UE_LOGI("local_body: native kel mesh captured (%p) for pawn %p", g_native, g_pawn);
    }

    if (!g_applied || g_appliedSkin != g_skin) {
        if (coop::client_model::ApplySkinToBody(local, g_skin, g_native)) {
            g_applied = true;
            g_appliedSkin = g_skin;
        } else if (!coop::client_model::IsNativeSkin(g_skin)) {
            // FALL BACK TO SOMETHING REAL, rather than latching on a value we cannot wear.
            //
            // This branch used to log and latch, on the reasoning that "the puppet on OTHER
            // peers still wears the skin -- only the local view degrades to kel". That holds
            // only when the pak is missing HERE and present THERE. When the name resolves
            // NOWHERE -- which is what happens once another mod's pak has been offered as a
            // skin and picked -- the announce keeps naming it, every peer fails the same
            // load, and this body keeps whatever it was ALREADY wearing because the apply
            // failed and overwrote nothing. That is how a host saw itself as a scientist
            // while everyone else saw dr. kel (user, 2026-09-01).
            //
            // RequestSkin now refuses an unresolvable pick outright, so this path is reached
            // by the OTHER producer: a name restored from the ini (or a starter roll) whose
            // pak has since been removed -- including the unusable one an older build
            // persisted before that refusal existed. The cure is the same either way: wear
            // the default, write it down, and TELL THE PEERS, so the worn skin and the
            // announced skin can never disagree.
            UE_LOGW("local_body: skin '%s' does not resolve here -- falling back to '%s' and "
                    "re-announcing (install the pak that carries it under "
                    "Content/Paks/LogicMods -- any subfolder; the four scientists live in "
                    "scientists.pak -- then re-pick)",
                    g_skin.c_str(), coop::skins::kNativeSkinName);
            coop::chat_feed::Push(
                L"Skin '" + std::wstring(g_skin.begin(), g_skin.end()) +
                    L"' is not installed -- wearing the stock body instead",
                coop::chat_feed::Keep::Transient);
            // THE STOCK BODY, NOT `kDefaultSkinName`. The first version of this fallback
            // named `hl_einstein_v1sc`, which is itself a PAK skin -- so on an install that
            // does not carry that pak the fallback was a second unresolvable name and the
            // divergence simply moved. `dr_kel` is the game's own body: it needs no pak, it
            // is what every peer already degrades to, and it is the one value that cannot
            // fail. (User, 2026-09-01, who had deliberately kept only scientists.pak.)
            coop::config::WriteIniValue(coop::config_registry::rows::player_skin,
                                        coop::skins::kNativeSkinName);
            SetSkinInternal(coop::skins::kNativeSkinName);
            g_applied = false;   // the next tick applies the stock body for real
            if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
                coop::player_handshake::AnnounceLocalSkin(*s, g_skin);
        }
        g_lastConverge = clock::now();
        return;
    }

    // 1 Hz convergence: detect a game-side re-dress (save-load, sleep, ragdoll
    // recovery...) that silently reverted the visible mesh. Re-apply + LOG the
    // event so the exact seam can be hooked once we know it exists.
    const auto now = clock::now();
    if (now - g_lastConverge < std::chrono::seconds(1)) return;
    g_lastConverge = now;
    void* expected = coop::client_model::IsNativeSkin(g_appliedSkin)
                         ? g_native
                         : coop::client_model::GetSkinMesh(g_appliedSkin);
    if (!expected) return;
    void* comp = Pup::GetMeshPlayerVisibleComponent(local);
    void* cur = comp ? Pup::GetComponentSkeletalMeshAsset(comp) : nullptr;
    if (cur && cur != expected) {
        UE_LOGW("local_body: game re-dressed the local body (mesh %p != expected %p) -- "
                "re-applying skin '%s' (tell the devs WHAT you did just before this line)",
                cur, expected, g_appliedSkin.c_str());
        g_applied = false;
    }
}

}  // namespace coop::local_body
