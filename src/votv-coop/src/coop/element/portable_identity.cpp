// coop/element/portable_identity.cpp -- see the header for the rule, its measurement,
// and why the game's Key is never written.

#include "coop/element/portable_identity.h"

#include "ue_wrap/actors/prop.h"        // GetInteractableKeyString (the generic Key read)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"   // UObject_ObjectFlags
#include "ue_wrap/engine/engine.h"      // IsChildActor / ParentActorOf
#include "ue_wrap/desk/dish.h"          // IndexOf (the keyless dish's stable ordinal)

#include <mutex>
#include <set>

namespace coop::element {

namespace {

namespace R = ue_wrap::reflection;

// UE4.27 EObjectFlags. RF_WasLoaded is set on an object deserialised from a package, so it
// discriminates "my name came from the cooked level asset and is therefore identical on every
// machine running this build" from "my name carries a per-process allocation counter". Measured
// over a loaded world: every one of the 85 name-resolvable instances carries 0x00280008
// (RF_LoadCompleted|RF_WasLoaded|RF_Transactional), while every serverBox_dish -- whose name is
// per-process -- carries 0x00000000 or 0x00000008 and never RF_WasLoaded.
constexpr uint32_t kRfWasLoaded = 0x00080000u;

// A child-actor chain deeper than this is a cycle or a layout misread; either way the
// game thread must not spin. The same 16 the setKey resolver uses for its SuperStruct
// climb (prop_synth_key.cpp:50).
constexpr int kMaxChainDepth = 16;

bool WasLoaded(void* obj) {
    if (!obj) return false;
    return (*reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(obj) +
                                               ue_wrap::profile::off::UObject_ObjectFlags) &
            kRfWasLoaded) != 0;
}

}  // namespace

std::wstring PortableIdentity(void* actor) {
    if (!actor) return std::wstring();

    // Walk UP the child-actor chain first, collecting component names, then compose from
    // the anchor down. Iterative rather than recursive so the depth cap is visible and a
    // deep chain cannot blow the game thread's stack.
    std::wstring suffix;
    void* cur = actor;
    for (int depth = 0; depth < kMaxChainDepth; ++depth) {
        if (!ue_wrap::engine::IsChildActor(cur)) {
            // The anchor.
            // ORDER: the KEY first, then the name. That is not a preference. A level-placed anchor
            // is DESTROYED during the world load and replaced by its save-loaded twin, so the same
            // physical object is indexed first under a loaded name (flags 0x00280008) and seconds
            // later under a per-process one (flags 0x00000008) at the same location and component.
            // The NAME dies with the incarnation; the KEY survives it -- both incarnations read the
            // same pkey -- and every anchor key in the world is present in the .sav, so the key is
            // the persisted quantity and the name the transient one. Preferring the name would hand
            // one physical object two identities inside a single session.
            std::wstring anchor;
            std::wstring key = ue_wrap::prop::GetInteractableKeyString(cur);
            if (key.empty() || key == L"None")
                key = ue_wrap::prop::GetActorSaveKeyString(cur);   // the OTHER half of the key surface
            if (!key.empty() && key != L"None") {
                anchor = L"k:" + key;
            } else if (int dishIdx = ue_wrap::dish::IndexOf(cur); dishIdx >= 0) {
                // The dish (Adish_C) is a level-placed anchor with NO save Key (not Aprop_C /
                // Aactor_save_C) and -- per the measurement above -- never RF_WasLoaded, so it
                // reaches here with nothing to anchor on and its child (the door) used to resolve
                // to NO identity. Its stable cross-peer identity is its ordinal in the
                // level-authored, index-sorted mainGamemode.dishs array: the SAME quantity
                // dish_sync pairs on, identical on every machine that loads this build. (The
                // name branch below would be per-process and is deliberately NOT used for it.)
                wchar_t idxBuf[16];
                swprintf(idxBuf, 16, L"d:%d", static_cast<int>(dishIdx));
                anchor = idxBuf;
            } else if (WasLoaded(cur)) {
                anchor = L"n:" + R::ToString(R::NameOf(cur));
            } else {
                // NO IDENTITY -- and say so loudly ONCE per class rather than returning a guess.
                // Two different things land here and the log must not hide either: an actor whose
                // Key really is None, and an actor of a class `ue_wrap::prop::GetInteractableKey`
                // cannot read AT ALL. That reader covers the Aprop_C lineage, trashBitsPile and
                // chipPile/clump, and returns FName{0,0} for everything else, which `ToString`
                // renders as the string "None", byte identical to a genuinely keyless actor
                // (prop.cpp:250-266); `AtriggerBase_C` is outside it. Deliberately NOT widened: the
                // reader has 33 call sites, all in the prop lanes, and widening it changes what
                // every one of them sees even though none of their code changes. So the line names
                // the CLASS, which is what tells the two cases apart.
                static std::mutex sSeenMu;
                static std::set<std::wstring> sSeen;
                const std::wstring cls = R::ClassNameOf(cur);
                bool first = false;
                { std::lock_guard<std::mutex> lk(sSeenMu); first = sSeen.insert(cls).second; }
                if (first)
                    UE_LOGI("portable_identity: no identity for anchor class '%ls' -- neither "
                            "RF_WasLoaded nor a Key this reader can see (first of this class)",
                            cls.c_str());
                return std::wstring();
            }
            return anchor + suffix;
        }
        std::wstring comp;
        void* parent = ue_wrap::engine::ParentActorOf(cur, &comp);
        if (!parent || comp.empty()) return std::wstring();  // the link is gone this frame
        suffix = L"/" + comp + suffix;
        cur = parent;
    }
    UE_LOGW("portable_identity: child-actor chain deeper than %d on actor %p -- refusing "
            "to derive an identity (cycle or layout drift)", kMaxChainDepth, actor);
    return std::wstring();
}

