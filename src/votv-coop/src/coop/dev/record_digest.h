// coop/dev/record_digest.h -- a save record's VALUES as one short line of per-group hashes.
//
// Co-located private header (src tree, not include/): dev instruments only. A record's payload
// SHAPE (how many elements per value group) is a class fingerprint and says nothing about what the
// groups hold, so "the disc kept its data across a rejoin" cannot be read off it. This prints one
// FNV-1a hash per non-empty value group, so two log lines taken at two moments compare by eye and
// a mismatch names the group that moved.
//
// Class, key and transform are left out on purpose -- the readout prints the first two beside the
// digest, and a carried record's transform is where the item stood when it was pocketed.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::record_digest {

struct Fnv32 {
    uint32_t h = 2166136261u;
    void Bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    }
    template <class T> void Pod(const T& v) { Bytes(&v, sizeof(v)); }
    // The length goes in first, so {"ab","c"} and {"a","bc"} do not collide.
    void Str(const std::wstring& s) { Pod(static_cast<uint32_t>(s.size())); Bytes(s.data(), s.size() * sizeof(wchar_t)); }
};

// "b=1a2b3c4d f=... sig=..." over the non-empty value groups, or "EMPTY".
inline std::string ValuesOf(const ue_wrap::save_record::SaveRecord& r) {
    std::string out;
    auto emit = [&out](const char* tag, bool any, const Fnv32& f) {
        if (!any) return;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%s=%08x", out.empty() ? "" : " ", tag, f.h);
        out += buf;
    };
    // A group of PODs: every inner array's length, then its elements' bytes.
    auto pods = [&emit](const char* tag, const auto& vv) {
        Fnv32 f;
        bool any = false;
        for (const auto& v : vv) {
            f.Pod(static_cast<uint32_t>(v.size()));
            for (const auto& e : v) { f.Pod(e); any = true; }
        }
        emit(tag, any, f);
    };
    auto strs = [&emit](const char* tag, const auto& vv) {
        Fnv32 f;
        bool any = false;
        for (const auto& v : vv) {
            f.Pod(static_cast<uint32_t>(v.size()));
            for (const auto& s : v) { f.Str(s); any = true; }
        }
        emit(tag, any, f);
    };

    pods("b", r.bools);
    pods("f", r.floats);
    pods("i", r.ints);
    strs("s", r.strings);
    strs("cls", r.classes);
    pods("v", r.vectors);
    pods("rot", r.rotators);
    pods("x", r.transforms);
    pods("by", r.bytes);
    strs("nm", r.names);
    return out.empty() ? "EMPTY" : out;
}

}  // namespace coop::dev::record_digest
