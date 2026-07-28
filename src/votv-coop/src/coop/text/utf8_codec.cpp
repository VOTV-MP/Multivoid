// coop/text/utf8_codec.cpp -- see coop/text/utf8_codec.h.

#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace coop::text {

std::string ToUtf8(const std::wstring& w) {
    // Hand-rolled rather than WideCharToMultiByte because it must be defined on
    // the two inputs the API is coy about: an UNPAIRED surrogate (dropped) and a
    // C0 control (dropped). Promoted verbatim from chat_sync::NickUtf8, which has
    // carried every chat line since 2026-07-04.
    std::string s;
    s.reserve(w.size() * 2);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(w[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size() &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(w[i + 1]) - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue;  // unpaired surrogate -- not a character
        }
        if (cp < 0x20) continue;
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return s;
}

bool FromUtf8Strict(const char* p, size_t n, std::wstring* out) {
    if (!out) return false;
    if (n == 0) { out->clear(); return true; }
    if (n > static_cast<size_t>(INT_MAX)) return false;
    // MB_ERR_INVALID_CHARS is the whole point: without it MultiByteToWideChar
    // SILENTLY substitutes U+FFFD, and the ASCII allowlist used to be the only
    // thing destroying ill-formed bytes. Deleting that allowlist without adding
    // this flag would have made ill-formed input merely ugly instead of refused.
    const int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p,
                                           static_cast<int>(n), nullptr, 0);
    if (need <= 0) return false;
    std::wstring w(static_cast<size_t>(need), L'\0');
    const int got = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p,
                                          static_cast<int>(n), w.data(), need);
    if (got <= 0) return false;
    w.resize(static_cast<size_t>(got));
    *out = std::move(w);
    return true;
}

std::wstring FromUtf8Lossy(const char* p, size_t n) {
    if (n == 0 || n > static_cast<size_t>(INT_MAX)) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(n), nullptr, 0);
    if (need <= 0) return {};
    std::wstring w(static_cast<size_t>(need), L'\0');
    const int got = ::MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(n), w.data(), need);
    if (got <= 0) return {};
    w.resize(static_cast<size_t>(got));
    return w;
}

std::string SanitizeUtf8(const char* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char u = static_cast<unsigned char>(p[i]);
        if (u >= 0x20 || u == 0x09) out.push_back(static_cast<char>(u));
    }
    return out;
}

std::string CapUtf8Bytes(std::string s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    return s;
}

std::wstring CapCodepoints(const std::wstring& w, size_t maxChars) {
    size_t chars = 0, i = 0;
    while (i < w.size() && chars < maxChars) {
        const wchar_t c = w[i];
        const bool pair = (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() &&
                           w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF);
        i += pair ? 2 : 1;
        ++chars;
    }
    return w.substr(0, i);
}

size_t CountCodepoints(const std::wstring& w) {
    size_t chars = 0, i = 0;
    while (i < w.size()) {
        const wchar_t c = w[i];
        const bool pair = (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() &&
                           w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF);
        i += pair ? 2 : 1;
        ++chars;
    }
    return chars;
}

void CopyUtf8ToBuffer(char* dst, size_t dstSize, const std::wstring& w) {
    if (!dst || dstSize == 0) return;
    dst[0] = '\0';
    if (w.empty()) return;
    // ToUtf8 drops C0 and unpaired surrogates; CapUtf8Bytes backs off to a
    // character boundary rather than splitting a sequence. Deliberately NOT
    // WideCharToMultiByte: its too-small-buffer contract is "return 0", which is
    // indistinguishable from "the name is empty" at the call site.
    const std::string s = CapUtf8Bytes(ToUtf8(w), dstSize - 1);
    std::memcpy(dst, s.data(), s.size());
    dst[s.size()] = '\0';
}

