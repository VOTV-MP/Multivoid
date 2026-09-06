//! TLS termination for the coop master + signaling binaries (Tier B).
//!
//! RULE 3: VPS infra, never ships in the mod. Design of record:
//! `research/findings/network/votv-tls-tier-b-c-DESIGN-2026-07-20.md`.
//!
//! Why we terminate TLS *inside* our own binaries instead of behind a reverse
//! proxy: :443 on the coop box belongs to a different tenant (measured
//! 2026-07-19: `ss -tlnp` shows xray on *:443 and *:8443), so there is no port
//! for an nginx/caddy front to own. The cert comes from Let's Encrypt over
//! HTTP-01 on :80 (measured free) and reaches these `DynamicUser=yes` services
//! through systemd `LoadCredential=`.
//!
//! TLS 1.2 IS REQUIRED, not optional: our clients are Windows and use schannel,
//! and Windows 10 19045 (the dev-install baseline) tops out at TLS 1.2 — a
//! TLS-1.3-only server would refuse every one of them. Hence the `tls12`
//! feature is enabled explicitly in Cargo.toml and must never be dropped.

use crate::common::{env_str, log};
use std::fs::File;
use std::io::BufReader;
use std::sync::Arc;
use tokio_rustls::rustls::pki_types::{CertificateDer, PrivateKeyDer};
use tokio_rustls::rustls::ServerConfig;
use tokio_rustls::TlsAcceptor;

/// Build a `TlsAcceptor` from `COOP_TLS_CERT` + `COOP_TLS_KEY` (PEM paths, as
/// handed over by systemd `LoadCredential`).
///
/// Returns `None` when EITHER var is unset/empty — that is the deliberate
/// plaintext-only mode used by the local differential harness and by the
/// parallel-port cutover window, NOT a silent downgrade of a configured
/// listener: a var that IS set but unreadable is a FATAL error (below), because
/// silently falling back to cleartext on a box that was meant to serve TLS is
/// exactly the failure this tier exists to prevent.
pub fn acceptor_from_env() -> Option<TlsAcceptor> {
    let cert_path = env_str("COOP_TLS_CERT", "");
    let key_path = env_str("COOP_TLS_KEY", "");
    if cert_path.is_empty() || key_path.is_empty() {
        // Unset means "plaintext mode" -- correct for the local differential
        // harness, WRONG for the public box, where coming up cleartext because a
        // provisioning step was skipped is a silent security regression. The
        // production env therefore sets COOP_REQUIRE_TLS=1 and the service
        // refuses to start instead: fail CLOSED by intent, not by accident.
        if env_str("COOP_REQUIRE_TLS", "") == "1" {
            log("FATAL: COOP_REQUIRE_TLS=1 but COOP_TLS_CERT/COOP_TLS_KEY are unset \
                 -- refusing to serve the control plane in cleartext");
            std::process::exit(1);
        }
        return None;
    }

    let certs = match load_certs(&cert_path) {
        Ok(c) if !c.is_empty() => c,
        Ok(_) => {
            log(&format!("FATAL: {cert_path} contains no certificates"));
            std::process::exit(1);
        }
        Err(e) => {
            log(&format!("FATAL: cannot read cert {cert_path}: {e}"));
            std::process::exit(1);
        }
    };
    let key = match load_key(&key_path) {
        Ok(Some(k)) => k,
        Ok(None) => {
            log(&format!("FATAL: {key_path} contains no private key"));
            std::process::exit(1);
        }
        Err(e) => {
            log(&format!("FATAL: cannot read key {key_path}: {e}"));
            std::process::exit(1);
        }
    };

    let mut cfg = match ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs, key)
    {
        Ok(c) => c,
        Err(e) => {
            log(&format!("FATAL: TLS config rejected the cert/key pair: {e}"));
            std::process::exit(1);
        }
    };
    // No TLS 1.3 session tickets. They are post-handshake messages that Windows
    // schannel surfaces to the client as SEC_I_RENEGOTIATE, and our client
    // deliberately does NOT implement a renegotiation re-entry machine (it logs
    // and reconnects). Emitting zero tickets keeps that branch unreachable
    // against our own server instead of relying on the client to tolerate it.
    cfg.send_tls13_tickets = 0;

    log(&format!("TLS enabled (cert={cert_path})"));
    Some(TlsAcceptor::from(Arc::new(cfg)))
}

fn load_certs(path: &str) -> std::io::Result<Vec<CertificateDer<'static>>> {
    let mut rd = BufReader::new(File::open(path)?);
    rustls_pemfile::certs(&mut rd).collect()
}

fn load_key(path: &str) -> std::io::Result<Option<PrivateKeyDer<'static>>> {
    let mut rd = BufReader::new(File::open(path)?);
    rustls_pemfile::private_key(&mut rd)
}
