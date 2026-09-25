// coop/items/order_rows.h -- a shop order's items on the wire, shared by the request (OrderRequest,
// client to host) and the queue mirror (OrderQueue, host to clients): each item a list_store row name,
// `uint8 nameLen` and then the name's ASCII bytes. Header-only.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::order_rows {

// list_store keys are ASCII identifiers, so they narrow and widen losslessly; a non-ASCII character
// becomes '?', which fails the receiver's catalog lookup and refuses the order loudly rather than
// resolving it to the wrong row.
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

// Pack rows[from..] into `buf` after the `pos` bytes already there, stopping before an item would pass
// `cap`. Returns how many were packed and advances `pos`.
inline int Pack(const std::vector<std::wstring>& rows, size_t from, uint8_t* buf, int& pos, int cap) {
    int packed = 0;
    for (size_t i = from; i < rows.size(); ++i) {
        std::string name = NarrowAscii(rows[i]);
        if (name.size() > static_cast<size_t>(coop::net::kMaxOrderRowName))
            name.resize(static_cast<size_t>(coop::net::kMaxOrderRowName));
        const int item = 1 + static_cast<int>(name.size());
        if (pos + item > cap) break;
        buf[pos++] = static_cast<uint8_t>(name.size());
        std::memcpy(buf + pos, name.data(), name.size());
        pos += static_cast<int>(name.size());
        ++packed;
    }
    return packed;
}

// Unpack `count` items from [p, end) onto `out`. False on a truncated item or one whose name is empty
// or longer than kMaxOrderRowName.
inline bool Unpack(const uint8_t* p, const uint8_t* end, int count, std::vector<std::wstring>& out) {
    for (int k = 0; k < count; ++k) {
        if (p >= end) return false;
        const uint8_t len = *p++;
        if (len == 0 || len > coop::net::kMaxOrderRowName || p + len > end) return false;
        out.push_back(WidenAscii(p, len));
        p += len;
    }
    return true;
}

}  // namespace coop::order_rows
