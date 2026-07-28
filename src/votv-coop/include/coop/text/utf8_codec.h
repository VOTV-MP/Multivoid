// coop/text/utf8_codec.h -- ARC D1: the ONE owner of text encoding.
//
// WHY THIS EXISTS. Before this file the mod had THREE encoders
// (chat_sync::NickUtf8, chat_feed::ToUtf8, player_handshake::ToUtf8) and, at the
// two places a name ENTERS the process, no decoder at all: config.cpp and
// session_runtime.cpp widened bytes to wchar_t ONE AT A TIME, which is a Latin-1
// widen. A UTF-8 name therefore reached SanitizeNickname as N mojibake wchars
// and was stripped whole -- the measured root of "Cyrillic nicknames do not
// work", and the reason widening the alphabet without fixing entry would have
// legalised mojibake rather than supporting Cyrillic.
//
// THE TWO CAPS ARE DIFFERENT QUESTIONS and both are named here:
//   kNickMaxChars (player_handshake.h) is a DISPLAY policy in CODEPOINTS.
//   kNickMaxBytes is a BUFFER/wire bound in bytes.
// A single byte cap would hand ASCII 20 characters, Cyrillic 10 and CJK 6 --
// script unfairness disguised as an invariant.
//
// WELL-FORMEDNESS IS ESTABLISHED WHERE WE READ, NOT WHERE WE WROTE. Entry-side
// truncation is a cap on MY machine and guarantees nothing about a stranger's
// bytes, so the receive boundary decodes STRICTLY and rejects a whole ill-formed
// field rather than repairing it (a repair invents a name nobody chose).
#pragma once

#include <cstddef>
#include <string>

namespace coop::text {

// The wire/buffer bound for a nickname, in BYTES. 4 x the codepoint cap, because
// one codepoint is at most 4 UTF-8 bytes -- so any name that satisfies the
// display policy also fits here, whatever script it is written in.
inline constexpr size_t kNickMaxBytes = 20 * 4;

// The width a fixed `char[]` needs to hold ANY name the display policy admits,
// plus the NUL. Every plain-data snapshot row and every persisted record that
// carries a nickname declares its buffer with THIS, never a literal -- a literal
// is a second owner of the cap, and the two drift silently (measured: five
// buffers sat at 24 bytes while the policy said 20 codepoints, i.e. ASCII-only
// arithmetic that nothing re-derived when D1 widened the alphabet).
inline constexpr size_t kNickBufBytes = kNickMaxBytes + 1;

// UTF-16 (Windows wchar_t) -> UTF-8. Surrogate-pair aware, so astral codepoints
// survive; C0 control codepoints are dropped. This is the promoted body of
// chat_sync::NickUtf8, in production since 2026-07-04.
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

// Cap a UTF-8 string to `maxBytes` WITHOUT splitting a multi-byte sequence --
// backs off past continuation bytes to a character boundary. A raw resize() here
// manufactures exactly the ill-formed tail the strict decoder above destroys,
// i.e. our own sender's name would arrive as the placeholder.
std::string CapUtf8Bytes(std::string s, size_t maxBytes);

// Cap a UTF-16 string to `maxChars` CODEPOINTS, never splitting a surrogate
// pair. Counting in wchar_t units (what the old cap did) cuts an astral
// character in half and yields an unpaired surrogate.
std::wstring CapCodepoints(const std::wstring& w, size_t maxChars);

// Count codepoints, pairing surrogates. What "20 characters" means to a human.
size_t CountCodepoints(const std::wstring& w);

// THE ONE EGRESS. Encode `w` into a fixed byte buffer, truncating on a CODEPOINT
// boundary and always NUL-terminating. Every `char nick[]` in a snapshot row or a
// persisted record is filled through here.
//
// WHY IT EXISTS -- measured 2026-07-28, and it had SHIPPED: a bare
// WideCharToMultiByte into a buffer that is one byte too small does not truncate.
// It returns 0 and sets ERROR_INSUFFICIENT_BUFFER, so a caller that writes
// out[ret] = '\0' stores the EMPTY string and the name VANISHES. v132 blanked the
// TAB row at 12 Cyrillic / 8 hanzi / 6 emoji characters, and the drills missed it
// because every drill name was short enough to fit. Truncating a long name is a
// display compromise; blanking it destroys the identity the arbiter just assigned
// and (since arc B persists) writes that loss to multivoid.ini.
//
// The other half of the same defect was the opposite reflex -- three sites
// open-coded `c < 128 ? c : '?'`, which does not blank but SQUASHES, so a
// Cyrillic name rendered as '????????' on the floating nameplate. Both are the
// same missing owner: encoding had a decoder (entry) and no encoder (egress).
void CopyUtf8ToBuffer(char* dst, size_t dstSize, const std::wstring& w);

// Array overload: the size comes from the type, so a caller cannot pass a stale
// length. Prefer this at every call site.
template <size_t N>
inline void CopyUtf8ToBuffer(char (&dst)[N], const std::wstring& w) {
    CopyUtf8ToBuffer(dst, N, w);
}

// Machine-asserted at boot beside the link-classify and nickname-arbiter
// selftests: round-trips across scripts, the strict decoder's refusal of
// truncated and over-long sequences, both caps on boundaries a raw resize would
// split. None of it is reachable by a LAN drill.
bool RunUtf8CodecSelftest();

}  // namespace coop::text
