// coop/creatures/walker_state_client.cpp -- see coop/creatures/walker_state_client.h.

#include "coop/creatures/walker_state_client.h"

#include "coop/element/death_seam.h"
#include "coop/net/protocol.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::walker_state_client {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace DS = coop::element::death_seam;
namespace OI = ue_wrap::object_index;
namespace R = ue_wrap::reflection;

constexpr wchar_t kClassName[] = L"prop_wMannequin_C";
constexpr size_t  kRows = 8;

// One walker's row. `actor` is the copy it was applied to (null while the state is held), named by its slot index and serial so a
// copy that ended is never taken for another born at its address.
struct Row {
    bool                used = false;
    coop::net::WireKey  key{};
    uint8_t             looking = 1;
    uint8_t             target = 0xFF;
    void*               actor = nullptr;
    int32_t             index = -1;
    int32_t             serial = 0;
    uint32_t            born = 0;  // the order rows were made in
};
Row      g_rows[kRows];
size_t   g_count = 0;
uint32_t g_born = 0;
bool     g_fullSaid = false;

bool     g_endSubscribed = false;
uint64_t g_setAsked = ~uint64_t{0};

constexpr size_t kMaxSubs = 2;
HuntFn           g_subs[kMaxSubs];
size_t           g_subCount = 0;

bool SameKey(const coop::net::WireKey& a, const coop::net::WireKey& b) {
    return a.len == b.len && std::memcmp(a.data, b.data, a.len) == 0;
}

Row* Find(const coop::net::WireKey& key) {
    for (Row& r : g_rows)
        if (r.used && SameKey(r.key, key)) return &r;
    return nullptr;
}

// A row for `key`: the one held, a free one, or, with the table full, the oldest that has no copy.
Row* Make(const coop::net::WireKey& key) {
    Row* take = nullptr;
    for (Row& r : g_rows) {
        if (!r.used) {
            take = &r;
            break;
        }
        if (!r.actor && (!take || r.born < take->born)) take = &r;
    }
    if (!take) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("walker_state_client: %zu walkers hold a copy here -- another one's state is dropped (said once)", kRows);
        }
        return nullptr;
    }
    if (take->used && !g_fullSaid) {
        g_fullSaid = true;
        UE_LOGW("walker_state_client: the table of %zu is full -- the oldest held state is replaced (said once)", kRows);
    }
    if (!take->used) ++g_count;
    *take = Row{};
    take->used = true;
    take->key = key;
    take->born = ++g_born;
    return take;
}

void Drop(Row& r) {
    r = Row{};
    if (g_count > 0) --g_count;
}

void Notify(const Row& r, bool ended) {
    for (size_t i = 0; i < g_subCount; ++i) g_subs[i](r.key, r.actor, r.target, ended);
}

// Write the row's pose onto its copy, and tell the hunt's listeners.
void Apply(Row& r) {
    if (!r.actor) return;
    if (AC::WriteWalkerStanding(r.actor, r.looking != 0))
        UE_LOGI("walker_state: applied to the copy of '%ls' looking=%d target=%d", coop::net::StringFromWireKey(r.key).c_str(),
                r.looking, r.target == 0xFF ? -1 : static_cast<int>(r.target));
    else
        UE_LOGW("walker_state: the copy of '%ls' has no anim stand to write", coop::net::StringFromWireKey(r.key).c_str());
    Notify(r, false);
}

void Attach(Row& r, void* actor) {
    r.actor = actor;
    r.index = R::InternalIndexOf(actor);
    r.serial = R::SlotSerial(r.index);
}

bool StillThere(const Row& r) {
    return r.actor && R::SlotSerial(r.index) == r.serial && R::InternalIndexOf(r.actor) == r.index && R::IsLive(r.actor);
}

// The live copy that wears `key`, or null.
void* CopyOf(const coop::net::WireKey& key) {
    const std::wstring want = coop::net::StringFromWireKey(key);
    std::vector<void*> walkers;
    AC::ListWalkers(walkers);
    for (void* w : walkers)
        if (ue_wrap::prop::GetKeyString(w) == want) return w;
    return nullptr;
}

// A copy ended: its row goes with it.
void OnCopyEnd(const DS::ActorEnd& end) {
    for (Row& r : g_rows) {
        if (!r.used || r.actor != end.actor) continue;
        Notify(r, true);
        Drop(r);
        return;
    }
}

}  // namespace

void OnState(const coop::net::WalkerStatePayload& p) {
    Row* r = Find(p.key);
    if (!r) r = Make(p.key);
    if (!r) return;
    r->looking = p.looking;
    r->target = p.target;
    if (!StillThere(*r)) {
        r->actor = nullptr;
        if (void* copy = CopyOf(p.key)) Attach(*r, copy);
    }
    if (!r->actor) {
        UE_LOGI("walker_state: held for '%ls' (no copy yet) looking=%d target=%d", coop::net::StringFromWireKey(p.key).c_str(),
                p.looking, p.target == 0xFF ? -1 : static_cast<int>(p.target));
        return;
    }
    Apply(*r);
}

void OnBound(void* actor, const std::wstring& key) {
    if (g_count == 0 || !actor || !AC::IsWalker(actor)) return;
    coop::net::WireKey k{};
    coop::net::WireKeyFromString(key, k);
    Row* r = Find(k);
    if (!r) return;
    Attach(*r, actor);
    Apply(*r);
}

uint8_t TargetOfCopy(void* copy) {
    for (const Row& r : g_rows)
        if (r.used && r.actor == copy && StillThere(r)) return r.target;
    return 0xFF;
}

bool RowOf(const std::wstring& key, uint8_t& target, bool& looking) {
    coop::net::WireKey k{};
    coop::net::WireKeyFromString(key, k);
    const Row* r = Find(k);
    if (!r) return false;
    target = r->target;
    looking = r->looking != 0;
    return true;
}

bool SubscribeHunt(HuntFn fn) {
    if (!fn) return false;
    for (size_t i = 0; i < g_subCount; ++i)
        if (g_subs[i] == fn) return false;
    if (g_subCount >= kMaxSubs) return false;
    g_subs[g_subCount++] = fn;
    return true;
}

void Tick() {
    if (g_endSubscribed) return;
    const uint64_t set = OI::ClassSetVersion();
    if (set == g_setAsked) return;
    g_setAsked = set;
    g_endSubscribed = DS::Install() && DS::SubscribeClass(kClassName, &OnCopyEnd);
}

void OnDisconnect() {
    for (Row& r : g_rows) r = Row{};
    g_count = 0;
    g_born = 0;
    g_fullSaid = false;
}

}  // namespace coop::walker_state_client
