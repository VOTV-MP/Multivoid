// coop/dev/stats_probe.cpp -- see coop/dev/stats_probe.h.

#include "coop/dev/stats_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/command_drill.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/effects.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::dev::stats_probe {
namespace {

namespace V = ue_wrap::vitals;
namespace E = ue_wrap::effects;

enum class Arm : uint8_t { Off, On, Red };

Arm ArmNow() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::stats_probe);
        return v == "on" ? Arm::On : v == "red" ? Arm::Red : Arm::Off;
    }();
    return arm;
}

constexpr float kTolerance = 0.001f;
constexpr float kTestStep = 7.f;    // a writable float row is written its value plus this
constexpr float kSnapshotBase = 1000.f;  // snapshot row i is written 1000 + i

bool g_done = false;
int g_bad = 0;  // MISMATCH and NOT FOUND lines
int g_ok = 0;

bool Near(float a, float b) { return std::fabs(a - b) <= kTolerance; }

void Good() { ++g_ok; }
void Bad() { ++g_bad; }

void NotFound(const char* what) {
    UE_LOGE("[STATS-PROBE] %s NOT FOUND", what);
    Bad();
}

// A writable row: read, write, read back, restore, read again. A refused or unreadable step is a
// NOT FOUND. Every attempted write is followed by a restore.
void ProbeWritable(V::Field f, bool red) {
    const V::Row& row = V::RowOf(f);
    float orig = 0.f, back = 0.f, restored = 0.f;
    if (!V::Read(f, &orig)) return NotFound(row.name);
    const float test = row.type == V::Type::Bool ? 1.f - orig : orig + kTestStep;
    const float expected = (red && f == V::Field::Health) ? orig + kTestStep + 1.f : test;
    const bool wrote = V::Write(f, test);
    const bool readBack = wrote && V::Read(f, &back);
    V::Write(f, orig);
    if (!wrote) return NotFound(row.name);
    const bool readRestored = V::Read(f, &restored);
    if (!readBack || !readRestored) return NotFound(row.name);
    const bool ok = Near(back, expected) && Near(restored, orig);
    if (ok) UE_LOGI("[STATS-PROBE] %s read=%.3f wrote=%.3f back=%.3f restored=%.3f OK", row.name, orig, test, back, restored);
    else UE_LOGE("[STATS-PROBE] %s read=%.3f wrote=%.3f back=%.3f restored=%.3f MISMATCH", row.name, orig, test, back, restored);
    ok ? Good() : Bad();
}

void ProbeReadOnly(V::Field f) {
    const V::Row& row = V::RowOf(f);
    float v = 0.f;
    if (!V::Read(f, &v)) return NotFound(row.name);
    UE_LOGI("[STATS-PROBE] %s read=%.3f OK", row.name, v);
    Good();
}

// The snapshot's seven members against their rows: each row is written a distinct value, the
// snapshot is read, and each member must hold its own row's value. The pairing is written out here
// on purpose: it is the instrument for SnapshotField's switch, so a swapped pair reads back crossed.
void ProbeSnapshot() {
    constexpr size_t kRows = 7;
    float orig[kRows];
    for (size_t i = 0; i < kRows; ++i)
        if (!V::Read(static_cast<V::Field>(i), &orig[i])) return NotFound("snapshot");
    // Write refuses a non-finite value, so a stat that is already one could not be restored.
    for (size_t i = 0; i < kRows; ++i) {
        if (std::isfinite(orig[i])) continue;
        UE_LOGE("[STATS-PROBE] snapshot MISMATCH: %s holds a non-number, not probed",
                V::RowOf(static_cast<V::Field>(i)).name);
        return Bad();
    }
    bool written[kRows] = {};
    for (size_t i = 0; i < kRows; ++i)
        written[i] = V::Write(static_cast<V::Field>(i), kSnapshotBase + static_cast<float>(i));
    V::Snapshot s;
    const bool haveSnapshot = V::ReadSnapshot(s);
    for (size_t i = 0; i < kRows; ++i)
        V::Write(static_cast<V::Field>(i), orig[i]);  // a refused write stored nothing; a failed one may have
    bool allWritten = true;
    for (size_t i = 0; i < kRows; ++i) allWritten = allWritten && written[i];
    if (!haveSnapshot || !allWritten) return NotFound("snapshot");

    struct Pair { const char* member; float held; V::Field row; };
    const Pair pairs[kRows] = {
        {"health", s.health, V::Field::Health},
        {"maxHealth", s.maxHealth, V::Field::MaxHealth},
        {"food", s.food, V::Field::Food},
        {"sleep", s.sleep, V::Field::Sleep},
        {"coffeePower", s.coffeePower, V::Field::CoffeePower},
        {"gasolinepilled", s.gasolinepilled, V::Field::GasolinePilled},
        {"strength", s.strength, V::Field::Strength},
    };
    for (size_t i = 0; i < kRows; ++i) {
        const float wrote = kSnapshotBase + static_cast<float>(static_cast<size_t>(pairs[i].row));
        if (Near(pairs[i].held, wrote)) continue;
        UE_LOGE("[STATS-PROBE] snapshot MISMATCH: %s holds %.3f, its row %s wrote %.3f", pairs[i].member,
                pairs[i].held, V::RowOf(pairs[i].row).name, wrote);
        return Bad();
    }
    UE_LOGI("[STATS-PROBE] snapshot OK");
    Good();
}

