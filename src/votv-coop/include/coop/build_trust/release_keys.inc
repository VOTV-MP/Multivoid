// The release public keys. One row per key:
//   MV_RELEASE_KEY(<id 1..254>, "<public key, 64 lowercase hex>", "<fixture signature, 128 lowercase hex>")
// One row per line, exactly this spacing: the release signer parses this file too.
// The fixture signature is the key's own signature over SignedMessage("selftest-fixture", 0, 32 zero
// bytes) -- a message no main.dll can match -- so the boot selftest proves this row decodes and verifies.
// The rows are printed by the release signer's keygen, which also parses this file. An id is never
// reused; 255 is the selftest's test key and never appears here.
MV_RELEASE_KEY(1, "ed7b1d9ac4e12c23a7e7ac63d10f93800823351dfeaa4b6e14f770d948961b5d", "bf2855a76ecaafcd1c8245515694e0ac30fb3f737ab922e9e9b9b6285645ed42762cc3b81bebd02532243f722c2cfc8bc363a52b4853cada66a52d4d8594210c")
