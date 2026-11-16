// coop/interactables/signal_wire.cpp -- see header.

#include "coop/interactables/signal_wire.h"

#include "coop/net/blob_chunks.h"
#include "coop/text/case_fold.h"

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::signal_wire {

namespace {

constexpr size_t kHeadSize = 12 + 8 + 20 + 8;  // flags/lens/pad + ints + floats + date
constexpr uint8_t kFlagImage = 0x08;

// Where the strings end: the photo-free part a row's identity covers.
size_t PrefixSize(const std::vector<uint8_t>& b) {
    if (b.size() < kHeadSize) return b.size();
    const size_t end = kHeadSize + 2 * (static_cast<size_t>(b[5]) + b[6] + b[7] + b[8]);
    return end < b.size() ? end : b.size();
}

// The object and signal leaves of a photo-free prefix (the name and id before them are FStrings), folded to lower case
// in place. They are FNames, and an FName renders in whichever spelling this process registered first, so two peers can
// render one name in two cases; the row is the same row, and its identity must not tell them apart.
void FoldNameLeaves(std::vector<uint8_t>& prefix) {
    if (prefix.size() < kHeadSize) return;
    for (size_t at = kHeadSize + 2 * (static_cast<size_t>(prefix[5]) + prefix[6]); at + 1 < prefix.size(); at += 2) {
        const uint32_t folded = coop::text::CaseFold(static_cast<uint16_t>(prefix[at] | (prefix[at + 1] << 8)));
        if (folded > 0xFFFF) continue;
        prefix[at] = static_cast<uint8_t>(folded & 0xFF);
        prefix[at + 1] = static_cast<uint8_t>(folded >> 8);
    }
}

bool g_saidImageOverCap = false;
bool g_saidImageImplausible = false;

// A photo the laptop can decode as the game's own are: a PNG whose IHDR, its first chunk, is at most 2048 on a side.
// The laptop's BytesToImage decodes whatever it is handed at its declared size (ui_laptop.cpp:3836), and the
// photos the desk takes are renders of its download screen.
bool PlausiblePhoto(const std::vector<uint8_t>& png) {
    static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 24 || std::memcmp(png.data(), kSig, sizeof(kSig)) != 0) return false;
    if (std::memcmp(png.data() + 12, "IHDR", 4) != 0) return false;
    const auto be32 = [&](size_t at) {
        return (static_cast<uint32_t>(png[at]) << 24) | (static_cast<uint32_t>(png[at + 1]) << 16) |
               (static_cast<uint32_t>(png[at + 2]) << 8) | static_cast<uint32_t>(png[at + 3]);
    };
    const uint32_t w = be32(16), h = be32(20);
    return w >= 1 && h >= 1 && w <= 2048 && h <= 2048;
}

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

std::vector<uint8_t> Serialize(const ue_wrap::signal_dynamic::Row& r) {
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
    const size_t prefix = kHeadSize + 2 * (nc + ic + oc + sc);
    bool image = !r.image.empty();
    if (image && prefix + 4 + r.image.size() > coop::blob_chunks::MaxBlobBytes()) {
        image = false;
        if (!g_saidImageOverCap) {
            g_saidImageOverCap = true;
            UE_LOGW("signal_wire: row '%ls' has a %zu-byte photo past the blob cap -- it crosses without it (said once)",
                    r.name.c_str(), r.image.size());
        }
    }
    std::vector<uint8_t> b;
    b.reserve(prefix + (image ? 4 + r.image.size() : 0));
    b.push_back(1);
    uint8_t flags = 0;
    if (r.hasData) flags |= 0x01;
    if (r.isCopy) flags |= 0x02;
    if (image) flags |= kFlagImage;
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
    if (image) {
        AppendPod<uint32_t>(b, static_cast<uint32_t>(r.image.size()));
        b.insert(b.end(), r.image.begin(), r.image.end());
    }
    return b;
}

bool Deserialize(const std::vector<uint8_t>& b, ue_wrap::signal_dynamic::Row& out) {
    if (b.size() < kHeadSize || b[0] != 1) return false;
    const uint8_t flags = b[1];
    out.hasData = (flags & 0x01) != 0;
    out.isCopy  = (flags & 0x02) != 0;
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
    if (!ReadWchars(b, off, nc, out.name) || !ReadWchars(b, off, ic, out.id) ||
        !ReadWchars(b, off, oc, out.object) || !ReadWchars(b, off, sc, out.signal))
        return false;
    out.image.clear();
    if (!(flags & kFlagImage)) return true;
    uint32_t bytes = 0;
    if (!ReadPod(b, off, bytes) || bytes == 0 || off + bytes > b.size()) return false;
    out.image.assign(b.begin() + static_cast<ptrdiff_t>(off), b.begin() + static_cast<ptrdiff_t>(off + bytes));
    if (!PlausiblePhoto(out.image)) {
        if (!g_saidImageImplausible) {
            g_saidImageImplausible = true;
            UE_LOGW("signal_wire: row '%ls' came with a %u-byte photo that is no plausible PNG -- kept without it "
                    "(said once)", out.name.c_str(), bytes);
        }
        out.image.clear();
    }
    return true;
}

uint64_t ContentHash(const std::vector<uint8_t>& blob) {
    if (blob.size() < 2) return 0;
    // The strings' end, the image bit zeroed: a row with its photo and without it hash identically.
    std::vector<uint8_t> copy(blob.begin(), blob.begin() + static_cast<ptrdiff_t>(PrefixSize(blob)));
    copy[1] = static_cast<uint8_t>(copy[1] & ~kFlagImage);
    FoldNameLeaves(copy);
    return coop::blob_chunks::Fnv64(copy);
}

}  // namespace coop::signal_wire