// What the table refuses: a write to a read-only row, a non-finite write, an id past the last row.
// A refusal that was accepted instead is written back, so a regressed guard leaves the game as it was.
void ProbeRefusals() {
    const char* which = nullptr;
    float before = 0.f, after = 0.f, deadBefore = 0.f;
    V::Field id = V::Field::Health;
    const bool deadRead = V::Read(V::Field::Dead, &deadBefore);
    if (V::Write(V::Field::Dead, 1.f)) {
        which = "a write to the read-only Dead was accepted";
        if (deadRead) V::Write(V::Field::Dead, deadBefore);
    }
    else if (!V::Read(V::Field::Health, &before)) which = "Health did not read";
    else if (V::Write(V::Field::Health, std::nanf(""))) {
        which = "a NaN write to Health was accepted";
        V::Write(V::Field::Health, before);
    }
    else if (!V::Read(V::Field::Health, &after) || !Near(before, after)) which = "a refused NaN write changed Health";
    else if (V::FieldFromId(static_cast<uint8_t>(V::Field::Count), &id)) which = "id 20 was accepted";
    else if (!V::FieldFromId(static_cast<uint8_t>(V::Field::Glasses), &id)) which = "id 19 was refused";
    if (!which) {
        UE_LOGI("[STATS-PROBE] refusals OK");
        return Good();
    }
    UE_LOGE("[STATS-PROBE] refusals MISMATCH: %s", which);
    Bad();
}

// The pawn class's and the save class's own defaults, read through the rows' offsets off their class default
// objects.
void ProbeDefaults() {
    V::Snapshot d;
    if (!V::ReadDefaults(d)) return NotFound("defaults");
    const bool ok = std::isfinite(d.health) && d.health > 0.f && std::isfinite(d.maxHealth) && d.maxHealth > 0.f &&
                    std::isfinite(d.food) && std::isfinite(d.sleep);
    if (ok) UE_LOGI("[STATS-PROBE] defaults health=%.3f maxhealth=%.3f food=%.3f sleep=%.3f OK", d.health, d.maxHealth, d.food, d.sleep);
    else UE_LOGE("[STATS-PROBE] defaults health=%.3f maxhealth=%.3f food=%.3f sleep=%.3f MISMATCH", d.health, d.maxHealth, d.food, d.sleep);
    ok ? Good() : Bad();
}

// ---- the effects leg ---------------------------------------------------------------------------

constexpr float kEffectStrength = 1.f;  // what an added effect is given, and what List must report
constexpr float kEffectSeconds = 5.f;

// The effects the probe adds: the removal of the one instance an add makes leaves nothing behind.
const wchar_t* const kEffectsAdded[] = {L"bloodLoss", L"foodPoison", L"nausea", L"sleepy", L"vaccine_a"};

// The effects the probe only lists by name, and what a removal of each would leave.
struct ListedOnly { const wchar_t* name; const char* leaves; };
const ListedOnly kEffectsListed[] = {
    {L"lsd", "the lsd reverb on"},
    {L"vaccine", "a widget on screen"},
    {L"poo", "a poo prop"},
};

