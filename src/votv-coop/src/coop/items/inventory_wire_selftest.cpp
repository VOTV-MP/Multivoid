// coop/items/inventory_wire_selftest.cpp -- the un-gated selftest of the profile blob (shape:
// coop/player/hand_history_selftest.cpp): the deflated envelope round-trips, a version-4 blob of an earlier build still
// reads, a hostile length is refused before it allocates, and a profile past one transport blob is cut into parts and put
// back together byte for byte. No engine and no wire. The synthetic record repeats what a real one does -- one tagged
// property stream per item, the same property names and class path each time, a unique key and a few floats -- so the
// sizes it prints are the shape of a real inventory's (the drills measure the real one).

#include "coop/items/inventory_wire.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace coop::inventory_wire {
namespace {

namespace W = coop::save_record_wire;

struct Tally {
    int pass = 0;
    int total = 0;
    void operator()(bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("inventory_wire selftest FAIL: %s", what);
    }
};

uint32_t g_rng = 12345u;
uint32_t Rand() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

// One item as the game's stream writes it: the tag names with their GUID suffixes, the class path, a unique key.
void AppendItem(std::vector<uint8_t>& rec) {
    static const char kHead[] =
        "position_3_857298F746915A06BE18BF8866B99248\0StructProperty\0Vector2D\0"
        "rotation_24_34F5C7CF4E527F73CA453BB107ED229D\0FloatProperty\0"
        "amount_14_FE891AE248E786225129B19811733B91\0ByteProperty\0None\0"
        "compartment_9_9537269E4531936A05C3DAA57FA3FDD4\0ByteProperty\0None\0"
        "class_26_966EC12C45A0D1DA297122B00DD609FE\0SoftObjectProperty\0"
        "/Game/objects/prop_fishSteak_10.prop_fishSteak_10_C\0"
        "transform_8_8BC7EC654B26A7B670A5A8936CBFEFD6\0StructProperty\0Transform\0Rotation\0Quat\0Translation\0Vector\0Scale3D\0"
        "name_18_1F1BBBB04FFE13CE4912ED8498B09972\0NameProperty\0fishsteak_10\0"
        "key_16_9B7C8C34413CDD6CDA151F90E2996F6A\0NameProperty\0";
    static const char kTail[] =
        "\0actorData_14_56E067C541A32001F39B00BF262A85D1\0ArrayProperty\0ByteProperty\0"
        "Temperature\0FloatProperty\0ignoreRotting\0BoolProperty\0invRead_Uses\0IntProperty\0"
        "timeElapsed\0FloatProperty\0invRead_Freshness\0IntProperty\0isSpoiled\0BoolProperty\0"
        "Name\0NameProperty\0fishsteak_10\0Static\0BoolProperty\0removeWOrespawn\0BoolProperty\0frozen\0BoolProperty\0";
    rec.insert(rec.end(), kHead, kHead + sizeof(kHead) - 1);
    static const char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    for (int i = 0; i < 22; ++i) rec.push_back(static_cast<uint8_t>(kAlpha[Rand() & 63]));
    rec.insert(rec.end(), kTail, kTail + sizeof(kTail) - 1);
    for (int i = 0; i < 12; ++i) rec.push_back(static_cast<uint8_t>(Rand()));   // the floats and ints
    // The rest of a real item's record is the property bytes the game writes between the tags; nothing in them repeats.
    for (int i = 0; i < 1200; ++i) rec.push_back(static_cast<uint8_t>((i % 7) ? 0 : 1));
}

coop::player_profile::Profile MakeProfile(int items) {
    coop::player_profile::Profile p;
    p.record.assign(23000, 0);
    for (size_t i = 0; i < p.record.size(); i += 13) p.record[i] = static_cast<uint8_t>(i >> 3);   // the base: not all zero
    for (int i = 0; i < items; ++i) AppendItem(p.record);
    p.hasVitals = true;
    p.vitals.health = 91.f; p.vitals.maxHealth = 100.f; p.vitals.food = 77.f; p.vitals.sleep = 40.f;
    p.vitals.foodConsumed = {L"mre", L"soap"};
    p.vitals.foodTolerance = {1.f, 2.f};
    p.pose = {10.f, 20.f, 30.f, 90.f, true};
    return p;
}

bool Same(const coop::player_profile::Profile& a, const coop::player_profile::Profile& b) {
    return a.record == b.record && a.hasVitals == b.hasVitals && a.vitals.health == b.vitals.health &&
           a.vitals.food == b.vitals.food && a.vitals.foodConsumed == b.vitals.foodConsumed &&
           a.vitals.foodTolerance == b.vitals.foodTolerance && a.pose.x == b.pose.x && a.pose.yaw == b.pose.yaw &&
           a.pose.valid == b.pose.valid;
}

void RunRoundTrip(Tally& check) {
    for (const int items : {0, 1, 13, 66}) {
        const coop::player_profile::Profile p = MakeProfile(items);
        const std::vector<uint8_t> blob = Serialize(p);
        coop::player_profile::Profile back;
        check(!blob.empty() && blob[0] == kVersion, "round trip: the blob does not carry the current version");
        check(Deserialize(blob, back) && Same(p, back), "round trip: the profile does not come back as it went");
        check(Serialize(back) == blob, "round trip: serializing the parsed profile does not give the same blob");
        // The ceiling this exists for: a record that was 58630 bytes raw at 13 items, and 66 of the smallest
        // everyday items (soap), each go as one transport blob, with room.
        check(blob.size() * 4 < coop::blob_chunks::MaxBlobBytes(), "round trip: a bag of 66 items does not fit one transport blob with 4x margin");
    }
}

void RunPlainVersion(Tally& check) {
    // What a build before deflate wrote: version 4, the body raw.
    const coop::player_profile::Profile p = MakeProfile(2);
    std::vector<uint8_t> b;
    b.push_back(kVersionPlain);
    W::AppU32(b, static_cast<uint32_t>(p.record.size()));
    b.insert(b.end(), p.record.begin(), p.record.end());
    W::AppU8(b, 1);
    for (const float v : {p.vitals.health, p.vitals.maxHealth, p.vitals.food, p.vitals.sleep, 0.f, 0.f, 0.f}) W::AppF32(b, v);
    W::AppU32(b, 2);
    for (size_t i = 0; i < 2; ++i) { W::AppWStr(b, p.vitals.foodConsumed[i]); W::AppF32(b, p.vitals.foodTolerance[i]); }
    W::AppU8(b, 1);
    W::AppF32(b, p.pose.x); W::AppF32(b, p.pose.y); W::AppF32(b, p.pose.z); W::AppF32(b, p.pose.yaw);
    coop::player_profile::Profile back;
    check(Deserialize(b, back) && Same(p, back), "version 4: a profile of an earlier build does not read");
}

void RunHostile(Tally& check) {
    const std::vector<uint8_t> good = Serialize(MakeProfile(3));
    coop::player_profile::Profile back;
    std::vector<uint8_t> b = good;
    b.resize(b.size() / 2);
    check(!Deserialize(b, back), "hostile: a truncated stream reads");
    b = good;
    b[0] = 9;
    check(!Deserialize(b, back), "hostile: an unknown version reads");
    b = good;
    b[1] = 0xFF; b[2] = 0xFF; b[3] = 0xFF; b[4] = 0x7F;   // a body of 2 GB claimed from a few KB
    check(!Deserialize(b, back), "hostile: a length past what deflate can expand to reads");
    b = good;
    b[1] = static_cast<uint8_t>(b[1] + 1);   // one byte more than the stream holds
    check(!Deserialize(b, back), "hostile: a length that is not the stream's reads");
    b = good;
    b.resize(5);
    check(!Deserialize(b, back), "hostile: a header with no stream reads");
    check(!Deserialize({}, back), "hostile: an empty blob reads");
}

void RunParts(Tally& check) {
    const size_t cap = coop::blob_chunks::MaxBlobBytes();
    // Incompressible bytes make a profile that must travel as parts: ~3.2 transport blobs.
    coop::player_profile::Profile p = MakeProfile(40);
    for (int i = 0; i < 175000; ++i) p.record.push_back(static_cast<uint8_t>(Rand()));
    const std::vector<uint8_t> blob = Serialize(p);
    check(blob.size() > 2 * cap, "parts: the synthetic profile is not past two transport blobs");
    const auto parts = SplitParts(blob, cap, 7);
    check(parts.size() >= 3 && parts.size() <= kMaxParts, "parts: the profile is not cut into 3 or more parts");
    bool sized = !parts.empty();
    for (const auto& part : parts) sized = sized && part.size() <= cap && part[0] == kPartMarker;
    check(sized, "parts: a part is past the transport blob or has no marker");

    PartJoiner j;
    std::vector<uint8_t> out;
    bool early = false, done = false;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::vector<uint8_t> part = parts[i];
        const bool whole = j.Feed(1, part, out);
        if (i + 1 < parts.size()) early = early || whole;
        else done = whole;
    }
    coop::player_profile::Profile back;
    check(!early && done && out == blob, "parts: the parts do not join into the profile blob byte for byte");
    check(Deserialize(out, back) && Same(p, back), "parts: the joined blob does not read back as the profile");

