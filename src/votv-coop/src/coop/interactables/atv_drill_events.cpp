// coop/interactables/atv_drill_events.cpp -- see coop/interactables/atv_drill_events.h.

#include "coop/interactables/atv_drill_events.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/core/reflection.h"

#include <cstring>
#include <mutex>
#include <string>

namespace coop::atv_drill_events {
namespace {

constexpr const wchar_t* kBatteryClass = L"prop_atvcarbattery_C";
constexpr uint32_t kRing = 256;   // a block at 20 Hz through a whole act (a 5 s drive is 100) before the decision reads

struct Ring {
    Rec recs[kRing];
    uint32_t count = 0;
};
std::mutex g_lock;
Ring g_ring[kEventCount];
uint32_t g_ord = 0;

}  // namespace

bool On() {
    static const bool on = coop::config::ResolveEnum(::coop::config_registry::rows::atv_gate_drill) != "off" ||
                           coop::config::ResolveEnum(::coop::config_registry::rows::atv_riders_drill) != "off";
    return on;
}

int32_t KeyTag(const std::wstring& key) {
    return static_cast<int32_t>(std::hash<std::wstring>{}(key) & 0x7fffffff);
}

void Note(Event e, int32_t i0, int32_t i1, int32_t i2, float f0, float f1, float f2, int32_t i3, float f3) {
    if (!On() || e < 0 || e >= kEventCount) return;
    std::lock_guard<std::mutex> hold(g_lock);
    Ring& r = g_ring[e];
    Rec& rec = r.recs[r.count % kRing];
    rec = Rec{++g_ord, i0, i1, i2, i3, f0, f1, f2, f3};
    ++r.count;
}

void NotePoseApplied(const std::wstring& key, void* actor, bool host, uint32_t riderEid, bool fromSyncer) {
    if (!On()) return;
    Note(kPoseApplied, KeyTag(key), host ? 0 : 1, ue_wrap::reflection::InternalIndexOf(actor), static_cast<float>(riderEid),
         fromSyncer ? 1.f : 0.f);
}

void NoteMirrorBound(const std::wstring& cls, void* actor) {
    if (!On() || cls != kBatteryClass) return;
    Note(kBatteryBound, ue_wrap::reflection::InternalIndexOf(actor));
}

void NoteRecordLanded(void* actor) {
    namespace R = ue_wrap::reflection;
    if (!On() || R::ClassNameOf(actor) != kBatteryClass) return;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), L"battery");
    float charge = -1.f;
    if (off >= 0) std::memcpy(&charge, static_cast<const uint8_t*>(actor) + off, sizeof(charge));
    Note(kBatteryCharged, R::InternalIndexOf(actor), 0, 0, charge);
}

uint32_t Count(Event e) {
    std::lock_guard<std::mutex> hold(g_lock);
    return e >= 0 && e < kEventCount ? g_ring[e].count : 0u;
}

bool Next(Event e, uint32_t& cursor, Rec& out) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (e < 0 || e >= kEventCount) return false;
    const Ring& r = g_ring[e];
    if (r.count - cursor > kRing) cursor = r.count - kRing;
    if (cursor >= r.count) return false;
    out = r.recs[cursor % kRing];
    ++cursor;
    return true;
}

bool AnyBefore(Event e, uint32_t cursor, uint32_t beforeOrd, bool (*pred)(const Rec&, int32_t), int32_t arg) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (e < 0 || e >= kEventCount) return false;
    const Ring& r = g_ring[e];
    if (r.count - cursor > kRing) cursor = r.count - kRing;
    for (; cursor < r.count; ++cursor) {
        const Rec& rec = r.recs[cursor % kRing];
        if (rec.ord < beforeOrd && pred(rec, arg)) return true;
    }
    return false;
}


}  // namespace coop::atv_drill_events