bool SameName(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

// How many entries of `name` the list holds, and the first of them.
int CountOf(const std::vector<E::Entry>& list, const std::wstring& name, const E::Entry** first) {
    int n = 0;
    for (const E::Entry& e : list) {
        if (!SameName(e.name, name.c_str())) continue;
        if (n == 0 && first) *first = &e;
        ++n;
    }
    return n;
}

// The entries of `name` as "[live strength time]" each, for a MISMATCH that has to say what the list held.
std::string Describe(const std::vector<E::Entry>& list, const std::wstring& name) {
    std::string s;
    for (const E::Entry& e : list) {
        if (!SameName(e.name, name.c_str())) continue;
        char b[64];
        std::snprintf(b, sizeof(b), "[%s %.2f %.2f]", e.live ? "live" : "dead", e.strength, e.time);
        s += b;
    }
    return s.empty() ? std::string("none") : s;
}

void EffectNotFound(const std::wstring& name) {
    UE_LOGE("[STATS-PROBE] effect %ls NOT FOUND", name.c_str());
    Bad();
}

// One effect of the add list: skipped when it is already active (an add would merge into it, or
// stack beside it), else added, listed, removed, listed.
void ProbeEffectAdded(const std::wstring& name, bool red) {
    std::vector<E::Entry> list;
    if (!E::List(&list)) return EffectNotFound(name);
    if (CountOf(list, name, nullptr) > 0) {
        UE_LOGI("[STATS-PROBE] effect %ls SKIPPED (already active)", name.c_str());
        return;
    }
    if (!E::Add(name, kEffectStrength, kEffectSeconds)) return EffectNotFound(name);
    const bool listedAdded = E::List(&list);
    const E::Entry* first = nullptr;
    const int added = listedAdded ? CountOf(list, name, &first) : 0;
    const float strength = first ? first->strength : 0.f;
    const float time = first ? first->time : 0.f;
    const int expected = (red && SameName(name, L"bloodLoss")) ? 2 : 1;
    const std::string addedList = Describe(list, name);   // before the removal changes `list`
    const bool addedOk = added == expected && first && first->live && Near(strength, kEffectStrength) &&
                         Near(time, kEffectSeconds);
    // The removal runs whatever the add read back, and takes the one instance an add makes. An add
    // that unexpectedly made two (a MISMATCH), or a failed List after the add, leaves one.
    const bool removed = E::Remove(name);
    const bool listedAfter = E::List(&list);
    if (!listedAdded || !removed || !listedAfter) return EffectNotFound(name);
    const int after = CountOf(list, name, nullptr);
    const bool ok = addedOk && after == 0;
    if (ok) UE_LOGI("[STATS-PROBE] effect %ls after-add=%d strength=%.3f time=%.3f after-remove=%d OK", name.c_str(), added, strength, time, after);
    else UE_LOGE("[STATS-PROBE] effect %ls after-add=%d strength=%.3f time=%.3f after-remove=%d MISMATCH -- the list held %s after the add, holds %s after the removal", name.c_str(), added, strength, time, after, addedList.c_str(), Describe(list, name).c_str());
    ok ? Good() : Bad();
}

// Every effect the game's table names: the add list through its whole cycle, the list-only ones by
// name, and any other a game update added as a MISMATCH, since nobody has vetted its removal.
void ProbeEffects(bool red) {
    std::vector<std::wstring> names;
    if (!E::Names(&names)) return NotFound("effects");
    for (const std::wstring& name : names) {
        bool added = false;
        for (const wchar_t* a : kEffectsAdded) added = added || SameName(name, a);
        if (added) {
            ProbeEffectAdded(name, red);
            continue;
        }
        const ListedOnly* listed = nullptr;
        for (const ListedOnly& l : kEffectsListed)
            if (SameName(name, l.name)) listed = &l;
        if (listed) {
            UE_LOGI("[STATS-PROBE] effect %ls LISTED (not added: its removal leaves %s)", name.c_str(), listed->leaves);
            Good();
            continue;
        }
        UE_LOGE("[STATS-PROBE] effect %ls MISMATCH: not vetted", name.c_str());
        Bad();
    }
}

// The host's readiness: one row of each owner reads.
bool HostReady() {
    float v = 0.f;
    return V::Read(V::Field::Health, &v) && V::Read(V::Field::Air, &v) && V::Read(V::Field::Dreaming, &v);
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ArmNow() == Arm::Off || g_done || !session || !session->running()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (host ? !HostReady() : !coop::dev::command_drill::ClientReady(session)) return;
    g_done = true;
    g_bad = 0;
    g_ok = 0;
    const bool red = ArmNow() == Arm::Red;
    for (uint8_t i = 0; i < static_cast<uint8_t>(V::Field::Count); ++i) {
        const V::Field f = static_cast<V::Field>(i);
        if (V::RowOf(f).write == V::WriteRule::ReadOnly) ProbeReadOnly(f);
        else ProbeWritable(f, red);
    }
    ProbeSnapshot();
    ProbeRefusals();
    ProbeDefaults();
    ProbeEffects(red);
    UE_LOGI("[STATS-PROBE] DONE bad=%d ok=%d", g_bad, g_ok);
}

void OnDisconnect() { g_done = false; }

}  // namespace coop::dev::stats_probe
