#!/usr/bin/env bash
# VOTV coop signaling + master + coturn provisioning for a SHARED VPS (COEXIST).
#
# Servers are the RUST binaries (tools/coop-server-rs; static musl ELF, uploaded
# alongside this script). No Python lands on the box (RULE 2: the .py originals
# are retired).
#
# Idempotent. Touches NOTHING of the box's other tenant services or sshd.
# Adds only:
#   - token-authed signaling relay          tcp/10000      (systemd: coop-signaling)
#   - HTTP master / lobby server            tcp/10001      (systemd: coop-master)
#   - coturn STUN/TURN                      udp+tcp/3478   relay 61000-61100
#
# coturn is hardened for a shared box:
#   - use-auth-secret (REST / EPHEMERAL creds): the master mints short-lived
#     username="<expiry>:<identity>" / password=base64(HMAC-SHA1(TURN_SECRET,user)),
#     coturn validates with static-auth-secret=TURN_SECRET. NO static user account =
#     no long-lived shared password to leak (the old lt-cred interim is retired).
#   - max-bps per allocation + total-quota: a leaked cred relays a bounded stream for
#     a bounded time, can't saturate the box's uplink.
#   - denied-peer-ip (v4 AND v6) + no-loopback-peers: a TURN user CANNOT pivot to the
#     box's localhost services or RFC1918.
#
# ONE secret, TWO consumers: TURN_SECRET is fed to coturn (static-auth-secret) AND to
# the master (COOP_TURN_SECRET) so the master's creds validate. SIG_TOKEN likewise is
# the signaling bearer AND is handed to clients by the master in /v1/host /v1/join.
#
# Upload BEFORE running (all to /root):
#   coop_provision.sh (this file), coop-master, coop-signaling
#   (the two static musl ELFs from tools/coop-server-rs/target/x86_64-unknown-linux-musl/release/)
#
# Run as root:  bash /root/coop_provision.sh
set -euo pipefail

SIG_PORT=10000
MASTER_PORT=10001
TURN_PORT=3478
RELAY_MIN=61000
RELAY_MAX=61100
SIG_DIR=/opt/coop-signaling-rs
MASTER_DIR=/opt/coop-master-rs
REALM=votv.mp

LOCAL_IP="$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oP 'src \K\S+' || hostname -I | awk '{print $1}')"
# -4: the URIs we hand clients must be IPv4 -- a dual-stack box answers ifconfig.me
# over v6 and the master would hand out an unbracketed-IPv6 signaling/STUN URI
# (measured on the Cloudzy box, 2026-07-16).
PUBLIC_IP="$(curl -4s --max-time 8 ifconfig.me || echo "$LOCAL_IP")"
[ -z "$PUBLIC_IP" ] && PUBLIC_IP="$LOCAL_IP"

echo "=== coop provision (COEXIST) ==="
echo "LOCAL_IP=$LOCAL_IP  PUBLIC_IP=$PUBLIC_IP"

# Safety guard: assert we are NOT about to clobber a port of the box's own
# services (sshd + the other tenants' listeners; superset of both boxes).
for reserved in 443 8443 2096 43766 57210 22; do  # (TS3 ports dropped 2026-07-16: container removed)
  for ours in "$SIG_PORT" "$MASTER_PORT" "$TURN_PORT"; do
    if [ "$ours" = "$reserved" ]; then
      echo "ABORT: chosen port $ours collides with reserved $reserved"; exit 1
    fi
  done
done

# --- shared secrets (persisted + reused on re-provision so already-configured
#     clients stay valid) ---
SIG_TOKEN_FILE=/etc/coop-signaling-token
if [ -f "$SIG_TOKEN_FILE" ]; then
  SIG_TOKEN="$(cat "$SIG_TOKEN_FILE")"
else
  SIG_TOKEN="$(openssl rand -hex 24)"
  printf '%s' "$SIG_TOKEN" > "$SIG_TOKEN_FILE"; chmod 600 "$SIG_TOKEN_FILE"
fi
TURN_SECRET_FILE=/etc/coop-turn-secret
if [ -f "$TURN_SECRET_FILE" ]; then
  TURN_SECRET="$(cat "$TURN_SECRET_FILE")"
else
  TURN_SECRET="$(openssl rand -hex 32)"
  printf '%s' "$TURN_SECRET" > "$TURN_SECRET_FILE"; chmod 600 "$TURN_SECRET_FILE"
fi

# --- 1. signaling server (systemd) ---
# Rust static ELF. install(1) would clobber a RUNNING binary (ETXTBSY-safe only
# because install unlinks first) -- but we still stop-before-replace on
# re-provision so the hash on disk is always the hash that is running.
mkdir -p "$SIG_DIR"
if [ -f /root/coop-signaling ]; then
  systemctl stop coop-signaling.service 2>/dev/null || true
  install -m 0755 /root/coop-signaling "$SIG_DIR/coop-signaling"
fi
if [ ! -x "$SIG_DIR/coop-signaling" ]; then
  echo "ABORT: $SIG_DIR/coop-signaling missing (upload /root/coop-signaling first)"; exit 1
