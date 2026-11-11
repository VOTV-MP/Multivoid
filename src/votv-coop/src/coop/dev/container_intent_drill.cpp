// coop/dev/container_intent_drill.cpp -- see coop/dev/container_intent_drill.h.

#include "coop/dev/container_intent_drill.h"
#include "container_intent_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/props/container_contents_sync.h"
#include "coop/session/world_ready_answer.h"

#include "ue_wrap/core/log.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace coop::dev::container_intent_drill {

namespace CC = coop::props::container_contents_sync;

namespace detail {

namespace {
struct ArmRow {
    const char* name;
    Arm arm;
};
constexpr ArmRow kArms[] = {{"throwhand", Arm::ThrowHand}, {"throwgrab", Arm::ThrowGrab},
                            {"throwhostgrab", Arm::ThrowHostGrab}, {"throwclamp", Arm::ThrowClamp},
                            {"throwplace", Arm::ThrowPlace}, {"throwjoin", Arm::ThrowJoin},
                            {"throwdepart", Arm::ThrowDepart}, {"throwtouch", Arm::ThrowTouch}};

bool g_done = false;
}  // namespace

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::container_intent_drill);
        for (const ArmRow& r : kArms)
            if (v == r.name) return r.arm;
        return Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    for (const ArmRow& r : kArms)
        if (r.arm == ArmOf()) return r.name;
    return "off";
}

const char* Role() { return coop::roster::LocalIsHost() ? "host" : "client"; }

void Finish() {
    g_done = true;
    UE_LOGI("[CI-DRILL] %s DONE", Role());
}

void Fail(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGW("[CI-DRILL] %s %s FAIL: %s", Role(), ArmName(), buf);
    Finish();
}

int CountOf(uint32_t eid) {
    int32_t n = -1;
    float vol = 0.f;
    return CC::ContentsDigest(eid, n, vol) ? n : -1;
}

uint32_t EidOf(void* actor) {
    return actor ? static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor))
                 : static_cast<uint32_t>(coop::element::kInvalidId);
}

bool IsJoiner() {
    coop::roster::Snapshot r;
    coop::roster::GetSnapshot(r);
    int mine = -1;
    for (int i = 0; i < r.count; ++i)
        if (r.rows[i].isLocal) mine = r.rows[i].slot;
    if (mine < 0) return false;
    bool joiner = false;
    for (int i = 0; i < r.count; ++i) {
        const coop::roster::Row& row = r.rows[i];
        if (row.connected && !row.isHost && !row.isLocal && row.slot >= 1 && row.slot < mine) joiner = true;
    }
    return joiner;
}

bool Done() { return g_done; }
void ClearDone() { g_done = false; }

}  // namespace detail

using namespace detail;

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled()) return;
    if (session) coop::world_ready_answer::dev::Tick(*session);   // the throwjoin hold's answers and its bound, whatever the phase
    if (Done() || !session || !session->connected()) return;
    if (coop::roster::LocalIsHost()) ThrowHostTick(*session);
    else ThrowClientTick(*session);
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    ThrowReset();
    ClearDone();
}

}  // namespace coop::dev::container_intent_drill
