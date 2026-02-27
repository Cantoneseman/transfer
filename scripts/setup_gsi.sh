#!/bin/bash
#
# HYPER-TRANSFER GSI Environment Setup Script (Enhanced v2)
#
# One-click GSI environment for dual Ubuntu 22.04 nodes.
# Supports FQDN override via /etc/hosts without modifying real hostname.
#
# Features:
# - NTP/Chrony time sync verification (GSI is clock-sensitive)
# - FQDN pseudo-domain via /etc/hosts (no real hostname change)
# - Self-signed CA + per-node Host & User certificates
# - Strict permission enforcement (400 for keys, 644 for certs)
# - grid-mapfile generation for authorization
# - Automatic grid-proxy-init integration
#
# Usage:
#   On Node A: SERVER_CN_OVERRIDE=nodea.hyper.local ./setup_gsi.sh
#   On Node B: SERVER_CN_OVERRIDE=nodeb.hyper.local ./setup_gsi.sh
#
# Environment Variables:
#   SERVER_CN_OVERRIDE  - Override the server CN (default: auto-detect)
#   REMOTE_NODE_IP      - IP of the peer node (for /etc/hosts hint)
#   REMOTE_NODE_FQDN    - FQDN of the peer node
#   SKIP_NTP_CHECK      - Set to 1 to skip NTP verification
#   NON_INTERACTIVE     - Set to 1 to skip all prompts (auto-yes)
#

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

NODE_A_IP="192.168.1.114"
NODE_B_IP="192.168.1.203"
NODE_A_FQDN="nodea.hyper.local"
NODE_B_FQDN="nodeb.hyper.local"

WORK_DIR="$HOME/my_grid_security"
GLOBUS_DIR="$HOME/.globus"
CERTS_DIR="$GLOBUS_DIR/certificates"

# Detect which node we are on
LOCAL_IP=$(ip -4 addr show scope global | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | head -1)

if [[ "$LOCAL_IP" == "$NODE_A_IP" ]]; then
    LOCAL_FQDN="$NODE_A_FQDN"
    REMOTE_IP="$NODE_B_IP"
    REMOTE_FQDN="$NODE_B_FQDN"
elif [[ "$LOCAL_IP" == "$NODE_B_IP" ]]; then
    LOCAL_FQDN="$NODE_B_FQDN"
    REMOTE_IP="$NODE_A_IP"
    REMOTE_FQDN="$NODE_A_FQDN"
else
    echo "[WARN] Cannot auto-detect node role from IP=$LOCAL_IP"
    LOCAL_FQDN="${SERVER_CN_OVERRIDE:-$(hostname -f)}"
    REMOTE_IP="${REMOTE_NODE_IP:-}"
    REMOTE_FQDN="${REMOTE_NODE_FQDN:-}"
fi

# Allow override
SERVER_CN="${SERVER_CN_OVERRIDE:-$LOCAL_FQDN}"

# Certificate subjects — use -subj with proper escaping
# GSI host cert CN format: "host/FQDN" but openssl -subj uses '/' as RDN separator
# Solution: pass subject components individually via config or use escaped format
CA_SUBJECT="/C=CN/O=Grid/OU=HyperTransfer/CN=HyperCA"
USER_CN="${USER:-$(whoami)}"
USER_SUBJECT="/C=CN/O=Grid/OU=HyperTransfer/CN=$USER_CN"

# For host cert, we embed the FQDN directly as CN (without "host/" prefix in -subj)
# The "host/" prefix causes issues with openssl's subject parser.
# GSI will match based on the CN value; we use GLOBUS_HOSTNAME env var for mapping.
HOST_CN="$SERVER_CN"
SERVER_SUBJECT="/C=CN/O=Grid/OU=HyperTransfer/CN=$HOST_CN"

# Validity period (days)
CA_DAYS=3650      # 10 years
CERT_DAYS=365     # 1 year

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# ============================================================================
# Helper Functions
# ============================================================================

