// coop/items/order_rows.h -- an order's items on the wire, shared by the request (OrderRequest, client
// to host) and the queue mirror (OrderQueue, host to clients). Each item is `uint8 head`, then the ASCII
// bytes of a name: the head's low seven bits are the name's length (1..kMaxOrderRowName) and its top bit
// its kind, clear for a list_store row name, set for the class of an item no row names -- one a world
// event builds outside the shop (the daily delivery, a gift). A by-class item goes on with `uint8
// asPropLen` (0..kMaxOrderRowName, 0 for None) and that many bytes: the list_props name the generic
// prop_C carries its identity in. A request carries rows only, since the host prices by row; the mirror
// carries both. Header-only.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/world/order_economy.h"  // QueuedItem

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::order_rows {

inline constexpr uint8_t kByClass  = 0x80;  // the head's kind bit
inline constexpr uint8_t kLenMask  = 0x7F;
static_assert(coop::net::kMaxOrderRowName <= kLenMask, "a name's length must fit the head's seven bits");

// list_store keys and class names are ASCII identifiers, so they narrow and widen losslessly; a
// non-ASCII character becomes '?', which fails the receiver's lookup rather than resolving to the
// wrong row or class.
inline std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c >= 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}

inline std::wstring WidenAscii(const uint8_t* p, int n) {
    std::wstring w;
    w.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w.push_back(static_cast<wchar_t>(p[i]));
    return w;
}

// One item of `kind` into `buf` after the `pos` bytes already there, unless it would pass `cap`.
inline bool PackOne(const std::wstring& name, uint8_t kind, uint8_t* buf, int& pos, int cap) {
    std::string s = NarrowAscii(name);
    if (s.empty()) return false;
    if (s.size() > static_cast<size_t>(coop::net::kMaxOrderRowName))
        s.resize(static_cast<size_t>(coop::net::kMaxOrderRowName));
    if (pos + 1 + static_cast<int>(s.size()) > cap) return false;
    buf[pos++] = static_cast<uint8_t>(kind | s.size());
    std::memcpy(buf + pos, s.data(), s.size());
    pos += static_cast<int>(s.size());
    return true;
}

// Pack rows[from..] as row items, stopping before one would pass `cap`. Returns how many were packed
// and advances `pos`.
inline int Pack(const std::vector<std::wstring>& rows, size_t from, uint8_t* buf, int& pos, int cap) {
    int packed = 0;
    for (size_t i = from; i < rows.size() && PackOne(rows[i], 0, buf, pos, cap); ++i) ++packed;
    return packed;
}

// Pack a queued order's items[from..], each by its row or by its class and asProp, as Pack.
inline int PackQueued(const std::vector<ue_wrap::order_economy::QueuedItem>& items, size_t from, uint8_t* buf,
                      int& pos, int cap) {
    int packed = 0;
    for (size_t i = from; i < items.size(); ++i) {
        const auto& it = items[i];
        const int start = pos;
        if (!it.row.empty()) {
            if (!PackOne(it.row, 0, buf, pos, cap)) break;
        } else {
            std::string ap = NarrowAscii(it.asProp);
            if (ap.size() > static_cast<size_t>(coop::net::kMaxOrderRowName))
                ap.resize(static_cast<size_t>(coop::net::kMaxOrderRowName));
            if (!PackOne(it.cls, kByClass, buf, pos, cap) || pos + 1 + static_cast<int>(ap.size()) > cap) {
                pos = start;  // a class without the room for its asProp does not go half
                break;
            }
            buf[pos++] = static_cast<uint8_t>(ap.size());
            std::memcpy(buf + pos, ap.data(), ap.size());
            pos += static_cast<int>(ap.size());
        }
        ++packed;
    }
    return packed;
}

// Read one item from [p, end): its kind and name. False on a truncated item or a length outside
// 1..kMaxOrderRowName.
inline bool UnpackOne(const uint8_t*& p, const uint8_t* end, bool& byClass, std::wstring& name) {
    if (p >= end) return false;
    const uint8_t head = *p++;
    const int len = head & kLenMask;
    if (len == 0 || len > coop::net::kMaxOrderRowName || p + len > end) return false;
    byClass = (head & kByClass) != 0;
    name = WidenAscii(p, len);
    p += len;
    return true;
}

// Unpack a request's `count` items onto `out`. False on a bad item, and on an item by class: a client
// orders shop rows, and the host prices nothing else.
inline bool Unpack(const uint8_t* p, const uint8_t* end, int count, std::vector<std::wstring>& out) {
    for (int k = 0; k < count; ++k) {
        bool byClass = false;
        std::wstring name;
        if (!UnpackOne(p, end, byClass, name) || byClass) return false;
        out.push_back(std::move(name));
    }
    return true;
}

// Unpack a mirrored order's `count` items onto `out`, by row or by class and asProp. False on a bad item.
inline bool UnpackQueued(const uint8_t* p, const uint8_t* end, int count,
                         std::vector<ue_wrap::order_economy::QueuedItem>& out) {
    for (int k = 0; k < count; ++k) {
        bool byClass = false;
        std::wstring name;
        if (!UnpackOne(p, end, byClass, name)) return false;
        if (!byClass) {
            out.push_back(ue_wrap::order_economy::QueuedItem{std::move(name), {}, {}});
            continue;
        }
        if (p >= end) return false;
        const int apLen = *p++;
        if (apLen > coop::net::kMaxOrderRowName || p + apLen > end) return false;
        out.push_back(ue_wrap::order_economy::QueuedItem{{}, std::move(name), WidenAscii(p, apLen)});
        p += apLen;
    }
    return true;
}

}  // namespace coop::order_rows
