#!/bin/bash
#
# HYPER-TRANSFER Certificate Distribution Script
#
# Pushes CA trust chain and host certificates between dual nodes
# with strict GSI permission enforcement.
#
# Usage:
#   ./distribute_certs.sh [remote_user@remote_host]
#
# Default: sumu@192.168.1.203 (Node B)
#

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

NODE_A_IP="192.168.1.114"
NODE_B_IP="192.168.1.203"

WORK_DIR="$HOME/my_grid_security"
GLOBUS_DIR="$HOME/.globus"
CERTS_DIR="$GLOBUS_DIR/certificates"
CA_CERT="$WORK_DIR/ca/ca.pem"

# Detect local node
LOCAL_IP=$(ip -4 addr show scope global | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | head -1)

if [[ "$LOCAL_IP" == "$NODE_A_IP" ]]; then
    DEFAULT_REMOTE="sumu@${NODE_B_IP}"
elif [[ "$LOCAL_IP" == "$NODE_B_IP" ]]; then
    DEFAULT_REMOTE="sumu@${NODE_A_IP}"
else
    DEFAULT_REMOTE="${1:-}"
fi

REMOTE="${1:-$DEFAULT_REMOTE}"

if [[ -z "$REMOTE" ]]; then
    echo "[ERROR] Cannot determine remote node. Usage: $0 user@host"
    exit 1
fi

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
log_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[ OK ]${NC} $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"; }

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║     HYPER-TRANSFER Certificate Distribution                ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo "  Local IP:  $LOCAL_IP"
echo "  Remote:    $REMOTE"
echo ""

# ============================================================================
# Step 1: Verify local CA exists
# ============================================================================

if [[ ! -f "$CA_CERT" ]]; then
    log_error "CA certificate not found: $CA_CERT"
    log_error "Run setup_gsi.sh first on this node."
    exit 1
fi

CA_HASH=$(openssl x509 -hash -noout -in "$CA_CERT")
log_info "CA Hash: $CA_HASH"

# ============================================================================
# Step 2: Test SSH connectivity
# ============================================================================

log_info "Testing SSH connectivity to $REMOTE ..."
if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "$REMOTE" "echo ok" &>/dev/null; then
    log_error "Cannot SSH to $REMOTE (BatchMode). Set up passwordless SSH first:"
    echo "  ssh-copy-id $REMOTE"
    exit 1
fi
log_success "SSH connection OK"

# ============================================================================
# Step 3: Create remote directory structure
# ============================================================================

log_info "Creating remote directory structure..."
ssh "$REMOTE" "mkdir -p ~/.globus/certificates ~/.globus/gridftp && chmod 755 ~/.globus ~/.globus/certificates"
log_success "Remote directories created"

# ============================================================================
# Step 4: Push CA trust chain
# ============================================================================

log_info "Pushing CA certificate to remote trust store..."
scp -q "$CERTS_DIR/${CA_HASH}.0" "$REMOTE:~/.globus/certificates/${CA_HASH}.0"
scp -q "$CERTS_DIR/${CA_HASH}.signing_policy" "$REMOTE:~/.globus/certificates/${CA_HASH}.signing_policy"
log_success "CA trust chain pushed"

# ============================================================================
# Step 5: Push grid-mapfile
# ============================================================================

if [[ -f "$HOME/.gridmap" ]]; then
    log_info "Pushing grid-mapfile..."
    scp -q "$HOME/.gridmap" "$REMOTE:~/.gridmap"
    log_success "grid-mapfile pushed"
fi

# ============================================================================
# Step 6: Lock permissions on remote
# ============================================================================

log_info "Enforcing strict permissions on remote node..."
ssh "$REMOTE" bash -s << 'REMOTE_PERMS'
set -e
# CA trust store
chmod 644 ~/.globus/certificates/*.0 2>/dev/null || true
chmod 644 ~/.globus/certificates/*.signing_policy 2>/dev/null || true

# User certs (if they exist from remote's own setup_gsi.sh)
[ -f ~/.globus/usercert.pem ] && chmod 644 ~/.globus/usercert.pem
[ -f ~/.globus/userkey.pem ]  && chmod 400 ~/.globus/userkey.pem

# Host certs (if they exist)
[ -f ~/.globus/gridftp/hostcert.pem ] && chmod 644 ~/.globus/gridftp/hostcert.pem
[ -f ~/.globus/gridftp/hostkey.pem ]  && chmod 400 ~/.globus/gridftp/hostkey.pem

# grid-mapfile
[ -f ~/.gridmap ] && chmod 644 ~/.gridmap

echo "Permissions locked."
REMOTE_PERMS
log_success "Remote permissions enforced"

# ============================================================================
# Step 7: Lock local permissions (double-check)
# ============================================================================

log_info "Verifying local permissions..."
chmod 644 "$GLOBUS_DIR/usercert.pem"
chmod 400 "$GLOBUS_DIR/userkey.pem"
chmod 644 "$CERTS_DIR/${CA_HASH}.0"
chmod 644 "$CERTS_DIR/${CA_HASH}.signing_policy"
[ -f "$HOME/.globus/gridftp/hostcert.pem" ] && chmod 644 "$HOME/.globus/gridftp/hostcert.pem"
[ -f "$HOME/.globus/gridftp/hostkey.pem" ]  && chmod 400 "$HOME/.globus/gridftp/hostkey.pem"
log_success "Local permissions verified"

# ============================================================================
# Step 8: Verify remote can read CA
# ============================================================================

log_info "Verifying remote CA trust..."
REMOTE_HASH=$(ssh "$REMOTE" "openssl x509 -hash -noout -in ~/.globus/certificates/${CA_HASH}.0 2>/dev/null" || echo "FAIL")
if [[ "$REMOTE_HASH" == "$CA_HASH" ]]; then
    log_success "Remote CA trust verified (hash=$CA_HASH)"
else
    log_error "Remote CA verification failed! Expected $CA_HASH, got $REMOTE_HASH"
    exit 1
fi

echo ""
log_success "Certificate distribution complete!"
echo ""
echo "  Next: Run setup_gsi.sh on the remote node ($REMOTE) to generate"
echo "  its own host and user certificates, then run this script from"
echo "  the remote node back to this one."
echo ""