log_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[ OK ]${NC} $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
log_section() {
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN} $1${NC}"
    echo -e "${GREEN}========================================${NC}"
}

confirm_or_skip() {
    if [[ "${NON_INTERACTIVE:-0}" == "1" ]]; then return 0; fi
    read -p "$1 (y/N): " -n 1 -r; echo
    [[ $REPLY =~ ^[Yy]$ ]]
}

# ============================================================================
# Pre-flight Checks
# ============================================================================

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║     HYPER-TRANSFER GSI Environment Setup (Enhanced v2)     ║"
echo "║     Dual-Node GridFTP Certificate Infrastructure           ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "  Local IP:     $LOCAL_IP"
echo "  Local FQDN:   $SERVER_CN"
echo "  Remote IP:    ${REMOTE_IP:-N/A}"
echo "  Remote FQDN:  ${REMOTE_FQDN:-N/A}"
echo "  User CN:      $USER_CN"
echo ""

# ----------------------------------------------------------------------------
# Step 0: NTP / Time Synchronization Check
# ----------------------------------------------------------------------------

log_section "Step 0: Time Synchronization Check"

if [[ "${SKIP_NTP_CHECK:-0}" == "1" ]]; then
    log_warn "NTP check skipped (SKIP_NTP_CHECK=1)"
else
    NTP_OK=false

    # Try timedatectl first (systemd)
    if command -v timedatectl &>/dev/null; then
        SYNC_STATUS=$(timedatectl show --property=NTPSynchronized --value 2>/dev/null || echo "no")
        if [[ "$SYNC_STATUS" == "yes" ]]; then
            NTP_OK=true
            log_success "System clock synchronized via systemd-timesyncd"
            timedatectl status 2>/dev/null | grep -E '(Local time|NTP|synchronized)' | while read -r line; do
                log_info "  $line"
            done
        fi
    fi

    # Try chronyc
    if ! $NTP_OK && command -v chronyc &>/dev/null; then
        if chronyc tracking &>/dev/null; then
            OFFSET=$(chronyc tracking 2>/dev/null | grep 'Last offset' | awk '{print $4}')
            NTP_OK=true
            log_success "Chrony active, last offset: ${OFFSET}s"
        fi
    fi

    # Try ntpq
    if ! $NTP_OK && command -v ntpq &>/dev/null; then
        if ntpq -p &>/dev/null; then
            NTP_OK=true
            log_success "NTP daemon active"
        fi
    fi

    if ! $NTP_OK; then
        log_error "No NTP synchronization detected!"
        log_error "GSI certificates are extremely sensitive to clock skew."
        log_error "Please install and enable one of: systemd-timesyncd, chrony, ntp"
        log_error "  sudo apt install -y systemd-timesyncd && sudo timedatectl set-ntp true"
        exit 1
    fi
fi

# ----------------------------------------------------------------------------
# Step 1: FQDN Setup via /etc/hosts (no real hostname change)
# ----------------------------------------------------------------------------

log_section "Step 1: FQDN Setup via /etc/hosts"

HOSTS_CHANGED=false

# Check if local FQDN entry exists
if ! grep -q "$LOCAL_IP.*$SERVER_CN" /etc/hosts 2>/dev/null; then
    log_warn "Missing /etc/hosts entry: $LOCAL_IP $SERVER_CN"
    log_info "Please run (requires sudo):"
    echo ""
    echo "  echo '$LOCAL_IP $SERVER_CN' | sudo tee -a /etc/hosts"
    if [[ -n "$REMOTE_IP" && -n "$REMOTE_FQDN" ]]; then
        echo "  echo '$REMOTE_IP $REMOTE_FQDN' | sudo tee -a /etc/hosts"
    fi
    echo ""
    log_info "After adding, re-run this script."

    if sudo -n true 2>/dev/null; then
        if confirm_or_skip "Attempt to add /etc/hosts entries now (needs sudo)?"; then
            echo "$LOCAL_IP $SERVER_CN" | sudo tee -a /etc/hosts >/dev/null
            if [[ -n "$REMOTE_IP" && -n "$REMOTE_FQDN" ]]; then
                echo "$REMOTE_IP $REMOTE_FQDN" | sudo tee -a /etc/hosts >/dev/null
            fi
            HOSTS_CHANGED=true
            log_success "Added /etc/hosts entries"
        fi
    else
        log_warn "No passwordless sudo available; skipping /etc/hosts modification"
        log_info "Please add the entries manually and re-run."
    fi
