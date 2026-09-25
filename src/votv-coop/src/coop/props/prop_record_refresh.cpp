// coop/props/prop_record_refresh.cpp -- see coop/props/prop_record_refresh.h.

#include "coop/props/prop_record_refresh.h"

#include "coop/net/session.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <iterator>
#include <string>
#include <unordered_map>

namespace coop::props::prop_record_refresh {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// The coalescing window. One player action can end in more than one upd() -- the open path runs
// it and the equip behind it can run it again -- and the record is the whole state either way,
// so the second publish would carry the same bytes. Game-thread serial, no lock.
constexpr auto kCoalesce = std::chrono::milliseconds(300);
std::unordered_map<void*, Clock::time_point> g_lastPublish;

unsigned long long g_cPublished = 0;
unsigned long long g_cCoalesced = 0;
unsigned long long g_cRelooked  = 0;

// `opened` is read to drive the look below; `name` and init() are what re-derive it.
int32_t g_openedOff = -1;
int32_t g_nameOff   = -1;
void*   g_initFn    = nullptr;

// The box's two names, exactly as its own Blueprint spells them: the open action sets
// 'drivebox_sb', putLidOn sets 'drivebox_s' back.
constexpr const wchar_t* kNameOpen   = L"drivebox_sb";
constexpr const wchar_t* kNameClosed = L"drivebox_s";

bool ReadOpened(void* box, bool& out) {
    if (!box) return false;
    if (g_openedOff < 0) {
        void* cls = R::ClassOf(box);
        if (!cls) return false;
        g_openedOff = R::FindPropertyOffset(cls, L"opened");
        if (g_openedOff < 0) return false;
    }
    out = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(box) + g_openedOff);
    return true;
}

// A record has just landed on `box`. Its LOOK follows `name`, NOT `opened`: Aprop_C::init picks
// the mesh from that field and the box's own init() chains to it. And `name` is the one thing the
// record deliberately does not carry -- save_record's splice keeps element 0 of every group, which
// is where Aprop_C writes name, for the RECEIVER, so a sender's name is dropped by design. So the
// look is re-derived here from the state that did travel, with the pair the Blueprint itself uses,
// and init() re-runs the mesh pick. Idempotent: a name already right returns before the write, so
// an init() that loops back through upd() stops on the second pass.
void RefreshLook(void* box, bool opened) {
    void* cls = R::ClassOf(box);
    if (!cls) return;
    if (g_nameOff < 0) g_nameOff = R::FindPropertyOffset(cls, L"name");
    // The memoised lookup: a box whose init never resolved would be looked up again, hop by hop, on
    // every record that lands on it.
    if (!g_initFn)     g_initFn  = R::FindDispatchFunctionCached(cls, L"init");
    if (g_nameOff < 0 || !g_initFn) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            UE_LOGW("prop_record_refresh: no `name` offset or no init() on the box -- a mirrored open "
                    "will set the state without the open mesh (it behaves open and looks shut)"); }
        return;
    }
    const wchar_t* want = opened ? kNameOpen : kNameClosed;
    if (ue_wrap::prop::GetPropNameString(box) == want) return;  // already the right look
    R::FName n = ue_wrap::fname_utils::StringToFName(want);
    std::memcpy(reinterpret_cast<uint8_t*>(box) + g_nameOff, &n, sizeof(n));
    ue_wrap::ParamFrame f(g_initFn);
    if (f.valid()) ue_wrap::Call(box, f);
    ++g_cRelooked;
    UE_LOGI("prop_record_refresh: a record landed on a drive box -> opened=%d, look re-derived to '%ls'",
            opened ? 1 : 0, want);
}

// The reel case's record fields, for the logs: `lid`, and each slot's reel progress (-1 = empty).
int32_t g_lidOff = -1, g_topOff = -1, g_bottomOff = -1;

std::string DescribeBox(void* box) {
    bool opened = false;
    return ReadOpened(box, opened) ? (opened ? "opened=1" : "opened=0") : "opened=?";
}

