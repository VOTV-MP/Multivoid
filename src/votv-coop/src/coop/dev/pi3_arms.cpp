// coop/dev/pi3_arms.cpp -- the PI-3 drill's arm table, the tests its verdicts apply to the observer's report and the
// lines it prints. See coop/dev/pi3_drill.h; the phases are pi3_drill.cpp's.

#include "pi3_drill_internal.h"  // the table's type and the tests (src-local)

#include "coop/dev/pi3_observer.h"
#include "coop/player/puppet_identity.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"

#include <cstdio>
#include <cstring>

namespace coop::dev::pi3_drill::detail {
namespace {

// The verbs the observer cancels when attributed (pi3_observer.h): a call of one of them that was not cancelled
// is the instrument's fault, whatever the machine does next.
const char* const kCancelList[] = {
    "lib_C::loadLevel", "lib_C::end", "lib_C::setSettings", "saveSlot_C::save", "save_main_C::save",
    "mainGamemode_C::save", "QuitGame",
};

const char* const kRowEnd = "lib_C::end";
const char* const kQuit = "QuitGame";
const char* const kRewind = "daynightCycle_C::rewind";

// Row 1's verbs, which the client arm's first leg shares.
#define PI3_WISP_W_JUDGED \
    { "saveSlot_C::save", "save_main_C::save", kQuit, "gib", "SetControlRotation" }

// Each horizon is twice the slowest judged verb of the arm's complete red (lane pi3red), at least
// 1000 ms: wisp_w 1985, orborb 5094, the client arm's wisp_w 2000, the rest at the contact itself.
// The paths are the object paths of the class dumps (VotV/Content/<x>/<y>.<y>_C is /Game/<x>/<y>.<y>_C).
const Spec kSpecs[kSpecCount] = {
    // The row arms: the host spawns, the client's puppet is the site.
    {"wisp_w", L"wisp_w_C", L"/Game/objects/wisp_w.wisp_w_C", "wisp_w", PI3_WISP_W_JUDGED, {}, nullptr,
     true, 3970},
    {"wisp_o", L"wisp_o_C", L"/Game/objects/wisp_o.wisp_o_C", "wisp_o", {"lib_C::setSettings"}, {}, nullptr,
     true, 1000},
    {"wisp_p", L"wisp_p_C", L"/Game/objects/wisp_p.wisp_p_C", "wisp_p", {"PlaySound2D", "AddToViewport"},
     {kRewind}, "rewind", true, 1000},
    {"kelura", L"kelura_C", L"/Game/objects/kelura.kelura_C", "kelura", {kRowEnd, kQuit}, {}, nullptr, true,
     1000},
    // The orborb ends the game in its own BeginPlay unless its spawner's string is stamped first
    // (ue_wrap/actors/spawner_stamp.h): the drill's spawn stamps it as every arm's does.
    {"orborb", L"npc_orborb_C", L"/Game/objects/npc/npc_orborb.npc_orborb_C", "orborb",
     {kRowEnd, kQuit}, {}, nullptr, true, 10188},
    // The orb's birth on the still peer (no verdict of the spawner's: the host holds the orb alive for the rig's
    // join): no judged verbs, no horizon. The counter is the orborb's, which the host reads and no one judges.
    {"orbbirth", L"npc_orborb_C", L"/Game/objects/npc/npc_orborb.npc_orborb_C", "orborb", {}, {}, nullptr, true, 0},
    {"grunt", L"prop_grunt_C", L"/Game/objects/prop_grunt.prop_grunt_C", "grunt", {"SetControlRotation"}, {},
     nullptr, true, 1000},
    // The control: this machine's own player IS served, as in single player, and the verbs are cancelled by the drill.
    {"control", L"kelura_C", L"/Game/objects/kelura.kelura_C", "kelura", {}, {kRowEnd, kQuit}, "served",
     false, 1000},
    // The client arm: wisp_w at the host's puppet, wisp_p at the host's puppet (whole on a client), wisp_p at the
    // client's own pawn (a reading).
    {"client", L"wisp_w_C", L"/Game/objects/wisp_w.wisp_w_C", "wisp_w", PI3_WISP_W_JUDGED, {}, nullptr, true,
     4000},
    {"client", L"wisp_p_C", L"/Game/objects/wisp_p.wisp_p_C", "wisp_p",
     {kRewind, "PlaySound2D", "AddToViewport"}, {}, nullptr, true, 1000},
    {"client", L"wisp_p_C", L"/Game/objects/wisp_p.wisp_p_C", "wisp_p", {}, {}, nullptr, false,
     1000},
};

#undef PI3_WISP_W_JUDGED

bool InList(const char* const* list, const char* verb) {
    for (int i = 0; i < kMaxVerbs && list[i]; ++i)
        if (std::strcmp(list[i], verb) == 0) return true;
    return false;
}

}  // namespace

const Spec& SpecOf(int id) { return kSpecs[id >= 0 && id < kSpecCount ? id : 0]; }

ArmId ArmFromName(const char* name) {
    static const struct { const char* name; ArmId arm; } kArms[] = {
        {"wisp_w", ArmId::WispW}, {"wisp_o", ArmId::WispO}, {"wisp_p", ArmId::WispP}, {"kelura", ArmId::Kelura},
        {"orborb", ArmId::Orborb}, {"orbbirth", ArmId::OrbBirth}, {"grunt", ArmId::Grunt},
        {"control", ArmId::Control}, {"client", ArmId::Client},
    };
    for (const auto& a : kArms)
        if (name && std::strcmp(a.name, name) == 0) return a.arm;
    return ArmId::Off;
}

const char* ArmName(ArmId arm) {
    switch (arm) {
    case ArmId::WispW: return "wisp_w";
    case ArmId::WispO: return "wisp_o";
    case ArmId::WispP: return "wisp_p";
    case ArmId::Kelura: return "kelura";
    case ArmId::Orborb: return "orborb";
    case ArmId::OrbBirth: return "orbbirth";
    case ArmId::Grunt: return "grunt";
    case ArmId::Control: return "control";
    case ArmId::Client: return "client";
    case ArmId::Off: break;
    }
    return "off";
}

int SpecIdOf(ArmId arm, int leg) {
    switch (arm) {
    case ArmId::WispW: return kSpecWispW;
    case ArmId::WispO: return kSpecWispO;
    case ArmId::WispP: return kSpecWispP;
    case ArmId::Kelura: return kSpecKelura;
    case ArmId::Orborb: return kSpecOrborb;
    case ArmId::OrbBirth: return kSpecOrbBirth;
    case ArmId::Grunt: return kSpecGrunt;
    case ArmId::Control: return kSpecControl;
    case ArmId::Client: return leg == 3 ? kSpecClientReading : leg == 2 ? kSpecClientWispP : kSpecClientWispW;
    case ArmId::Off: break;
    }
    return kSpecWispW;
}

void* ResolveClass(const Spec& spec) {
    void* cls = ue_wrap::object_index::ClassByName(spec.cls);
    return cls ? cls : ue_wrap::asset_load::LoadObjectByPath(spec.path);
}

uint32_t Counter(const char* row) {
    int n = 0;
    const coop::puppet_identity::RowCounter* counters = coop::puppet_identity::Counters(n);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(counters[i].row, row) == 0) return counters[i].refused;
    return 0;
}

