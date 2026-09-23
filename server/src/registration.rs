//! The signaling relay's registration proof (security A59). A registered identity IS an Ed25519
//! public key -- since b144 a peer's durable key is its rendezvous name -- so its holder signs the
//! relay's nonce before the name may be addressed. The decision lives here, apart from the relay's
//! I/O, so known-answer vectors can watch it refuse.

use crate::common::hex_to_bytes;

// Domain tag for the registration proof. Deliberately DIFFERENT from the admission
// exchange's `multivoid-peer-admission-v1`: both are signed with the peer's one
// durable key, and the separation is what stops a hostile relay -- which chooses
// the nonce -- from steering a client into producing a signature that would also
// be a valid admission proof. Since the two tags differ in their first bytes, no
// choice of nonce can make one blob equal the other.
//
// The blob is tag ‖ identity ‖ nonce with no separators or lengths, which is
// unambiguous by construction rather than by convention: `identity_shape_ok`
// accepts exactly 68 characters and the nonce is exactly 64, so no two distinct
// (identity, nonce) pairs can produce the same bytes.
pub const REGISTER_TAG: &[u8] = b"multivoid-signaling-register-v1";

/// The bytes a registration signs: tag ‖ identity ‖ nonce.
pub fn blob(ident: &str, nonce: &str) -> Vec<u8> {
    let mut blob = Vec::with_capacity(REGISTER_TAG.len() + ident.len() + nonce.len());
    blob.extend_from_slice(REGISTER_TAG);
    blob.extend_from_slice(ident.as_bytes());
    blob.extend_from_slice(nonce.as_bytes());
    blob
}

/// The registration proof's whole DECISION, split from its I/O so it can be
/// exercised with known-answer vectors. A gate whose only exercise is a live
/// drill is one nobody has ever watched REFUSE, and that is indistinguishable
/// from `Ok(())` -- so the negatives below are the test, not the positive.
///
/// `ident` must already have passed `identity_shape_ok`; `nonce` is the 64-hex
/// value this connection was just sent, which is what makes a recorded proof
/// useless against the next one.
pub fn check_registration_proof(ident: &str, nonce: &str, auth_line: &str) -> Result<(), &'static str> {
    let hex = ident.strip_prefix("gen:").ok_or("not a key identity")?;
    let pubkey = hex_to_bytes::<32>(hex).ok_or("key identity did not decode")?;
    let sig_hex = auth_line.trim().strip_prefix("auth ").ok_or("expected an auth line")?;
    let sig = hex_to_bytes::<64>(sig_hex).ok_or("malformed proof")?;
    ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, &pubkey)
        .verify(&blob(ident, nonce), &sig)
        .map_err(|_| "does not hold the key this identity names")
}

#[cfg(test)]
mod tests {
    use super::{blob, check_registration_proof};
    use crate::common::identity_shape_ok;
    use ring::rand::SystemRandom;
    use ring::signature::{Ed25519KeyPair, KeyPair};

    /// A peer: its `gen:` identity and the key behind it. Built from a REAL
    /// keypair rather than fixed bytes so the positive arm exercises the same
    /// path a live client does.
    struct Peer {
        ident: String,
        kp: Ed25519KeyPair,
    }

    fn peer() -> Peer {
        let rng = SystemRandom::new();
        let pkcs8 = Ed25519KeyPair::generate_pkcs8(&rng).unwrap();
        let kp = Ed25519KeyPair::from_pkcs8(pkcs8.as_ref()).unwrap();
        let mut ident = String::from("gen:");
        for b in kp.public_key().as_ref() {
            ident.push_str(&format!("{b:02x}"));
        }
        Peer { ident, kp }
    }

    /// Sign `ident ‖ nonce` under the register tag and render the wire line.
    /// `as_ident` is what goes INTO the blob, which is how the squat arm makes a
    /// signature that names one identity while being offered under another.
    fn auth_line(p: &Peer, as_ident: &str, nonce: &str) -> String {
        let sig = p.kp.sign(&blob(as_ident, nonce));
        let mut out = String::from("auth ");
        for b in sig.as_ref() {
            out.push_str(&format!("{b:02x}"));
        }
        out
    }

    const N1: &str = "1111111111111111111111111111111111111111111111111111111111111111";
    const N2: &str = "2222222222222222222222222222222222222222222222222222222222222222";

    #[test]
    fn a_real_holder_registers() {
        let p = peer();
        assert!(
            identity_shape_ok(&p.ident),
            "the generated identity is not the shape the server gates on"
        );
        assert!(check_registration_proof(&p.ident, N1, &auth_line(&p, &p.ident, N1)).is_ok());
    }

    #[test]
    fn the_ways_to_not_hold_the_key_are_all_refused() {
        let victim = peer();
        let attacker = peer();
        assert_ne!(
            victim.ident, attacker.ident,
            "two peers minted the SAME identity -- every arm below would prove nothing"
        );

        // 1. SQUAT -- exactly A59: sign with your own key, register as someone
        //    else. The attacker signs a blob naming the VICTIM, which is the
        //    strongest form (a blob naming itself would never be offered).
        assert!(
            check_registration_proof(&victim.ident, N1, &auth_line(&attacker, &victim.ident, N1))
                .is_err()
        );

        // 2. REPLAY -- a proof recorded from an earlier connection, whose nonce
        //    this one never issued.
        assert!(
            check_registration_proof(&victim.ident, N2, &auth_line(&victim, &victim.ident, N1))
                .is_err()
        );

        // 3. NO PROOF -- not an auth line at all (what a peer that skips the step
        //    produces, and what a relayed peer line would look like).
        assert!(check_registration_proof(&victim.ident, N1, "gen:dead beef").is_err());
        assert!(check_registration_proof(&victim.ident, N1, "").is_err());

        // 4. MALFORMED -- right verb, wrong bytes. Uppercase is its own case: the
        //    identity alphabet is lowercase-only for a measured reason, and the
        //    proof must not be laxer than the name it proves.
        let good = auth_line(&victim, &victim.ident, N1);
        assert!(check_registration_proof(&victim.ident, N1, &good[..good.len() - 2]).is_err());
        assert!(check_registration_proof(&victim.ident, N1, &good.to_uppercase()).is_err());
    }

    #[test]
    fn one_flipped_bit_is_refused() {
        // The arm that fails if verification is ever reduced to a length check.
        let p = peer();
        let mut line = auth_line(&p, &p.ident, N1).into_bytes();
        let last = line.len() - 1;
        line[last] = if line[last] == b'0' { b'1' } else { b'0' };
        assert!(check_registration_proof(&p.ident, N1, &String::from_utf8(line).unwrap()).is_err());
    }
}