fi
# Env file (root-600). systemd reads it as root and passes the vars to the
# sandboxed DynamicUser process, so the token never needs to be world-readable.
cat > /etc/coop-signaling.env <<EOF
COOP_SIGNALING_PORT=$SIG_PORT
COOP_SIGNALING_TOKEN=$SIG_TOKEN
EOF
chmod 600 /etc/coop-signaling.env

cat > /etc/systemd/system/coop-signaling.service <<EOF
[Unit]
Description=VOTV coop P2P signaling server (async, token-authed)
# network-online (not just network) so the public IP is assigned before bind --
# matters on reboot (Audit, 2026-06-05).
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
EnvironmentFile=/etc/coop-signaling.env
ExecStart=$SIG_DIR/coop-signaling
Restart=always
RestartSec=3
# Sandboxed + resource-capped: a token-authed but public service must not be root,
# and the caps stop a connection flood from starving the shared box -- the cgroup
# is OOM-killed first, never the box's other tenants. (Audit F2/#8, 2026-06-05.)
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
RestrictAddressFamilies=AF_INET AF_INET6
MemoryMax=128M
TasksMax=256
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
EOF

# --- 2. master / lobby server (systemd) ---
mkdir -p "$MASTER_DIR"
if [ -f /root/coop-master ]; then
  systemctl stop coop-master.service 2>/dev/null || true
  install -m 0755 /root/coop-master "$MASTER_DIR/coop-master"
fi
if [ ! -x "$MASTER_DIR/coop-master" ]; then
  echo "ABORT: $MASTER_DIR/coop-master missing (upload /root/coop-master first)"; exit 1
fi
# The master holds BOTH secrets: COOP_TURN_SECRET (to mint coturn REST creds) and
# COOP_SIGNALING_TOKEN (handed to clients so they can greet signaling). It also tells
# clients where signaling / STUN / TURN live (PUBLIC_IP).
cat > /etc/coop-master.env <<EOF
COOP_MASTER_PORT=$MASTER_PORT
COOP_TURN_SECRET=$TURN_SECRET
COOP_SIGNALING_TOKEN=$SIG_TOKEN
COOP_SIGNALING_URL=$PUBLIC_IP:$SIG_PORT
COOP_STUN_URI=stun:$PUBLIC_IP:$TURN_PORT
COOP_TURN_URI=turn:$PUBLIC_IP:$TURN_PORT
# Latest released mod, served by /v1/latest -- bump on each release + restart coop-master
# (no rebuild; overrides the compiled-in default).
COOP_LATEST_PROTO=111
EOF
chmod 600 /etc/coop-master.env

cat > /etc/systemd/system/coop-master.service <<EOF
[Unit]
Description=VOTV coop master / lobby server (async HTTP)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
EnvironmentFile=/etc/coop-master.env
ExecStart=$MASTER_DIR/coop-master
Restart=always
RestartSec=3
# Same sandbox/cap posture as signaling -- a public HTTP endpoint on the shared box
# is bounded to its own cgroup (the in-process MAX_CONNS cap + MemoryMax = the flood
# OOM-kills this unit, never the box's other tenants). (Audit C2, 2026-06-05.)
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
RestrictAddressFamilies=AF_INET AF_INET6
MemoryMax=128M
TasksMax=256
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable coop-signaling.service coop-master.service
# restart (not just enable --now): enable --now is a no-op when the service is
# already running, so a changed unit/ExecStart/env would NOT be applied on a
# re-provision. restart guarantees the current code + unit are live.
systemctl restart coop-signaling.service coop-master.service

# --- 3. coturn (STUN/TURN), REST/ephemeral-cred mode ---
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq coturn

cat > /etc/turnserver.conf <<EOF
# VOTV coop coturn -- COEXIST on a shared VPS. Relay range is above the other tenants'
# observed UDP ports. use-auth-secret = ephemeral REST creds (the master mints them;
# no static account to leak). denied-peer-ip blocks pivoting to localhost/private.
listening-port=$TURN_PORT
min-port=$RELAY_MIN
max-port=$RELAY_MAX
relay-ip=$LOCAL_IP
external-ip=$PUBLIC_IP
# Ephemeral REST credentials (design 7): coturn validates the master's
# username="<expiry>:<identity>", password=base64(HMAC-SHA1(static-auth-secret,user)).
use-auth-secret
static-auth-secret=$TURN_SECRET
realm=$REALM
fingerprint
no-cli
no-tls
no-dtls
# Bound abuse on the shared box: cap simultaneous relay allocations (total-quota) AND
# per-allocation bandwidth (max-bps, bytes/sec -- sized well above one coop stream's
# ~tens of KB/s, but caps a leaked cred from saturating the box's uplink). Audit H2.
total-quota=200
max-bps=262144
# user-quota: per-username simultaneous-allocation cap. Sized for a NAT'd household
# (the master now binds the REST username to the client IP bucket). NOTE: coturn's
# REST username carries a rolling expiry, so this does not fully aggregate a rotating
# source; the master's per-/64 rate limit on cred minting is the primary bound. DiD.
user-quota=12
# Aggregate server bandwidth ceiling (bytes/sec, all sessions) so a leaked/abused cred
# pool cannot saturate the shared uplink even at total-quota x max-bps. Sized well
# above realistic coop relay (~hundreds of KB/s per household). Audit M2/M5. 0=unlimited.
bps-capacity=8388608
stale-nonce=600
no-multicast-peers
# Block relaying to loopback/private -- v4 AND v6 (an IPv6 / v4-mapped pivot to the
# box's own localhost services on 127.0.0.1/::1 must be closed). Audit + M5 (CGNAT + v4-mapped).
no-loopback-peers
denied-peer-ip=0.0.0.0-0.255.255.255
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=100.64.0.0-100.127.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255
denied-peer-ip=::1
denied-peer-ip=::ffff:0.0.0.0-::ffff:255.255.255.255
denied-peer-ip=fc00::-fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff
denied-peer-ip=fe80::-febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff
EOF

