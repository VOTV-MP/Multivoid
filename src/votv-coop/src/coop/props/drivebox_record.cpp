// coop/props/drivebox_record.cpp -- see coop/props/drivebox_record.h.

#include "coop/props/drivebox_record.h"

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
#include <cstring>
#include <chrono>
#include <string>
#include <unordered_map>

namespace coop::props::drivebox_record {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

using Clock = std::chrono::steady_clock;

constexpr int kTagDriveBox = 0x44424F58;  // 'DBOX'

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_installed{false};

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
    if (!g_initFn)     g_initFn  = R::FindFunction(cls, L"init");
    if (g_nameOff < 0 || !g_initFn) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            UE_LOGW("drivebox_record: no `name` offset or no init() on the box -- a mirrored open "
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
    UE_LOGI("drivebox_record: a record landed on a box -> opened=%d, look re-derived to '%ls'",
            opened ? 1 : 0, want);
}

void OnUpdPost(const sg::Call& call) {
    // Our own apply is the one caller to ignore: prop_save_data puts a record in through the
    // prop's loadData, whose tail calls upd(), so without this an applied record would publish
    // itself straight back and the two peers would trade one record forever. The neighbouring
    // lanes gate on the same flag for the same reason. Logged rather than dropped silently: this
    // line is the far side's receipt, and it says which state actually landed.
    if (call.fromOurCode) {
        bool opened = false;
        if (ReadOpened(call.object, opened)) RefreshLook(call.object, opened);
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    void* box = call.object;
    if (!box) return;
    // The load churn is not a change. A joining client runs every save prop's loadData, which
    // ends in upd(), so without this the join would send the host an intent per box describing
    // the state the host has just sent us. The host's world is the reconciled authority, so the
    // gate never holds there.
    if (s->role() == coop::net::Role::Client &&
        !coop::join_membership_sweep::HasLoadTailQuiesced()) {
        return;
    }
    const auto now = Clock::now();
    auto it = g_lastPublish.find(box);
    if (it != g_lastPublish.end() && now - it->second < kCoalesce) { ++g_cCoalesced; return; }

    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(box);
    if (key.empty() || key == L"None") {
        // A box mid-construction: the key is minted inside the construction script, which calls
        // upd() itself. Nothing to address a record to, and the birth paths publish it anyway.
        return;
    }
    g_lastPublish[box] = now;
    // Host: the record to every peer. Client: an intent the host validates and re-publishes, which
    // is also the acknowledgement. Either way the far side's loadData applies it and calls upd(),
    // so `opened`, `drives_in` and the repaint all land together.
    if (coop::prop_save_data::Publish(s, box, key)) {
        ++g_cPublished;
        UE_LOGI("drivebox_record: published the box's record key='%ls' (%s)", key.c_str(),
                s->role() == coop::net::Role::Client ? "client intent" : "host broadcast");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (session && session->connected()) sg::SetEnabled(true);  // each lane asserts its own enable
    if (g_installed.load(std::memory_order_acquire)) return;
    if (!GT::IsGameThread()) return;

    void* cls = R::FindClass(L"prop_box_C");
    if (!cls) return;  // not loaded yet -- retry next tick
    void* fn = R::FindFunction(cls, L"upd");
    if (!fn) {
        UE_LOGW("drivebox_record: prop_box_C::upd UFunction not found -- the drive box's record "
                "will only travel on its birth paths (an in-place open stays local)");
        g_installed.store(true, std::memory_order_release);  // stop the retry
        return;
    }
    // The script gate, not a ProcessEvent observer: the box calls its own upd() from inside its
    // Blueprint, a route that never reaches ProcessEvent, and an observer there saw nothing. The
    // watch is on the exact UFunction rather than on the name `upd`, which half the props in the
    // game declare.
    if (!sg::Watch(fn, kTagDriveBox, nullptr, &OnUpdPost)) {
        UE_LOGW("drivebox_record: script-gate watch on prop_box_C::upd refused -- retrying");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("drivebox_record: watching prop_box_C::upd -- the drive box republishes its own save "
            "record (opened + drives_in) on every in-place change");
}

void OnDisconnect() {
    if (g_cPublished || g_cCoalesced || g_cRelooked)
        UE_LOGI("drivebox_record: session end -- published=%llu coalesced=%llu relooked=%llu",
                g_cPublished, g_cCoalesced, g_cRelooked);
    g_lastPublish.clear();
    g_cPublished = 0;
    g_cCoalesced = 0;
    g_cRelooked  = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::props::drivebox_record
