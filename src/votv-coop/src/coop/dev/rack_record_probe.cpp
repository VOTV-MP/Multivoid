// coop/dev/rack_record_probe.cpp -- [dev] every drive box's save record measured against the hand lane's bounds
// (VOTVCOOP_RACK_RECORD_PROBE=1 on the host), for VOTV-MP/Multivoid#192: a client's held drive box's record was refused
// "(bounds)" and the box it placed came back empty. 30 s after the host's session connects, then every 30 s: each
// prop_driveRack_C's record, its group counts, its longest text, its byte-free size and hand_record_sync::BoundsFault's
// verdict (`[RACKREC]`).

#include "coop/dev/rack_record_probe.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/hand_record_sync.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::rack_record_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace SR = ue_wrap::save_record;

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_RACK_RECORD_PROBE") == "1";
    return on;
}

template <class G>
std::string Counts(const G& groups) {
    std::string s = std::to_string(groups.size()) + "[";
    for (size_t i = 0; i < groups.size(); ++i) s += (i ? "," : "") + std::to_string(groups[i].size());
    return s + "]";
}

size_t Longest(const std::vector<std::vector<std::wstring>>& groups) {
    size_t n = 0;
    for (const auto& g : groups) for (const auto& s : g) n = (std::max)(n, s.size());
    return n;
}

void Report() {
    void* const cls = OI::ClassByName(L"prop_driveRack_C");
    if (!cls) return;
    std::vector<void*> racks;
    OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__"))
            static_cast<std::vector<void*>*>(ctx)->push_back(o);
    }, &racks);
    for (void* a : racks) {
        SR::SaveRecord rec;
        if (!SR::CaptureRecord(a, rec)) continue;
        const std::string fault = coop::hand_record_sync::BoundsFault(rec);
        UE_LOGI("[RACKREC] eid=%u key='%ls' bools=%s floats=%s ints=%s strings=%s (longest %zu) classes=%s (longest %zu) "
                "names=%s (longest %zu) vectors=%s bytes=%s signals=%zu nested=%zu -> %s",
                static_cast<unsigned>(coop::element::Registry::Get().EidForActor(a)), rec.key.c_str(),
                Counts(rec.bools).c_str(), Counts(rec.floats).c_str(), Counts(rec.ints).c_str(), Counts(rec.strings).c_str(),
                Longest(rec.strings), Counts(rec.classes).c_str(), Longest(rec.classes), Counts(rec.names).c_str(),
                Longest(rec.names), Counts(rec.vectors).c_str(), Counts(rec.bytes).c_str(), rec.signals.size(),
                rec.nested.size(), fault.empty() ? "within the bounds" : fault.c_str());
    }
}

uint64_t g_next = 0;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected() || session->role() != coop::net::Role::Host)
        return;
    const uint64_t now = ::GetTickCount64();
    if (!g_next) g_next = now + 30000;
    if (now < g_next) return;
    g_next = now + 30000;
    Report();
}

}  // namespace coop::dev::rack_record_probe