else
    log_success "Local FQDN entry found: $LOCAL_IP -> $SERVER_CN"
fi

# Verify remote entry too
if [[ -n "$REMOTE_IP" && -n "$REMOTE_FQDN" ]]; then
    if grep -q "$REMOTE_IP.*$REMOTE_FQDN" /etc/hosts 2>/dev/null; then
        log_success "Remote FQDN entry found: $REMOTE_IP -> $REMOTE_FQDN"
    else
        log_warn "Missing remote entry: $REMOTE_IP $REMOTE_FQDN"
        if sudo -n true 2>/dev/null; then
            if $HOSTS_CHANGED || confirm_or_skip "Add remote /etc/hosts entry (needs sudo)?"; then
                echo "$REMOTE_IP $REMOTE_FQDN" | sudo tee -a /etc/hosts >/dev/null
                log_success "Added remote /etc/hosts entry"
            fi
        else
            log_warn "No passwordless sudo; please add '$REMOTE_IP $REMOTE_FQDN' to /etc/hosts manually"
        fi
    fi
fi

# Verify FQDN resolves
if ping -c 1 -W 2 "$SERVER_CN" &>/dev/null; then
    log_success "FQDN $SERVER_CN resolves and is reachable"
else
    log_warn "FQDN $SERVER_CN does not resolve or is unreachable (may be OK if DNS not configured)"
fi

# ----------------------------------------------------------------------------
# Step 2: Create directory structure
# ----------------------------------------------------------------------------

log_section "Step 2: Creating Directory Structure"

mkdir -p "$WORK_DIR"/{ca,server,user}
mkdir -p "$CERTS_DIR"
mkdir -p "$GLOBUS_DIR"

# Ensure correct directory permissions
chmod 755 "$GLOBUS_DIR"
chmod 755 "$CERTS_DIR"

log_success "Created $WORK_DIR"
log_success "Created $GLOBUS_DIR (mode 755)"
log_success "Created $CERTS_DIR (mode 755)"

# ----------------------------------------------------------------------------
# Step 3: Generate CA (Certificate Authority)
# ----------------------------------------------------------------------------

log_section "Step 3: Creating Self-Signed CA"

CA_KEY="$WORK_DIR/ca/ca.key"
CA_CERT="$WORK_DIR/ca/ca.pem"

if [[ -f "$CA_CERT" ]]; then
    log_warn "CA already exists at $CA_CERT"
    if confirm_or_skip "Regenerate CA? This will invalidate all existing certificates!"; then
        rm -f "$CA_KEY" "$CA_CERT"
    else
        log_info "Keeping existing CA"
    fi
fi

if [[ ! -f "$CA_CERT" ]]; then
    log_info "Generating CA private key (4096-bit, password-less)..."
    openssl genrsa -out "$CA_KEY" 4096 2>/dev/null
    chmod 400 "$CA_KEY"

    log_info "Generating CA certificate (valid $CA_DAYS days)..."
    openssl req -new -x509 \
        -key "$CA_KEY" \
        -out "$CA_CERT" \
        -days "$CA_DAYS" \
        -subj "$CA_SUBJECT" \
        -sha256

    chmod 644 "$CA_CERT"
    log_success "CA created: $CA_CERT"
fi

# Get CA hash for trust store
CA_HASH=$(openssl x509 -hash -noout -in "$CA_CERT")
log_info "CA Hash: $CA_HASH"