std::string DescribeReelCase(void* rc) {
    if (g_lidOff < 0) {
        void* cls = R::ClassOf(rc);
        if (!cls) return "?";
        g_lidOff    = R::FindPropertyOffset(cls, L"lid");
        g_topOff    = R::FindPropertyOffset(cls, L"reeltop");
        g_bottomOff = R::FindPropertyOffset(cls, L"reelBottom");
    }
    if (g_lidOff < 0 || g_topOff < 0 || g_bottomOff < 0) return "?";
    const char* b = reinterpret_cast<const char*>(rc);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "lid=%d reeltop=%.2f reelBottom=%.2f",
                  *reinterpret_cast<const bool*>(b + g_lidOff) ? 1 : 0,
                  *reinterpret_cast<const float*>(b + g_topOff),
                  *reinterpret_cast<const float*>(b + g_bottomOff));
    return buf;
}

// The watched classes. `relook`: the drive box's look does not follow its record and is
// re-derived on receipt (RefreshLook); the reel case's upd() reads `lid` itself.
struct Watched {
    const wchar_t* cls;
    int tag;
    bool relook;
    std::string (*describe)(void*);
};
constexpr Watched kWatched[] = {
    { L"prop_box_C",     0x44424F58, true,  &DescribeBox },       // 'DBOX'
    { L"prop_reelbox_C", 0x5242584F, false, &DescribeReelCase },  // 'RBXO'
};
// The UFunction each watch was armed on, so a re-armed one can be recognised. A world reload
// destroys and re-creates these classes and their functions: at a new address the old watch simply
// never fires again, and at a recycled one it would fire for whatever now lives there. An exact
// watch has to be re-validated; the neighbouring lanes use name watches, which resolve themselves.
void* g_watchedFn[std::size(kWatched)] = {};  // game thread only

const Watched* ByTag(int tag) {
    for (const Watched& w : kWatched)
        if (w.tag == tag) return &w;
    return nullptr;
}

