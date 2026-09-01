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
// The choice we have already warned about, so the warning is one per CHOICE and not
// one per tick for as long as a pak stays missing.
std::string g_lastUnwearable;
// The choice the verdict below belongs to, so a NEW choice is judged afresh.
std::string g_wearVerdictFor;
coop::client_model::Wearable g_wearVerdict = coop::client_model::Wearable::Unknown;

std::mutex  g_uiMutex;          // guards g_skinUi only
std::string g_skinUi;

void SetSkinInternal(const std::string& name) {
    g_skin = name;
    std::lock_guard<std::mutex> lk(g_uiMutex);
    g_skinUi = name;
}

}  // namespace

void SetInitialSkin(const std::string& name) {
    // A MALFORMED ini value falls back to the STOCK BODY. The retired `kDefaultSkinName`
    // named a PAK skin, so on an install not carrying it the "default" was one more name
    // that cannot load. `dr_kel` ships with the game and always resolves.
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
        // THE SAME PREDICATE THE APPLY ENFORCES. Asking `GetSkinMesh` alone was weaker: a pak
        // with a mesh but no atlas passed here, got persisted and announced, and failed to
        // apply on the very next tick -- two contradictory chat lines in consecutive frames.
        // Refused only on a DEFINITE No: `Unknown` means the resolver declined to ask inside
        // its retry window, and refusing a legitimate pick on a throttle would be a worse
        // failure than letting the apply heal it.
        if (coop::client_model::CanWearSkin(name) == coop::client_model::Wearable::No) {
            UE_LOGW("local_body: skin '%s' cannot be worn on this machine -- keeping '%s'; "
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

    // THE CHOICE AND THE WORN BODY ARE TWO VALUES, and fusing them was the root under the
    // root. `g_skin` is what the player CHOSE: it is persisted, announced, and restored from
    // the ini, and NOTHING local may overwrite it. What this body can actually put on is
    // DERIVED, every tick, from whether the choice resolves here.
    //
    // The first fix wrote the fallback back into `g_skin` -- persisting it and re-announcing
    // it -- because with one variable that was the only way to change the body. That turned a
    // LOCAL, MOMENTARY observation into a GLOBAL, PERMANENT verdict: a texture that had not
    // finished loading in the join window (a deferral `client_model` documents as "retry
    // heals") destroyed the player's saved skin and took it off every peer that COULD render
    // it. Splitting the two removes that possibility rather than guarding against it.
    //
    // Only a DEFINITE `No` degrades the body. `Unknown` means the resolver declined to ask
    // inside its retry window, so we keep trying the real choice and let the apply heal.
    // THE VERDICT IS STICKY PER CHOICE, and `Unknown` never overturns one. The resolver
    // refuses to re-probe inside a 5 s window, so a plain per-tick read alternates
    // No -> Unknown -> No forever -- and with it the worn body, which flip-flopped between
    // the stock body and a doomed re-apply every five seconds, warning the player each time
    // (12 warnings in one 30 s smoke, measured). A decision this expensive to reach is kept
    // until the CHOICE changes or the asset actually turns up.
    if (g_wearVerdictFor != g_skin) {
        g_wearVerdictFor = g_skin;
        g_wearVerdict = coop::client_model::Wearable::Unknown;
    }
    if (const auto w = coop::client_model::CanWearSkin(g_skin);
        w != coop::client_model::Wearable::Unknown) {
        g_wearVerdict = w;
    }
    const bool cannotWear = g_wearVerdict == coop::client_model::Wearable::No;
    const std::string worn = cannotWear ? std::string(coop::skins::kNativeSkinName) : g_skin;
    if (cannotWear && g_lastUnwearable != g_skin) {
        // ONCE PER CHOICE, not per tick: this is reached every tick while a chosen pak is
        // missing, and the peers are told nothing because the CHOICE still stands -- one that
        // owns the pak renders it correctly, which is the graceful degrade this lane promises
        // on screen ("Peers WITHOUT that pak see the default kel body instead").
        g_lastUnwearable = g_skin;
        UE_LOGW("local_body: skin '%s' cannot be worn here -- wearing the stock body; the "
                "CHOICE is kept (install the pak that carries it under "
                "Content/Paks/LogicMods -- any subfolder; the four scientists live in "
                "scientists.pak)", g_skin.c_str());
        // NO CHAT LINE HERE, and the distinction is who ACTED. `RequestSkin` says something
        // because the player just clicked a skin and deserves to know their click was
        // refused. This path is reached with the player doing NOTHING -- an ini value whose
        // pak is not installed -- and there is nothing for them to do about it at that
        // moment. Narrating it puts a sentence about another mod's pak in front of someone
        // who never chose it as a skin (user, 2026-09-01: "зачем это знать юзеру вообще").
        // The log line above is for us; the picker refusing the pick is for them.
    }
    if (!cannotWear) g_lastUnwearable.clear();

    if (!g_applied || g_appliedSkin != worn) {
        // LATCHED EITHER WAY. A failure here is transient by construction (the mesh resolved
        // and its atlas has not, or a component was momentarily unwritable), and the 1 Hz
        // converge below is what retries it. Leaving it unlatched re-entered this block at
        // pump rate -- ~500 ProcessEvent dispatches a second, plus a flushed error line per
        // component per tick when the cause was an unresolved mesh setter.
        coop::client_model::ApplySkinToBody(local, worn, g_native);
        g_applied = true;
        g_appliedSkin = worn;
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