# ----------------------------------------------------------------------------
# Step 4: Generate Host Certificate (for GridFTP server on this node)
# ----------------------------------------------------------------------------

log_section "Step 4: Creating Host Certificate (CN=host/$SERVER_CN)"

SERVER_KEY="$WORK_DIR/server/hostkey.pem"
SERVER_CSR="$WORK_DIR/server/host.csr"
SERVER_CERT="$WORK_DIR/server/hostcert.pem"

log_info "Generating host private key (2048-bit, password-less)..."
openssl genrsa -out "$SERVER_KEY" 2048 2>/dev/null

log_info "Generating host CSR..."
openssl req -new \
    -key "$SERVER_KEY" \
    -out "$SERVER_CSR" \
    -subj "$SERVER_SUBJECT"

log_info "Signing host certificate with CA..."
openssl x509 -req \
    -in "$SERVER_CSR" \
    -CA "$CA_CERT" \
    -CAkey "$CA_KEY" \
    -CAcreateserial \
    -out "$SERVER_CERT" \
    -days "$CERT_DAYS" \
    -sha256 2>/dev/null

# GSI-critical permissions
chmod 400 "$SERVER_KEY"
chmod 644 "$SERVER_CERT"

log_success "Host cert: $SERVER_CERT (mode 644)"
log_success "Host key:  $SERVER_KEY (mode 400)"

# ----------------------------------------------------------------------------
# Step 5: Generate User Certificate
# ----------------------------------------------------------------------------

log_section "Step 5: Creating User Certificate (CN=$USER_CN)"

USER_KEY="$WORK_DIR/user/userkey.pem"
USER_CSR="$WORK_DIR/user/user.csr"
USER_CERT="$WORK_DIR/user/usercert.pem"

log_info "Generating user private key (2048-bit, password-less)..."
openssl genrsa -out "$USER_KEY" 2048 2>/dev/null

log_info "Generating user CSR..."
openssl req -new \
    -key "$USER_KEY" \
    -out "$USER_CSR" \
    -subj "$USER_SUBJECT"

log_info "Signing user certificate with CA..."
openssl x509 -req \
    -in "$USER_CSR" \
    -CA "$CA_CERT" \
    -CAkey "$CA_KEY" \
    -CAcreateserial \
    -out "$USER_CERT" \
    -days "$CERT_DAYS" \
    -sha256 2>/dev/null

# GSI-critical permissions
chmod 400 "$USER_KEY"
chmod 644 "$USER_CERT"

log_success "User cert: $USER_CERT (mode 644)"
log_success "User key:  $USER_KEY (mode 400)"

# ----------------------------------------------------------------------------
# Step 6: Install to ~/.globus
# ----------------------------------------------------------------------------

log_section "Step 6: Installing to ~/.globus"

# User certificate and key
cp "$USER_CERT" "$GLOBUS_DIR/usercert.pem"
chmod 644 "$GLOBUS_DIR/usercert.pem"
log_success "Installed $GLOBUS_DIR/usercert.pem (mode 644)"

cp "$USER_KEY" "$GLOBUS_DIR/userkey.pem"
chmod 400 "$GLOBUS_DIR/userkey.pem"
log_success "Installed $GLOBUS_DIR/userkey.pem (mode 400)"

# CA certificate with hash name (trust store)
cp "$CA_CERT" "$CERTS_DIR/${CA_HASH}.0"
chmod 644 "$CERTS_DIR/${CA_HASH}.0"
log_success "Installed CA trust: $CERTS_DIR/${CA_HASH}.0"

# ----------------------------------------------------------------------------
# Step 7: Create signing_policy file
# ----------------------------------------------------------------------------

log_section "Step 7: Creating Signing Policy"

SIGNING_POLICY="$CERTS_DIR/${CA_HASH}.signing_policy"

cat > "$SIGNING_POLICY" << POLICY_EOF
# HYPER-TRANSFER CA Signing Policy
# Generated: $(date -Iseconds)
access_id_CA    X509    '$CA_SUBJECT'
pos_rights      globus  CA:sign
cond_subjects   globus  '"/C=CN/O=Grid/*"'
POLICY_EOF

