// coop/dev/container_route_drill.cpp -- see coop/dev/container_route_drill.h.

#include "coop/dev/container_route_drill.h"
#include "container_route_drill_detail.h"

#include "coop/dev/container_route_census.h"
#include "coop/element/registry.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_slice_wire.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::container_route_drill {
namespace detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace ci = ue_wrap::container_inventory;
namespace SR = ue_wrap::save_record;
namespace cw = coop::props::container_slice_wire;

size_t g_largest = 0;
std::string g_largestClass;
uint64_t g_seamsSince = 0;   // the first SeamsGate ask of this session

}  // namespace

uint32_t EidOf(void* actor) {
    return actor ? static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor))
                 : static_cast<uint32_t>(coop::element::kInvalidId);
}

int32_t RecordCount(uint32_t eid) {
    int32_t n = -1;
    float vol = 0.f;
    return coop::props::container_contents_sync::ContentsDigest(eid, n, vol) ? n : -1;
}

void LogHashes(const char* role, uint32_t eid, const char* what) {
    std::vector<uint64_t> per;
    uint64_t whole = 0;
    if (!coop::props::container_contents_sync::DevRecordHashes(eid, per, whole)) {
        UE_LOGW("[CI-CENSUS] %s records (%s): eid=%u does not read", role, what, eid);
        return;
    }
    UE_LOGI("[CI-CENSUS] %s records (%s): eid=%u n=%zu slice hash=%016llx", role, what, eid, per.size(),
            static_cast<unsigned long long>(whole));
    for (size_t i = 0; i < per.size(); ++i)
        UE_LOGI("[CI-CENSUS] %s record %zu hash=%016llx", role, i, static_cast<unsigned long long>(per[i]));
}

void LogSizes(const char* role, const char* what, void* container) {
    void* inv = container ? ci::InventoryOf(container) : nullptr;
    std::vector<SR::SaveRecord> recs;
    if (!inv || !ci::ReadContents(inv, recs, cw::kMaxRecords)) {
        UE_LOGW("[CI-CENSUS] %s record sizes (%s): the contents do not read", role, what);
        return;
    }
    UE_LOGI("[CI-CENSUS] %s record sizes (%s): n=%zu", role, what, recs.size());
    for (const SR::SaveRecord& r : recs) {
        std::vector<uint8_t> bytes;
        coop::save_record_wire::SerSave(bytes, r);
        std::string cls(r.className.begin(), r.className.end());
        UE_LOGI("[CI-CENSUS] record %s bytes=%zu", cls.c_str(), bytes.size());
        if (bytes.size() > g_largest) {
            g_largest = bytes.size();
            g_largestClass = cls;
        }
    }
}

void LogLargest(const char* role) {
    if (g_largest)
        UE_LOGI("[CI-CENSUS] %s record largest %s bytes=%zu", role, g_largestClass.c_str(), g_largest);
}

void Abandon(const char* role, const char* why) {
    UE_LOGW("[CI-CENSUS] drill ABANDONED on the %s: %s", role, why);
}

void RouteFail(const char* phase) {
    namespace cen = coop::dev::container_route_census;
    UE_LOGE("[CI-CENSUS] %s FAIL: %s", cen::IsBeforeOnly() ? "drill-before" : "drill", phase);
}

void* Screen() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"propInventory") : -1;
    void* ui = nullptr;
    if (off >= 0) ui = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + off);
    return ui && R::IsLive(ui) ? ui : nullptr;
}

int SlotCount(void* screen) {
    const int32_t off = screen ? R::FindPropertyOffset(R::ClassOf(screen), L"slots_prop") : -1;
    return off >= 0 ? SR::ReadArr(screen, off).num : 0;
}

void* SlotAt(void* screen, int i) {
    const int32_t off = screen ? R::FindPropertyOffset(R::ClassOf(screen), L"slots_prop") : -1;
    if (off < 0) return nullptr;
    const SR::Arr arr = SR::ReadArr(screen, off);
    if (i < 0 || i >= arr.num || !arr.data) return nullptr;
    void* slot = nullptr;
    std::memcpy(&slot, arr.data + static_cast<size_t>(i) * sizeof(void*), sizeof(slot));
    return slot && R::IsLive(slot) ? slot : nullptr;
}

std::string FirstSlotText() {
    void* slot = SlotAt(Screen(), 0);
    const int32_t nameOff = slot ? R::FindPropertyOffset(R::ClassOf(slot), L"prop") : -1;
    const int32_t amountOff = slot ? R::FindPropertyOffset(R::ClassOf(slot), L"amount") : -1;
    if (nameOff < 0 || amountOff < 0) return {};
    R::FName name{};
    int32_t amount = 0;
    std::memcpy(&name, static_cast<const uint8_t*>(slot) + nameOff, sizeof(name));
    std::memcpy(&amount, static_cast<const uint8_t*>(slot) + amountOff, sizeof(amount));
    const std::wstring w = R::ToString(name);
    char text[96];
    std::snprintf(text, sizeof(text), "prop=%s amount=%d", std::string(w.begin(), w.end()).c_str(), amount);
    return text;
}

std::string SlotChange(const std::string& before, const std::string& after) {
    if (after.empty()) return {};
    return " first slot " + (before.empty() ? std::string("?") : before) + " -> " + after;
}

bool CallNoArg(void* obj, const wchar_t* fn) {
    void* f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

bool IntAfter(const std::string& text, const char* key, int& out) {
    const std::string needle = std::string(key) + "=";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    const char* begin = text.c_str() + at + needle.size();
    char* end = nullptr;
    const long v = std::strtol(begin, &end, 10);
    if (end == begin) return false;
    out = static_cast<int>(v);
    return true;
}

void ResetSeamsGate() { g_seamsSince = 0; }

SeamGate SeamsGate(const char* role) {
    if (!g_seamsSince) g_seamsSince = ::GetTickCount64();
    const char* dead = nullptr;
    switch (coop::dev::container_route_census::SeamsState(&dead)) {
        case coop::dev::container_route_census::Seams::Live:
            g_seamsSince = 0;
            return SeamGate::Go;
        case coop::dev::container_route_census::Seams::Dead: {
            char why[96];
            std::snprintf(why, sizeof(why), "seam %s is dead", dead ? dead : "?");
            Abandon(role, why);
            return SeamGate::Dead;
        }
        case coop::dev::container_route_census::Seams::Pending: break;
    }
    if (::GetTickCount64() - g_seamsSince > kSeamBoundMs) {
        Abandon(role, "the census's seams never settled");
        return SeamGate::Dead;
    }
    return SeamGate::Wait;
}

bool ScreenRebuilt(uint64_t gen0, void* slot0) {
    return coop::dev::container_route_census::GenCalls() > gen0 || SlotAt(Screen(), 0) != slot0;
}

int LiveDrives() {
    int n = 0;
    void* cls = ue_wrap::object_index::ClassByName(kDriveClass);
    if (!cls) return 0;
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++*static_cast<int*>(c);
    }, &n);
    return n;
}

}  // namespace detail

void Tick(coop::net::Session* s) {
    static const bool on = coop::dev::container_route_census::IsDrill();
    if (!on || !s) return;
    if (s->role() == coop::net::Role::Host) detail::HostTick(s);
    else if (s->connected()) detail::ClientTick(s);
}

void OnDisconnect() {
    if (!coop::dev::container_route_census::IsDrill()) return;
    detail::HostReset();
    detail::ClientReset();
    detail::ResetSeamsGate();
}

}  // namespace coop::dev::container_route_drill
