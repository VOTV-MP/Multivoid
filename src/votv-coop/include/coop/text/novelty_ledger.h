// coop/text/novelty_ledger.h -- how much NEW alphabet a remote peer may introduce.
//
// THE EXPOSURE THE LAZY ATLAS OPENS (docs/security/TRACKER.md, W11). Chat text is
// the one attacker-controlled string in this process, and after the ImGui 1.92
// flip every codepoint in the repertoire is rasterisable on demand from it. A few
// hundred bytes of deliberately diverse UTF-8 therefore forces thousands of
// FreeType rasterisations on EVERY receiving peer, inside the frame that draws
// them. RHI-independent, and nothing in ImGui bounds per-frame rasterisation
// (measured by absence).
//
// WHAT THIS DOES AND DOES NOT PREVENT -- state it plainly, because an earlier
// revision of the design claimed more:
//
//   IT BOUNDS A STUTTER. Rasterising thousands of first-sight glyphs in one frame
//   is CPU cost paid on the render thread. Capping how fast new codepoints may
//   ENTER bounds that burst.
//
//   IT DOES NOT PREVENT PACK EXHAUSTION, and it never could: this ledger is
//   monotone, so a patient peer reaches the whole repertoire within its budget and
//   a long-lived multilingual server gets there for free. After that a single
//   snapshot carrying everything passes with ZERO novelty. What answers pack
//   exhaustion is capacity, and that was measured: the pathological demand (chat
//   feed + overhead bubble + scoreboard, all asking for the entire repertoire at
//   once) is 3,589,892 px2 against 2048^2's 4,194,304 -- it fits, with 17 %
//   headroom. ui/atlas_watch.cpp's pack-failure detector is the tripwire for the
//   case that arithmetic did not foresee.
//
// WHY AT THE RECEIVE BOUNDARY AND NOT IN A DRAW LOOP. The chat feed is one of at
// least THREE surfaces that rasterise remote-authored text -- the feed rows, the
// overhead chat bubble (which bakes through CalcTextSizeA before any draw-time
// budget could be consulted) and the scoreboard's remote nicks. A cap in one draw
// loop is a SITE LIST, and the other two are the ones an attacker would use. It
// also wanted a soft cap, a row-deferral rule and a forward-progress guarantee --
// three pieces of machinery that existed only because the bound was in the wrong
// place. Bounded where the text ENTERS, every surface that later draws the string
// is bounded by construction.
//
// THE LEDGER IS OURS, NOT ImGui's. The obvious implementation is to ask the atlas
// what it has already baked. That is wrong twice. THREAD: the receive boundary
// runs on the GAME thread while ImFontBaked::IndexLookup is render-thread state
// that ClearOutputData reallocates from inside the packer's own MakeSpace.
// SEMANTICS: a pressure-triggered discard ERASES "already seen", so an
// atlas-backed budget would forget precisely under the pressure it exists to
// bound, and a cooperative peer's next message would be refused after an
// unrelated repack. So this keeps its own monotone set, touched only on the game
// thread, with no ImGui dependency at all.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::text {

// Admit a decoded remote string. Returns true if the peer stays within its
// novelty budget; false means the caller must REFUSE THE WHOLE FIELD, exactly as
// it already refuses ill-formed UTF-8 -- a partially-admitted string is a
// sentence nobody typed.
//
// `peerSlot` scopes the budget so one hostile peer cannot spend everyone's. Call
// on the GAME THREAD only.
//
// WIRED AT CHAT ONLY, and that is a measurement rather than an oversight. The
// other remote text field is the nickname, and it is bounded by CONSTRUCTION:
// kNickMaxChars codepoints x kMaxPeers peers is at most 80 codepoints for the
// whole lobby, ever. A field that cannot exceed a small constant is not an
// amplification vector, and charging it would add slot plumbing to the handshake
// decoder to police a bound that already holds. Chat is unbounded in count and
// attacker-chosen in content, which is why W11 names it and not the nick.
bool AdmitRemoteText(uint8_t peerSlot, const std::wstring& text);

// There is deliberately no ForgetPeer here. Slots RECYCLE lowest-free, so a slot
// goes person X -> person Y with no absence in between and a stale window would
// be inherited by the incoming occupant -- but that clear belongs to the roster
// ledger's per-slot teardown, which the window declares itself into
// (PerSlotState<T>). A manual clear beside an automatic one is a second owner.

// Everything a fresh process would have: an empty seen-set and no windows. The
// seen-set is deliberately NOT cleared on a session end -- what it models is what
// this PROCESS's atlas has been asked to rasterise, which does not un-happen.
void ResetForTests();

// Machine-asserted at boot beside the codec and repertoire selftests: the budget
// refuses a burst, admits the same text again for free (monotone), scopes per
// peer, and never counts a codepoint the atlas cannot bake anyway.
bool RunNoveltyLedgerSelftest();

}  // namespace coop::text