# Ubuntu gates the coturn service behind this flag.
if grep -q '^#\?TURNSERVER_ENABLED' /etc/default/coturn 2>/dev/null; then
  sed -i 's/^#\?TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/default/coturn
else
  echo 'TURNSERVER_ENABLED=1' >> /etc/default/coturn
fi
systemctl enable coturn >/dev/null 2>&1 || true
systemctl restart coturn

# --- 4. firewall (ufw) -- only if it is already ACTIVE on the box (the Cloudzy
# box ships ufw default-deny-incoming; the old box ran no ufw). We only ADD
# allows for OUR ports; nothing of the box's own rules is touched. 80/tcp is
# for Let's Encrypt HTTP-01 (Tier B TLS cert issuance/renewal).
if ufw status 2>/dev/null | grep -q "Status: active"; then
  ufw allow "$SIG_PORT/tcp"    comment 'coop signaling'        >/dev/null
  ufw allow "$MASTER_PORT/tcp" comment 'coop master'           >/dev/null
  ufw allow "$TURN_PORT/tcp"   comment 'coop coturn stun/turn' >/dev/null
  ufw allow "$TURN_PORT/udp"   comment 'coop coturn stun/turn' >/dev/null
  ufw allow "$RELAY_MIN:$RELAY_MAX/udp" comment 'coop turn relay' >/dev/null
  ufw allow 80/tcp             comment 'coop letsencrypt http-01' >/dev/null
  echo "ufw: coop ports allowed ($SIG_PORT, $MASTER_PORT, $TURN_PORT, $RELAY_MIN-$RELAY_MAX/udp, 80)"
fi

# Cap journald so neither our logs nor coturn's can fill the small disk and wedge
# the shared box (which would take the box's other tenants down too). (Audit F1, 2026-06-05.)
mkdir -p /etc/systemd/journald.conf.d
cat > /etc/systemd/journald.conf.d/coop-cap.conf <<'EOF'
[Journal]
SystemMaxUse=200M
EOF
systemctl restart systemd-journald 2>/dev/null || true
journalctl --vacuum-size=200M >/dev/null 2>&1 || true

sleep 2
echo "=== RESULT ==="
echo "signaling  : $(systemctl is-active coop-signaling) tcp/$SIG_PORT (rust, token-authed) sha256:$(sha256sum "$SIG_DIR/coop-signaling" | cut -c1-12)"
echo "master     : $(systemctl is-active coop-master) tcp/$MASTER_PORT (rust HTTP) sha256:$(sha256sum "$MASTER_DIR/coop-master" | cut -c1-12)"
echo "coturn     : $(systemctl is-active coturn) udp+tcp/$TURN_PORT relay $RELAY_MIN-$RELAY_MAX (use-auth-secret)"
# Print only a NON-reversible fingerprint of each secret, never the raw value (audit
# M4): a raw echo lands in terminal scrollback / shell history / journald / CI capture,
# turning a one-time provision into a secret-at-rest leak outside the root-600 files.
echo "SIG_TOKEN  : sha256:$(printf '%s' "$SIG_TOKEN" | sha256sum | cut -c1-12)  (written to $SIG_TOKEN_FILE, chmod 600)"
echo "TURN_SECRET: sha256:$(printf '%s' "$TURN_SECRET" | sha256sum | cut -c1-12)  (written to $TURN_SECRET_FILE, chmod 600)"
echo "MASTER_URL : http://$PUBLIC_IP:$MASTER_PORT  (GET /v1/lobbies, /healthz)"
echo "STUN_URL   : stun:$PUBLIC_IP:$TURN_PORT"
echo "TURN_URL   : turn:$PUBLIC_IP:$TURN_PORT"
echo "--- listeners ---"
ss -tulpn 2>/dev/null | grep -E ":$SIG_PORT|:$MASTER_PORT|:$TURN_PORT" || echo "(no matching listeners -- check service status)"
echo "--- service status ---"
systemctl --no-pager --lines=3 status coop-signaling coop-master coturn 2>/dev/null | grep -E "Active:|Main PID:|coop-|turnserver" || true
