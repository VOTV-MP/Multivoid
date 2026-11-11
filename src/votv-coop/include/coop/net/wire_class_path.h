// coop/net/wire_class_path.h -- the content path of a class, carried in the spare bytes of a WireClassName.
//
// A wire class name is the class's short leaf ("prop_food_mre_AB_C"), which a peer that has not loaded the class cannot
// turn back into the class: the game loads a Blueprint class on first use, and a name lookup finds only a loaded one.
// The sender knows where the class lives (its package, "/Game/objects/prop_food_mre_AB"), so that rides with the name,
// and a receiver that misses the lookup loads the class from it (ue_wrap/core/class_load.h).
//
// The bytes are the ones a WireClassName leaves unused (`data` holds `len` characters and the rest was always zero), so
// the struct, every payload that holds one and the protocol number are unchanged:
//
//     data[0 .. len)        the leaf, as before (every existing reader stops at len)
//     data[len]             0 -- the leaf stays a C string for a reader that ignores len
//     data[len + 1]         t: the path's length n (1..62) in the low 7 bits, bit 7 = kFullPackage
//     data[len + 2 .. +n)   the path characters
//
// With bit 7 clear the path is the package's directory ("/Game/objects/", ending in '/'), and the package is that
// directory plus the leaf without its "_C" (a Blueprint class sits in a package named for its asset, which is every
// prop class there is); with bit 7 set it is the whole package name (a class whose package is named otherwise). The object
// path the loader takes is the package, a dot and the leaf. A peer that predates this reads the leaf and ignores the
// rest; a payload from such a peer has zeros after the leaf, which reads as "no path". A path that does not fit the 63
// bytes is left off, and the receiver is where it was.

#pragma once

#include "coop/net/protocol.h"

#include <string>

namespace coop::net::wire_class_path {

inline constexpr uint8_t kFullPackage = 0x80;

// Write `leaf` into `cn` (as the callers' loops did) and, when `objectPath` ("<package>.<leaf>") names this leaf in a
// /Game/ package and fits, its path after it. An empty or unfitting path leaves just the leaf. Returns whether a path
// was written.
bool Fill(WireClassName& cn, const std::wstring& leaf, const std::wstring& objectPath);

// The leaf alone, as a wide string (the old reader).
std::wstring Leaf(const WireClassName& cn);

// The object path the name carries ("/Game/objects/x.x_C"), empty when it carries none or the bytes do not parse.
// Not checked for loadability: class_load::IsLoadablePath is the receiver's gate.
std::wstring ObjectPath(const WireClassName& cn);

// Whether the name carries a path.
inline bool HasPath(const WireClassName& cn) { return !ObjectPath(cn).empty(); }

// The un-gated codec selftest (round trips, the full-package form, a path that does not fit, an old peer's name,
// hostile bytes); logs "wire_class_path selftest: ALL PASS" or the first failure.
void RunSelftest();

}  // namespace coop::net::wire_class_path
