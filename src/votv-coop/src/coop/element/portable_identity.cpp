// coop/element/portable_identity.cpp -- see the header for the rule, its measurement,
// and why the game's Key is never written.

#include "coop/element/portable_identity.h"

#include "ue_wrap/actors/prop.h"        // GetInteractableKeyString (the generic Key read)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"   // UObject_ObjectFlags
#include "ue_wrap/engine/engine.h"      // IsChildActor / ParentActorOf

#include <mutex>
#include <set>

namespace coop::element {

namespace {

namespace R = ue_wrap::reflection;

// UE4.27 EObjectFlags. RF_WasLoaded is set on an object deserialised from a package, so
// it discriminates "my name came from the cooked level asset and is therefore identical
// on every machine running this build" from "my name carries a per-process allocation
// counter". Measured 2026-09-05: every one of the 85 name-resolvable instances carries
// 0x00280008 (RF_LoadCompleted|RF_WasLoaded|RF_Transactional); every serverBox_dish --
// whose name is per-process -- carries 0x00000000 or 0x00000008 and never RF_WasLoaded.
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
            // The anchor. Prefer the level-baked name; fall back to a Key that can only
            // have come from the save (a top-level runtime actor whose Key is non-None was
            // either save-loaded or minted -- and a MINTED one is not portable, which is
            // exactly why the caller must treat "" and a wrong answer as different things.
            // The measurement says every top-level actor reached here has a stable Key;
            // see the RE doc's three-population table).
            std::wstring anchor;
            if (WasLoaded(cur)) {
                anchor = L"n:" + R::ToString(R::NameOf(cur));
            } else {
                const std::wstring key = ue_wrap::prop::GetInteractableKeyString(cur);
                if (key.empty() || key == L"None") {
                    // NO IDENTITY, and say so loudly ONCE per class rather than returning a
                    // guess. Two different things land here and the log must not hide either:
                    // an actor whose Key really is None, and an actor of a class
                    // `GetInteractableKey` cannot read at all -- it covers the Aprop_C lineage,
                    // trashBitsPile and chipPile/clump, and returns None for AtriggerBase_C
                    // (prop.cpp:250-266). Deliberately NOT widened here: that reader has 33
                    // call sites, all in the prop lanes, and widening it would change what every
                    // one of them sees (OPUS 8 -- the firing set changes even though the code
                    // does not).
                    static std::mutex sSeenMu;
                    static std::set<std::wstring> sSeen;
                    const std::wstring cls = R::ClassNameOf(cur);
                    bool first = false;
                    { std::lock_guard<std::mutex> lk(sSeenMu); first = sSeen.insert(cls).second; }
                    if (first)
                        UE_LOGI("portable_identity: no identity for anchor class '%ls' -- not "
                                "RF_WasLoaded and no readable Key (first of this class)",
                                cls.c_str());
                    return std::wstring();
                }
                anchor = L"k:" + key;
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
