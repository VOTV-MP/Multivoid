// coop/player/local_body.cpp -- see coop/player/local_body.h.

#include "coop/player/local_body.h"

#include "coop/comms/chat_feed.h"
#include "coop/player/client_model.h"
#include "coop/player/players_registry.h"
#include "coop/player/skin_registry.h"
#include "coop/session/player_handshake.h"
#include "coop/config/config.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hot_path_guard.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"

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
    SetSkinInternal(coop::skins::IsValidSkinName(name) ? name
                                                       : std::string(coop::skins::kDefaultSkinName));
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
    // Persist NOW (file IO is fine off the game thread; same ini as player_guid).
    coop::config::WriteIniValue("player_skin", name.c_str());
    ue_wrap::game_thread::Post([name] {
        SetSkinInternal(name);
        g_applied = false;  // Tick re-applies to the local pawn (also un-latches a pak-missing skip)
        UE_LOGI("local_body: skin -> '%s' (persisted; applying to local body + announcing)", name.c_str());
        if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
            coop::player_handshake::AnnounceLocalSkin(*s, name);
        coop::chat_feed::Push(L"Skin: " + std::wstring(name.begin(), name.end()));
    });
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    UE_ASSERT_GAME_THREAD("local_body::Tick");
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
            // Pak missing on this machine: stop retrying per tick; a RequestSkin
            // (or Refresh->re-pick) un-latches. The puppet on OTHER peers still
            // wears the skin -- only the local view degrades to kel.
            UE_LOGW("local_body: skin '%s' not loadable locally -- local body stays kel "
                    "(drop the pak into LogicMods/votv-coop and re-pick)", g_skin.c_str());
            g_applied = true;
            g_appliedSkin = g_skin;
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