void OnUpdPost(const sg::Call& call) {
    const Watched* w = ByTag(call.tag);
    if (!w || !call.object) return;
    // Our own apply is the one caller to ignore: prop_save_data puts a record in through the
    // prop's loadData, whose tail calls upd(), so without this an applied record would publish
    // itself straight back and the two peers would trade one record forever. The neighbouring
    // lanes gate on the same flag for the same reason. Logged rather than dropped silently: this
    // line is the far side's receipt, and it says which state actually landed.
    if (call.fromOurCode) {
        bool opened = false;
        if (w->relook) {
            if (ReadOpened(call.object, opened)) RefreshLook(call.object, opened);
        } else {
            UE_LOGI("prop_record_refresh: a record landed on %ls -> %s", w->cls,
                    w->describe(call.object).c_str());
        }
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    void* actor = call.object;
    // The load churn is not a change. A joining client runs every save prop's loadData, which
    // ends in upd(), so without this the join would send the host an intent per prop describing
    // the state the host has just sent us. The host's world is the reconciled authority, so the
    // gate never holds there.
    if (s->role() == coop::net::Role::Client &&
        !coop::join_membership_sweep::HasLoadTailQuiesced()) {
        return;
    }
    const auto now = Clock::now();
    auto it = g_lastPublish.find(actor);
    if (it != g_lastPublish.end() && now - it->second < kCoalesce) { ++g_cCoalesced; return; }
    // Evict what the window has passed. The map is keyed on the actor's address, so without this it
    // both grows for the life of the session and can hold a dead box's entry against a new one the
    // allocator puts at the same address -- which would swallow that box's first publish.
    for (auto e = g_lastPublish.begin(); e != g_lastPublish.end();)
        e = (now - e->second >= kCoalesce) ? g_lastPublish.erase(e) : std::next(e);

    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (key.empty() || key == L"None") {
        // A prop mid-construction: the key is minted inside the construction script, which calls
        // upd() itself. Nothing to address a record to, and the birth paths publish it anyway.
        return;
    }
    g_lastPublish[actor] = now;
    // Host: the record to every peer. Client: an intent the host validates and re-publishes, which
    // is also the acknowledgement. Either way the far side's loadData applies it and calls upd(),
    // so the lid, the contents and the repaint all land together.
    if (coop::prop_save_data::Publish(s, actor, key)) {
        ++g_cPublished;
        UE_LOGI("prop_record_refresh: published %ls key='%ls' %s (%s)", w->cls, key.c_str(),
                w->describe(actor).c_str(),
                s->role() == coop::net::Role::Client ? "client intent" : "host broadcast");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!GT::IsGameThread()) return;
    // Throttle the class walks while a class is unresolved, the shape prop_spawn_authoring and
    // prop_drop_intent use: this Install is the per-tick retry pump, a FindClass MISS is not cached
    // (a class can load later), and a walk renders a name per object, so an unresolved class would
    // cost one full object-array walk per frame for as long as the world goes without one. A class
    // that IS resolved costs a memoised lookup, so the pass below still runs every frame for it and
    // re-arms the watch when a world load hands back a new function.
    static int s_retry = 0;
    if (s_retry > 0) { --s_retry; return; }
    for (size_t i = 0; i < std::size(kWatched); ++i) {
        const Watched& w = kWatched[i];
        void* cls = R::FindClass(w.cls);
        if (!cls) { s_retry = 60; continue; }  // not loaded yet -- a second of frames from now
        // The memoised lookup holds its answer by slot and serial, so a class re-created by a world
        // load answers with the NEW function and the comparison below re-arms on it.
        void* fn = R::FindDispatchFunctionCached(cls, L"upd");
        if (fn && fn == g_watchedFn[i]) continue;  // armed, on the function that is here now
        if (!fn) {
            static bool s_saidMissing[std::size(kWatched)] = {};
            if (!s_saidMissing[i]) {
                s_saidMissing[i] = true;
                UE_LOGW("prop_record_refresh: %ls::upd UFunction not found -- its record will only "
                        "travel on its birth paths (an in-place change stays local)", w.cls);
            }
            s_retry = 60;
            continue;
        }
        // The script gate, not a ProcessEvent observer: the prop calls its own upd() from inside
        // its Blueprint, a route that never reaches ProcessEvent, and an observer there saw
        // nothing. The watch is on the exact UFunction rather than on the name `upd`, which half
        // the props in the game declare.
        if (g_watchedFn[i] && g_watchedFn[i] != fn)
            sg::Unwatch(g_watchedFn[i], w.tag, nullptr, &OnUpdPost);  // the world that owned it is gone
        if (!sg::Watch(fn, w.tag, nullptr, &OnUpdPost)) {
            // Back off with the same sixty frames the unresolved-class branch takes, and say so
            // once: a refusal persists (the gate did not install, or its table is full), so an
            // unthrottled retry would warn and re-resolve every frame for the life of the process.
            s_retry = 60;
            static bool s_saidRefused = false;
            if (!s_saidRefused) {
                s_saidRefused = true;
                UE_LOGW("prop_record_refresh: script-gate watch on %ls::upd refused -- retrying", w.cls);
            }
            continue;
        }
        const bool rearm = g_watchedFn[i] != nullptr;
        g_watchedFn[i] = fn;
        UE_LOGI("prop_record_refresh: %hs %ls::upd -- it republishes its own save record on "
                "every in-place change", rearm ? "re-armed on" : "watching", w.cls);
    }
}

void OnDisconnect() {
    if (g_cPublished || g_cCoalesced || g_cRelooked)
        UE_LOGI("prop_record_refresh: session end -- published=%llu coalesced=%llu relooked=%llu",
                g_cPublished, g_cCoalesced, g_cRelooked);
    g_lastPublish.clear();
    g_cPublished = 0;
    g_cCoalesced = 0;
    g_cRelooked  = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::props::prop_record_refresh