    // A blob that is not a part passes through, and one that fits is not cut.
    std::vector<uint8_t> small = Serialize(MakeProfile(1)), through;
    const auto one = SplitParts(small, cap, 8);
    check(one.size() == 1 && one[0] == small, "parts: a blob that fits is cut");
    check(j.Feed(2, small, through) && !through.empty() && through[0] == kVersion, "parts: a whole blob does not pass through");

    // Order and group: a part out of order, or of another group, drops the group in progress.
    std::vector<uint8_t> a = parts[0], c = parts[2];
    PartJoiner k;
    check(!k.Feed(1, a, out), "parts: the first part completes a group");
    check(!k.Feed(1, c, out), "parts: a part out of order is taken");
    std::vector<uint8_t> b1 = parts[1];
    check(!k.Feed(1, b1, out), "parts: a part after the group was dropped is taken");
    const auto other = SplitParts(blob, cap, 9);
    std::vector<uint8_t> o0 = other[0], p1 = parts[1];
    PartJoiner m;
    m.Feed(1, o0, out);
    check(!m.Feed(1, p1, out), "parts: a part of another group is taken");
    // Two senders' groups do not mix.
    PartJoiner n;
    std::vector<uint8_t> x0 = parts[0], y0 = other[0];
    n.Feed(1, x0, out);
    n.Feed(2, y0, out);
    bool joined1 = false, joined2 = false;
    for (size_t i = 1; i < parts.size(); ++i) {
        std::vector<uint8_t> xp = parts[i], yp = other[i];
        joined1 = n.Feed(1, xp, out);
        joined2 = n.Feed(2, yp, out);
    }
    check(joined1 && joined2, "parts: two senders' groups in progress at once do not both complete");
    // Too big for the parts' ceiling: refused, not cut short.
    std::vector<uint8_t> huge(cap * kMaxParts + 1, 0);
    check(SplitParts(huge, cap, 1).empty(), "parts: a blob past the ceiling of parts is cut");
    std::vector<uint8_t> bad = {kPartMarker, 1, 0, 0, 1, 9};   // a count of 1 is not a group
    check(!j.Feed(3, bad, out), "parts: a one-part group is taken");
}

bool RunSelftestBody() {
    Tally check;
    RunRoundTrip(check);
    RunPlainVersion(check);
    RunHostile(check);
    RunParts(check);
    // The sizes, for the record: what one item adds, and what the whole bag costs.
    const std::vector<uint8_t> empty = Serialize(MakeProfile(0)), full = Serialize(MakeProfile(66));
    UE_LOGI("inventory_wire selftest: synthetic bag, 0 items %zu bytes, 66 items %zu bytes", empty.size(), full.size());
    if (check.pass == check.total) {
        UE_LOGI("inventory_wire selftest: ALL PASS (%d checks)", check.total);
        return true;
    }
    UE_LOGE("inventory_wire selftest: %d/%d checks passed", check.pass, check.total);
    return false;
}

}  // namespace

bool RunSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunSelftestBody(); });
    return verdict;
}

}  // namespace coop::inventory_wire