uint64_t IdentityHash(const std::wstring& readable) {
    // FNV-1a-64 over the UTF-16 code units, low byte then high byte, so the value does
    // not depend on wchar_t's size or the host's endianness -- two peers on the same
    // build must agree byte for byte, and this is the whole mechanism by which they do.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (wchar_t c : readable) {
        const uint16_t u = static_cast<uint16_t>(c);
        h ^= static_cast<uint8_t>(u & 0xFF);       h *= 0x100000001b3ULL;
        h ^= static_cast<uint8_t>((u >> 8) & 0xFF); h *= 0x100000001b3ULL;
    }
    return h;
}

std::wstring PortableWireKey(void* actor) {
    const std::wstring readable = PortableIdentity(actor);
    if (readable.empty()) return std::wstring();
    wchar_t buf[24];
    swprintf(buf, 24, L"mv_%016llx",
             static_cast<unsigned long long>(IdentityHash(readable)));
    return std::wstring(buf);
}

bool RunSelfTest() {
    int checks = 0, failed = 0;
    auto CHECK = [&](bool cond, const char* what) {
        ++checks;
        if (!cond) { ++failed; UE_LOGE("portable_identity selftest FAIL: %s", what); }
    };

    // 1. DETERMINISM -- the same string must hash the same way twice.
    CHECK(IdentityHash(L"n:door5_133") == IdentityHash(L"n:door5_133"), "hash is deterministic");

    // 2. PINNED VALUES. These are the whole cross-machine contract: a build that changes the
    //    hash silently re-keys every portable identity and every peer stops agreeing with every
    //    OTHER build. Pinning them means such a change cannot ship unnoticed -- it turns a
    //    silent wire-compat break into a red selftest. (FNV-1a-64 over the UTF-16 code units,
    //    low byte then high byte.)
    CHECK(IdentityHash(L"") == 0xcbf29ce484222325ULL, "empty string is the FNV offset basis");
    CHECK(IdentityHash(L"a") == 0x089be207b544f1e4ULL, "single ASCII char pinned");
    CHECK(IdentityHash(L"n:dish19/lightroot") == 0x5824277a40808c78ULL, "a real identity pinned");

    // 3. DISTINCTNESS on the shapes that actually collide in this domain: the same component
    //    under two parents, and two components under one parent.
    CHECK(IdentityHash(L"n:dish19/door") != IdentityHash(L"n:dish5/door"), "parent distinguishes");
    CHECK(IdentityHash(L"n:dish19/door") != IdentityHash(L"n:dish19/lightswitch"), "component distinguishes");

    // 3b. The DISH-INDEX shape (`d:<ordinal>`) is the portable identity a keyless,
    //     non-WasLoaded dish (a level-placed Adish_C) gives its child actors such as the door.
    //     It must be a well-formed identity of its own: distinct from the name anchor of the
    //     same component, and two different dish indices must not collide.
    CHECK(IdentityHash(L"d:5/door") != IdentityHash(L"n:dish5/door"), "dish-index anchor distinct from name");
    CHECK(IdentityHash(L"d:5/door") != IdentityHash(L"d:19/door"), "dish index distinguishes");

    // 4. NON-ASCII must not be truncated to its low byte -- two Cyrillic names one code unit
    //    apart must not collide (the wchar-truncation bug class the text lane already paid for).
    CHECK(IdentityHash(L"n:дверь") != IdentityHash(L"n:дверъ"),
          "non-ASCII is hashed by code unit, not by low byte");

    // 5. The WIRE TOKEN's shape: 19 chars, `mv_` + 16 lowercase hex. It must fit the 31-char
    //    WireKey with room to spare, or the wire silently truncates the identity.
    const std::wstring tok = L"mv_" + [] {
        wchar_t b[24]; swprintf(b, 24, L"%016llx", 0x0123456789abcdefULL); return std::wstring(b); }();
    CHECK(tok.size() == 19, "wire token is 19 characters");
    CHECK(tok == L"mv_0123456789abcdef", "wire token format is mv_ + 16 lowercase hex");

    if (failed == 0) UE_LOGI("portable_identity selftest: ALL PASS (%d checks)", checks);
    else             UE_LOGE("portable_identity selftest: %d of %d checks FAILED", failed, checks);
    return failed == 0;
}

}  // namespace coop::element
