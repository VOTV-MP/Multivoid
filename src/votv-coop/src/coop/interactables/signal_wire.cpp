// coop/signal_wire.cpp -- see header.

#include "coop/interactables/signal_wire.h"

#include "coop/net/blob_chunks.h"

#include "ue_wrap/log.h"

#include <cstring>

namespace coop::signal_wire {

namespace {

constexpr size_t kHeadSize = 12 + 8 + 20 + 8;  // flags/lens/pad + ints + floats + date

void AppendWchars(std::vector<uint8_t>& b, const std::wstring& s, size_t cap) {
    size_t n = s.size() > cap ? cap : s.size();
    for (size_t i = 0; i < n; ++i) {
        const uint16_t c = static_cast<uint16_t>(s[i]);
        b.push_back(static_cast<uint8_t>(c & 0xFF));
        b.push_back(static_cast<uint8_t>(c >> 8));
    }
}

bool ReadWchars(const std::vector<uint8_t>& b, size_t& off, size_t chars, std::wstring& s) {
    if (off + chars * 2 > b.size()) return false;
    s.clear();
    s.reserve(chars);
    for (size_t i = 0; i < chars; ++i) {
        s.push_back(static_cast<wchar_t>(b[off] | (b[off + 1] << 8)));
        off += 2;
    }
    return true;
}

template <class T>
void AppendPod(std::vector<uint8_t>& b, T v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}

template <class T>
bool ReadPod(const std::vector<uint8_t>& b, size_t& off, T& v) {
    if (off + sizeof(T) > b.size()) return false;
    std::memcpy(&v, b.data() + off, sizeof(T));
    off += sizeof(T);
    return true;
}

}  // namespace

std::vector<uint8_t> Serialize(const ue_wrap::signal_dynamic::Row& r, bool adopt) {
    const size_t nc = r.name.size() > kNameCap ? kNameCap : r.name.size();
    const size_t ic = r.id.size() > kIdCap ? kIdCap : r.id.size();
    const size_t oc = r.object.size() > kObjectCap ? kObjectCap : r.object.size();
    const size_t sc = r.signal.size() > kSignalCap ? kSignalCap : r.signal.size();
    if (oc < r.object.size() || sc < r.signal.size()) {
        // A truncated FName resolves to the WRONG template on the receiver --
        // load-bearing, so it must be loud (caps are sized to never hit).
        UE_LOGW("signal_wire: FName over cap (object %zu, signal %zu chars) -- truncated",
                r.object.size(), r.signal.size());
    }
    std::vector<uint8_t> b;
    b.reserve(kHeadSize + 2 * (nc + ic + oc + sc));
    b.push_back(1);
    uint8_t flags = 0;
    if (r.hasData) flags |= 0x01;
    if (r.isCopy) flags |= 0x02;
    if (adopt) flags |= 0x04;
    b.push_back(flags);
    b.push_back(r.frequency);
    b.push_back(r.quality);
    b.push_back(r.objectType);
    b.push_back(static_cast<uint8_t>(nc));
    b.push_back(static_cast<uint8_t>(ic));
    b.push_back(static_cast<uint8_t>(oc));
    b.push_back(static_cast<uint8_t>(sc));
    b.push_back(0); b.push_back(0); b.push_back(0);
    AppendPod<int32_t>(b, r.level);
    AppendPod<int32_t>(b, r.polarity);
    AppendPod<float>(b, r.size);
    AppendPod<float>(b, r.decoded);
    AppendPod<float>(b, r.downloadedAtQuality);
    AppendPod<float>(b, r.locX);
    AppendPod<float>(b, r.locY);
    AppendPod<int64_t>(b, r.date);
    AppendWchars(b, r.name, kNameCap);
    AppendWchars(b, r.id, kIdCap);
    AppendWchars(b, r.object, kObjectCap);
    AppendWchars(b, r.signal, kSignalCap);
    return b;
}

bool Deserialize(const std::vector<uint8_t>& b, ue_wrap::signal_dynamic::Row& out,
                 bool& outAdopt) {
    if (b.size() < kHeadSize || b[0] != 1) return false;
    const uint8_t flags = b[1];
    out.hasData = (flags & 0x01) != 0;
    out.isCopy  = (flags & 0x02) != 0;
    outAdopt    = (flags & 0x04) != 0;
    out.frequency  = b[2];
    out.quality    = b[3];
    out.objectType = b[4];
    const size_t nc = b[5], ic = b[6], oc = b[7], sc = b[8];
    if (nc > kNameCap || ic > kIdCap || oc > kObjectCap || sc > kSignalCap) return false;
    size_t off = 12;
    if (!ReadPod(b, off, out.level) || !ReadPod(b, off, out.polarity) ||
        !ReadPod(b, off, out.size) || !ReadPod(b, off, out.decoded) ||
        !ReadPod(b, off, out.downloadedAtQuality) ||
        !ReadPod(b, off, out.locX) || !ReadPod(b, off, out.locY) ||
        !ReadPod(b, off, out.date))
        return false;
    return ReadWchars(b, off, nc, out.name) &&
           ReadWchars(b, off, ic, out.id) &&
           ReadWchars(b, off, oc, out.object) &&
           ReadWchars(b, off, sc, out.signal);
}

uint64_t ContentHash(const std::vector<uint8_t>& blob) {
    if (blob.size() < 2) return 0;
    // Zero the adopt bit so a connect snapshot and a live append of the same
    // row hash identically.
    std::vector<uint8_t> copy = blob;
    copy[1] = static_cast<uint8_t>(copy[1] & ~0x04);
    return coop::blob_chunks::Fnv64(copy);
}

}  // namespace coop::signal_wire
