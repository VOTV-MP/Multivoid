// coop/text/utf8_codec.h -- the one owner of text encoding: every conversion between the
// engine's UTF-16 and our UTF-8 wire and files goes through here.
//
// The two caps are different questions and both are named below. kNickMaxChars
// (player_handshake.h) is a display policy in CODEPOINTS; kNickMaxBytes is a buffer and wire
// bound in BYTES. A single byte cap would hand ASCII 20 characters, Cyrillic 10 and CJK 6.
//
// Well-formedness is established where we READ, not where we wrote: entry-side truncation
// bounds only this machine, so the receive boundary decodes strictly and refuses a whole
// ill-formed field rather than repairing it, because a repair invents a name nobody chose.
#pragma once

#include <cstddef>
#include <string>

namespace coop::text {

// The wire/buffer bound for a nickname, in BYTES. 4 x the codepoint cap, because
// one codepoint is at most 4 UTF-8 bytes -- so any name that satisfies the
// display policy also fits here, whatever script it is written in.
inline constexpr size_t kNickMaxBytes = 20 * 4;

// The width a fixed `char[]` needs to hold any name the display policy admits, plus the NUL.
// Every snapshot row and persisted record that carries a nickname declares its buffer with
// THIS and never a literal: a literal is a second owner of the cap and the two drift.
inline constexpr size_t kNickBufBytes = kNickMaxBytes + 1;

// UTF-16 (Windows wchar_t) -> UTF-8. Surrogate-pair aware, so astral codepoints survive; C0
// control codepoints are dropped.
std::string ToUtf8(const std::wstring& w);

// UTF-8 -> UTF-16, STRICT. Returns false and leaves `out` untouched when the
// input is not well-formed UTF-8 (MB_ERR_INVALID_CHARS): the caller then shows a
// placeholder rather than a repaired string. Empty input is well-formed.
bool FromUtf8Strict(const char* p, size_t n, std::wstring* out);

// UTF-8 -> UTF-16 for text we produced ourselves (config files, our own
// registries). Lossy where the source is not well-formed, which is acceptable
// only because the source is ours; never use it on a peer's bytes.
std::wstring FromUtf8Lossy(const char* p, size_t n);

// Strip C0 control bytes (keeping TAB) from a UTF-8 string. A DENYLIST: it
// removes what is dangerous at the render surface instead of enumerating what is
// allowed, which is the only form that can survive a widening alphabet.
std::string SanitizeUtf8(const char* p, size_t n);

// Cap a UTF-8 string to `maxBytes` without splitting a multi-byte sequence: it backs off past
// continuation bytes to a character boundary. A raw resize() here manufactures exactly the
// ill-formed tail the strict decoder refuses, so our own name would arrive as the placeholder.
std::string CapUtf8Bytes(std::string s, size_t maxBytes);

// Decode the codepoint starting at `w[i]`, pairing surrogates, and return how many wchar_t
// UNITS it occupied (1 or 2). An unpaired surrogate decodes to itself over one unit, so a
// caller can deny it explicitly instead of mis-measuring the string around it.
//
// wchar_t is a code UNIT, not a character: `for (wchar_t c : name)` sees an emoji as two
// meaningless halves. Four call sites share this decoder rather than each re-deriving the
// pair arithmetic.
size_t DecodeCodepoint(const std::wstring& w, size_t i, uint32_t* cp);

// Cap a UTF-16 string to `maxChars` CODEPOINTS, never splitting a surrogate pair. Counting in
// wchar_t units cuts an astral character in half and yields an unpaired surrogate.
std::wstring CapCodepoints(const std::wstring& w, size_t maxChars);

// Count codepoints, pairing surrogates. What "20 characters" means to a human.
size_t CountCodepoints(const std::wstring& w);

// THE ONE EGRESS. Encode `w` into a fixed byte buffer, truncating on a CODEPOINT boundary and
// always NUL-terminating. Every `char nick[]` in a snapshot row or a persisted record is
// filled through here.
//
// A bare WideCharToMultiByte into a buffer one byte too small does not truncate: it returns 0
// and sets ERROR_INSUFFICIENT_BUFFER, so a caller that writes out[ret] = '\0' stores the EMPTY
// string and the name vanishes. The opposite reflex, `c < 128 ? c : '?'`, squashes a
// non-Latin name to '????????'. Truncating a long name is a display compromise; blanking or
// squashing it destroys the identity the arbiter just assigned.
void CopyUtf8ToBuffer(char* dst, size_t dstSize, const std::wstring& w);

// Array overload: the size comes from the type, so a caller cannot pass a stale
// length. Prefer this at every call site.
template <size_t N>
inline void CopyUtf8ToBuffer(char (&dst)[N], const std::wstring& w) {
    CopyUtf8ToBuffer(dst, N, w);
}

// Asserted at boot beside the link-classify and nickname-arbiter selftests: round-trips across
// scripts, the strict decoder's refusal of truncated and over-long sequences, and both caps on
// boundaries a raw resize would split.
bool RunUtf8CodecSelftest();

}  // namespace coop::text