bool RunUtf8CodecSelftest() {
    int pass = 0, total = 0;
    auto ok = [&](bool cond, const char* what) {
        ++total;
        if (cond) ++pass;
        else UE_LOGE("utf8-codec selftest: FAIL -- %s", what);
    };
    auto roundTrip = [&](const std::wstring& w, const char* what) {
        const std::string u8 = ToUtf8(w);
        std::wstring back;
        ok(FromUtf8Strict(u8.data(), u8.size(), &back) && back == w, what);
    };

    // Round trips across the scripts the widened alphabet has to carry.
    // These literals are only trustworthy because the target now compiles with
    // /utf-8 (CMakeLists). Without it MSVC decodes source with the SYSTEM
    // codepage, and this very selftest caught that in our own build: it counted
    // three codepoints in the two-character literal below. If a future build
    // drops the flag, this case fails again -- which is the point.
    roundTrip(L"Pelmentor", "ascii round trip");
    roundTrip(L"Пельмен", "cyrillic round trip");
    roundTrip(L"中文名字", "hanzi round trip");
    roundTrip(L"あかカナ", "kana round trip");
    // Astral: an emoji survives ONLY as a surrogate pair through wchar_t.
    roundTrip(L"\xD83D\xDE00", "astral (emoji) round trip");

    // The strict decoder REFUSES what the lossy one would silently repair.
    {
        std::wstring sink;
        const char truncated[] = "\xD0";              // lead byte, no continuation
        ok(!FromUtf8Strict(truncated, 1, &sink), "strict refuses a truncated sequence");
        const char overlong[] = "\xC0\xAF";           // over-long '/'
        ok(!FromUtf8Strict(overlong, 2, &sink), "strict refuses an over-long sequence");
        const char loneCont[] = "\x80";               // continuation with no lead
        ok(!FromUtf8Strict(loneCont, 1, &sink), "strict refuses a lone continuation");
        ok(FromUtf8Strict("", 0, &sink) && sink.empty(), "empty is well-formed");
    }

    // The byte cap never splits a character. Cyrillic is 2 bytes/char, so an ODD
    // cap lands mid-character and must back off.
    {
        const std::string cyr = ToUtf8(L"Пельм");  // 10 bytes
        const std::string cut = CapUtf8Bytes(cyr, 5);
        std::wstring sink;
        ok(cut.size() == 4, "byte cap backs off to a character boundary");
        ok(FromUtf8Strict(cut.data(), cut.size(), &sink), "byte-capped text is still well-formed");
    }

    // The codepoint cap counts characters, not wchar_t units, and never splits a
    // surrogate pair -- the exact trap the old UTF-16-unit cap fell into.
    {
        const std::wstring emoji = L"\xD83D\xDE00\xD83D\xDE0D";  // 2 codepoints, 4 units
        ok(CountCodepoints(emoji) == 2, "codepoint count pairs surrogates");
        const std::wstring one = CapCodepoints(emoji, 1);
        ok(one.size() == 2, "codepoint cap keeps a surrogate pair whole");
        const std::string u8 = ToUtf8(one);
        std::wstring sink;
        ok(FromUtf8Strict(u8.data(), u8.size(), &sink), "codepoint-capped text encodes cleanly");
        ok(CountCodepoints(L"中文") == 2, "codepoint count on BMP CJK");
    }

    // The denylist strips controls and keeps everything else, including bytes a
    // widened alphabet needs.
    {
        const char raw[] = "a\x01\x1F" "b\tc";
        const std::string s = SanitizeUtf8(raw, sizeof(raw) - 1);
        ok(s == "ab\tc", "denylist strips C0, keeps TAB");
        const std::string cyr = ToUtf8(L"П");
        ok(SanitizeUtf8(cyr.data(), cyr.size()) == cyr, "denylist keeps non-ASCII");
    }

    // THE EGRESS, and specifically the cliff that shipped in v132. The old code
    // called WideCharToMultiByte with a 23-byte cap; at 12 Cyrillic characters
    // (24 bytes) it returned 0 and the row went BLANK. These cases assert the two
    // properties the bare API does not give us: a too-long name TRUNCATES rather
    // than vanishing, and it truncates on a character boundary.
    {
        char narrowBuf[8] = {};
        // 12 Cyrillic characters = 24 bytes, the exact v132 cliff, into a buffer
        // far smaller still. Must be non-empty and well-formed.
        CopyUtf8ToBuffer(narrowBuf, L"Пельменьмень");
        std::wstring sink;
        ok(narrowBuf[0] != '\0', "egress truncates instead of blanking (the v132 cliff)");
        ok(std::strlen(narrowBuf) <= 7, "egress respects the buffer bound");
        ok(FromUtf8Strict(narrowBuf, std::strlen(narrowBuf), &sink),
           "egress output is well-formed after truncation");
        ok(std::strlen(narrowBuf) % 2 == 0, "egress cut on a 2-byte-char boundary, not mid-sequence");

        // A name that FITS must survive byte-identically -- truncation is only for
        // the overflow case, and a 4-byte astral character must not be halved.
        char full[coop::text::kNickBufBytes] = {};
        CopyUtf8ToBuffer(full, L"Пельмень2");
        ok(std::string(full) == ToUtf8(L"Пельмень2"), "egress is lossless when the name fits");
        char tiny[5] = {};
        CopyUtf8ToBuffer(tiny, L"\xD83D\xDE00\xD83D\xDE00");  // 2 astral chars, 8 bytes
        ok(std::strlen(tiny) == 4, "egress keeps a whole 4-byte character or none");
        ok(FromUtf8Strict(tiny, std::strlen(tiny), &sink), "astral truncation stays well-formed");

        // A buffer too small for even one character yields empty, not garbage.
        char nothing[2] = {};
        CopyUtf8ToBuffer(nothing, L"П");
        ok(nothing[0] == '\0', "egress emits empty when not one character fits");

        // The declared buffer width must actually admit the declared policy: 20
        // codepoints of the widest script. If someone lowers kNickBufBytes below
        // the cap, this fails here rather than silently in a snapshot row.
        ok(coop::text::kNickBufBytes > coop::text::kNickMaxBytes,
           "kNickBufBytes leaves room for the NUL");
        char widest[coop::text::kNickBufBytes] = {};
        CopyUtf8ToBuffer(widest, std::wstring(20, L'中'));  // 20 hanzi = 60 bytes
        ok(std::strlen(widest) == 60, "a full-length CJK name fits the declared buffer");
    }

    UE_LOGI("utf8-codec selftest: %s (%d/%d)", pass == total ? "PASS" : "FAIL", pass, total);
    return pass == total;
}

}  // namespace coop::text