chmod 644 "$SIGNING_POLICY"
log_success "Created $SIGNING_POLICY"

# ----------------------------------------------------------------------------
# Step 8: Install host certificate for GridFTP server
# ----------------------------------------------------------------------------

log_section "Step 8: Installing Host Certificate for GridFTP"

# For user-level GridFTP (no root required), we use ~/.globus
# The gridftp server will look for hostcert/hostkey in specific locations.
# We set environment variables to point to our certs.

GRIDFTP_CERT_DIR="$HOME/.globus/gridftp"
mkdir -p "$GRIDFTP_CERT_DIR"

cp "$SERVER_CERT" "$GRIDFTP_CERT_DIR/hostcert.pem"
chmod 644 "$GRIDFTP_CERT_DIR/hostcert.pem"

cp "$SERVER_KEY" "$GRIDFTP_CERT_DIR/hostkey.pem"
chmod 400 "$GRIDFTP_CERT_DIR/hostkey.pem"

log_success "Host cert installed: $GRIDFTP_CERT_DIR/hostcert.pem (644)"
log_success "Host key installed:  $GRIDFTP_CERT_DIR/hostkey.pem (400)"

# Also try system-level if we have sudo
if [[ -w /etc/grid-security/ ]] || sudo -n true 2>/dev/null; then
    log_info "Installing system-level host certificates..."
    sudo mkdir -p /etc/grid-security
    sudo cp "$SERVER_CERT" /etc/grid-security/hostcert.pem
    sudo chmod 644 /etc/grid-security/hostcert.pem
    sudo cp "$SERVER_KEY" /etc/grid-security/hostkey.pem
    sudo chmod 400 /etc/grid-security/hostkey.pem
    log_success "System host certs installed in /etc/grid-security/"
else
    log_warn "No sudo access; skipping /etc/grid-security/ install"
    log_info "GridFTP will use user-level certs via environment variables"
fi

# ----------------------------------------------------------------------------
# Step 9: Create grid-mapfile (authorization)
# ----------------------------------------------------------------------------

log_section "Step 9: Creating grid-mapfile"

GRIDMAP_FILE="$HOME/.gridmap"

# Map our user certificate DN to the local username
USER_DN=$(openssl x509 -in "$USER_CERT" -noout -subject -nameopt rfc2253 | sed 's/^subject=//')
# Also get the traditional slash-separated DN for grid-mapfile
USER_DN_SLASH=$(openssl x509 -in "$USER_CERT" -noout -subject | sed 's/^subject=//' | sed 's/^ *//')

cat > "$GRIDMAP_FILE" << GRIDMAP_EOF
"$USER_DN_SLASH" $USER_CN
GRIDMAP_EOF

chmod 644 "$GRIDMAP_FILE"
log_success "Created $GRIDMAP_FILE"
log_info "  Mapped: \"$USER_DN_SLASH\" -> $USER_CN"

# ----------------------------------------------------------------------------
# Step 10: Verify installation
# ----------------------------------------------------------------------------

log_section "Step 10: Verifying Installation"

log_info "Verifying CA certificate..."
openssl x509 -in "$CA_CERT" -noout -subject -issuer -dates
log_success "CA certificate valid"

log_info "Verifying host certificate chain..."
openssl verify -CAfile "$CA_CERT" "$SERVER_CERT"
log_success "Host certificate verified against CA"

log_info "Verifying user certificate chain..."
openssl verify -CAfile "$CA_CERT" "$USER_CERT"
log_success "User certificate verified against CA"

log_info "Checking critical file permissions..."
echo "  $(ls -la "$GLOBUS_DIR/usercert.pem" | awk '{print $1, $NF}')"
echo "  $(ls -la "$GLOBUS_DIR/userkey.pem" | awk '{print $1, $NF}')"
echo "  $(ls -la "$GRIDFTP_CERT_DIR/hostcert.pem" | awk '{print $1, $NF}')"
echo "  $(ls -la "$GRIDFTP_CERT_DIR/hostkey.pem" | awk '{print $1, $NF}')"