const Seen* Find(const Seen* seen, int n, const char* verb) {
    for (int i = 0; i < n; ++i)
        if (std::strcmp(seen[i].verb, verb) == 0) return &seen[i];
    return nullptr;
}

const char* FirstJudged(const Spec& spec, const Seen* seen, int n) {
    const Seen* first = nullptr;
    for (int i = 0; i < n; ++i)
        if (InList(spec.judged, seen[i].verb) && (!first || seen[i].firstMs < first->firstMs)) first = &seen[i];
    return first ? first->verb : nullptr;
}

const char* FirstUncancelled(const Seen* seen, int n) {
    for (int i = 0; i < n; ++i)
        for (const char* verb : kCancelList)
            if (std::strcmp(seen[i].verb, verb) == 0 && seen[i].count > seen[i].cancelled) return seen[i].verb;
    return nullptr;
}

bool BirthGuardReached(const Seen* seen, int n) { return Find(seen, n, kRowEnd) || Find(seen, n, kQuit); }

bool Complete(const Spec& spec, const Seen* seen, int n) {
    for (int i = 0; i < kMaxVerbs && spec.judged[i]; ++i)
        if (!Find(seen, n, spec.judged[i])) return false;
    for (int i = 0; i < kMaxVerbs && spec.required[i]; ++i)
        if (!Find(seen, n, spec.required[i])) return false;
    return true;
}

const char* Unmet(const Spec& spec, const Seen* seen, int n, bool counterOk) {
    if (!counterOk) return "counter";
    for (int i = 0; i < kMaxVerbs && spec.required[i]; ++i)
        if (!Find(seen, n, spec.required[i])) return spec.unmet;
    return nullptr;
}

void Label(char* out, size_t cap, const Spec& spec, int leg) {
    if (leg > 0) std::snprintf(out, cap, "%s leg=%d", spec.label, leg);
    else std::snprintf(out, cap, "%s", spec.label);
}

void FormatVerbs(char* out, size_t cap, const Seen* seen, int n) {
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < n && used + 1 < cap; ++i) {
        const int w = std::snprintf(out + used, cap - used, "%s%s=%d/%d@%d", i ? " " : "", seen[i].verb,
                                    seen[i].count, seen[i].cancelled, seen[i].firstMs);
        if (w < 0 || static_cast<size_t>(w) >= cap - used) break;
        used += static_cast<size_t>(w);
    }
}

void LogSeen(const char* label, bool complete, const Seen* seen, int n, int puppet, int own) {
    char verbs[512];
    FormatVerbs(verbs, sizeof verbs, seen, n);
    UE_LOGI("[PI-3] seen %s role=spawner complete=%d %s contacts=%d/%d", label, complete ? 1 : 0, verbs, puppet, own);
}

void LogStill(const char* arm, const Seen* seen, int n, int own) {
    char verbs[512];
    FormatVerbs(verbs, sizeof verbs, seen, n);
    UE_LOGI("[PI-3] STILL %s %s contacts=%d", arm, verbs, own);
}

}  // namespace coop::dev::pi3_drill::detail