# Verify key permissions are strict enough
USERKEY_PERM=$(stat -c %a "$GLOBUS_DIR/userkey.pem")
HOSTKEY_PERM=$(stat -c %a "$GRIDFTP_CERT_DIR/hostkey.pem")

if [[ "$USERKEY_PERM" -le 400 && "$HOSTKEY_PERM" -le 400 ]]; then
    log_success "All key permissions are correctly locked (<=400)"
else
    log_error "Key permissions too loose! userkey=$USERKEY_PERM hostkey=$HOSTKEY_PERM"
    log_error "GSI will reject keys with permissions > 400"
    exit 1
fi

# ----------------------------------------------------------------------------
# Step 11: Test grid-proxy-init
# ----------------------------------------------------------------------------

log_section "Step 11: Testing grid-proxy-init"

if command -v grid-proxy-init &>/dev/null; then
    log_info "Running grid-proxy-init (password-less key, should not prompt)..."

    # Set environment for proxy init
    export X509_USER_CERT="$GLOBUS_DIR/usercert.pem"
    export X509_USER_KEY="$GLOBUS_DIR/userkey.pem"
    export X509_CERT_DIR="$CERTS_DIR"

    if grid-proxy-init -verify -debug 2>&1 | tail -5; then
        log_success "grid-proxy-init succeeded!"

        log_info "Proxy info:"
        grid-proxy-info 2>&1 | while read -r line; do
            echo "  $line"
        done
    else
        log_error "grid-proxy-init failed!"
        log_error "Check certificate chain and permissions."
        log_info "Debug: grid-proxy-init -verify -debug"
    fi
else
    log_warn "grid-proxy-init not found. Install: apt install globus-proxy-utils"
fi

# ----------------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------------

log_section "Setup Complete!"

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║                    FILES CREATED                           ║"
echo "╠══════════════════════════════════════════════════════════════╣"
echo "║ CA Certificate:     $WORK_DIR/ca/ca.pem"
echo "║ CA Private Key:     $WORK_DIR/ca/ca.key"
echo "║ Host Certificate:   $WORK_DIR/server/hostcert.pem"
echo "║ Host Private Key:   $WORK_DIR/server/hostkey.pem"
echo "║ User Certificate:   $WORK_DIR/user/usercert.pem"
echo "║ User Private Key:   $WORK_DIR/user/userkey.pem"
echo "║ Grid Mapfile:       $GRIDMAP_FILE"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║              ENVIRONMENT VARIABLES TO EXPORT               ║"
echo "╠══════════════════════════════════════════════════════════════╣"
echo "║ Add these to your ~/.bashrc or run before GridFTP:         ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "  export X509_USER_CERT=$GLOBUS_DIR/usercert.pem"
echo "  export X509_USER_KEY=$GLOBUS_DIR/userkey.pem"
echo "  export X509_CERT_DIR=$CERTS_DIR"
echo "  export GRIDMAP=$GRIDMAP_FILE"
echo "  export GLOBUS_HOSTNAME=$SERVER_CN"
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║              NEXT STEPS                                    ║"
echo "╠══════════════════════════════════════════════════════════════╣"
echo "║ 1. Run distribute_certs.sh to push CA to peer node        ║"
echo "║ 2. Run setup_gsi.sh on the peer node                      ║"
echo "║ 3. Start GridFTP server in debug mode:                     ║"
echo "║    globus-gridftp-server -p 2811 -debug -log-level all \\  ║"
echo "║      -cert \$HOME/.globus/gridftp/hostcert.pem \\           ║"
echo "║      -key  \$HOME/.globus/gridftp/hostkey.pem              ║"
echo "║ 4. Test with globus-url-copy                               ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

log_success "GSI environment setup complete for $SERVER_CN!"
