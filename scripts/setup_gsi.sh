#!/bin/bash
#
# HYPER-TRANSFER GSI Environment Setup Script
#
# This script sets up a complete Grid Security Infrastructure (GSI) environment
# for GridFTP authentication using self-signed certificates.
#
# Features:
# - Creates a self-signed CA (Certificate Authority)
# - Generates server certificate for GridFTP server
# - Generates user certificate for grid-proxy-init
# - All keys are password-less for automation
# - Configures ~/.globus for immediate use
#
# Usage: ./setup_gsi.sh
#

set -e  # Exit on any error

# ============================================================================
# Configuration
# ============================================================================

WORK_DIR="$HOME/my_grid_security"
GLOBUS_DIR="$HOME/.globus"
CERTS_DIR="$GLOBUS_DIR/certificates"

# Certificate subjects
CA_SUBJECT="/C=CN/O=Grid/OU=HyperTransfer/CN=HyperCA"
SERVER_CN="192.168.1.203"
SERVER_SUBJECT="/C=CN/O=Grid/OU=HyperTransfer/CN=$SERVER_CN"
USER_CN="sumu"
USER_SUBJECT="/C=CN/O=Grid/OU=HyperTransfer/CN=$USER_CN"

# Validity period (days)
CA_DAYS=3650      # 10 years
CERT_DAYS=365     # 1 year

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================================
# Helper Functions
# ============================================================================

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_section() {
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN} $1${NC}"
    echo -e "${GREEN}========================================${NC}"
}

# ============================================================================
# Main Script
# ============================================================================

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║       HYPER-TRANSFER GSI Environment Setup                     ║"
echo "║       GridFTP Certificate Authority & Certificates             ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# ----------------------------------------------------------------------------
# Step 1: Create directory structure
# ----------------------------------------------------------------------------

log_section "Step 1: Creating Directory Structure"

mkdir -p "$WORK_DIR"/{ca,server,user}
mkdir -p "$CERTS_DIR"
mkdir -p "$GLOBUS_DIR"

log_success "Created $WORK_DIR"
log_success "Created $GLOBUS_DIR"
log_success "Created $CERTS_DIR"

# ----------------------------------------------------------------------------
# Step 2: Generate CA (Certificate Authority)
# ----------------------------------------------------------------------------

log_section "Step 2: Creating Self-Signed CA"

CA_KEY="$WORK_DIR/ca/ca.key"
CA_CERT="$WORK_DIR/ca/ca.pem"

if [[ -f "$CA_CERT" ]]; then
    log_warn "CA already exists at $CA_CERT"
    read -p "Regenerate CA? This will invalidate all existing certificates! (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        log_info "Keeping existing CA"
    else
        rm -f "$CA_KEY" "$CA_CERT"
    fi
fi

if [[ ! -f "$CA_CERT" ]]; then
    log_info "Generating CA private key (password-less)..."
    openssl genrsa -out "$CA_KEY" 4096 2>/dev/null
    
    log_info "Generating CA certificate..."
    openssl req -new -x509 \
        -key "$CA_KEY" \
        -out "$CA_CERT" \
        -days "$CA_DAYS" \
        -subj "$CA_SUBJECT" \
        -sha256
    
    log_success "CA created: $CA_CERT"
fi

# Get CA hash for later use
CA_HASH=$(openssl x509 -hash -noout -in "$CA_CERT")
log_info "CA Hash: $CA_HASH"

# ----------------------------------------------------------------------------
# Step 3: Generate Server Certificate
# ----------------------------------------------------------------------------

log_section "Step 3: Creating Server Certificate (CN=$SERVER_CN)"

SERVER_KEY="$WORK_DIR/server/server.key"
SERVER_CSR="$WORK_DIR/server/server.csr"
SERVER_CERT="$WORK_DIR/server/server.pem"

log_info "Generating server private key (password-less)..."
openssl genrsa -out "$SERVER_KEY" 2048 2>/dev/null

log_info "Generating server CSR..."
openssl req -new \
    -key "$SERVER_KEY" \
    -out "$SERVER_CSR" \
    -subj "$SERVER_SUBJECT"

log_info "Signing server certificate with CA..."
openssl x509 -req \
    -in "$SERVER_CSR" \
    -CA "$CA_CERT" \
    -CAkey "$CA_KEY" \
    -CAcreateserial \
    -out "$SERVER_CERT" \
    -days "$CERT_DAYS" \
    -sha256 2>/dev/null

log_success "Server certificate created: $SERVER_CERT"

# ----------------------------------------------------------------------------
# Step 4: Generate User Certificate
# ----------------------------------------------------------------------------

log_section "Step 4: Creating User Certificate (CN=$USER_CN)"

USER_KEY="$WORK_DIR/user/userkey.pem"
USER_CSR="$WORK_DIR/user/user.csr"
USER_CERT="$WORK_DIR/user/usercert.pem"

log_info "Generating user private key (password-less)..."
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

log_success "User certificate created: $USER_CERT"

# ----------------------------------------------------------------------------
# Step 5: Install to ~/.globus
# ----------------------------------------------------------------------------

log_section "Step 5: Installing to ~/.globus"

# Install user certificate and key
log_info "Installing user certificate..."
cp "$USER_CERT" "$GLOBUS_DIR/usercert.pem"
chmod 644 "$GLOBUS_DIR/usercert.pem"
log_success "Installed $GLOBUS_DIR/usercert.pem (mode 644)"

log_info "Installing user private key..."
cp "$USER_KEY" "$GLOBUS_DIR/userkey.pem"
chmod 600 "$GLOBUS_DIR/userkey.pem"
log_success "Installed $GLOBUS_DIR/userkey.pem (mode 600)"

# Install CA certificate with hash name
log_info "Installing CA certificate to trust store..."
cp "$CA_CERT" "$CERTS_DIR/${CA_HASH}.0"
chmod 644 "$CERTS_DIR/${CA_HASH}.0"
log_success "Installed $CERTS_DIR/${CA_HASH}.0"

# ----------------------------------------------------------------------------
# Step 6: Create signing_policy file
# ----------------------------------------------------------------------------

log_section "Step 6: Creating Signing Policy"

SIGNING_POLICY="$CERTS_DIR/${CA_HASH}.signing_policy"

cat > "$SIGNING_POLICY" << EOF
# HYPER-TRANSFER CA Signing Policy
# Generated: $(date -Iseconds)
#
# This policy allows the HyperCA to sign certificates for the Grid organization.

access_id_CA    X509    '$CA_SUBJECT'
pos_rights      globus  CA:sign
cond_subjects   globus  '"/C=CN/O=Grid/*"'
EOF

chmod 644 "$SIGNING_POLICY"
log_success "Created $SIGNING_POLICY"

# ----------------------------------------------------------------------------
# Step 7: Verify installation
# ----------------------------------------------------------------------------

log_section "Step 7: Verifying Installation"

log_info "Verifying CA certificate..."
openssl x509 -in "$CA_CERT" -noout -subject -issuer
log_success "CA certificate valid"

log_info "Verifying server certificate chain..."
openssl verify -CAfile "$CA_CERT" "$SERVER_CERT"
log_success "Server certificate verified"

log_info "Verifying user certificate chain..."
openssl verify -CAfile "$CA_CERT" "$USER_CERT"
log_success "User certificate verified"

log_info "Checking key permissions..."
ls -la "$GLOBUS_DIR/usercert.pem" "$GLOBUS_DIR/userkey.pem"

# ----------------------------------------------------------------------------
# Summary and Instructions
# ----------------------------------------------------------------------------

log_section "Setup Complete!"

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                    FILES CREATED                               ║"
echo "╠════════════════════════════════════════════════════════════════╣"
echo "║ CA Certificate:     $WORK_DIR/ca/ca.pem"
echo "║ CA Private Key:     $WORK_DIR/ca/ca.key"
echo "║ Server Certificate: $WORK_DIR/server/server.pem"
echo "║ Server Private Key: $WORK_DIR/server/server.key"
echo "║ User Certificate:   $WORK_DIR/user/usercert.pem"
echo "║ User Private Key:   $WORK_DIR/user/userkey.pem"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║              LOCAL INSTALLATION (~/.globus)                    ║"
echo "╠════════════════════════════════════════════════════════════════╣"
echo "║ User Cert:  $GLOBUS_DIR/usercert.pem"
echo "║ User Key:   $GLOBUS_DIR/userkey.pem"
echo "║ CA Cert:    $CERTS_DIR/${CA_HASH}.0"
echo "║ Policy:     $CERTS_DIR/${CA_HASH}.signing_policy"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

echo -e "${YELLOW}╔════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${YELLOW}║          FILES TO COPY TO REMOTE SERVER ($SERVER_CN)          ║${NC}"
echo -e "${YELLOW}╠════════════════════════════════════════════════════════════════╣${NC}"
echo -e "${YELLOW}║                                                                ║${NC}"
echo -e "${YELLOW}║ Run these commands to set up the remote server:               ║${NC}"
echo -e "${YELLOW}║                                                                ║${NC}"
echo -e "${YELLOW}╚════════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "# 1. Copy CA certificate to remote server's trust store:"
echo "scp $CERTS_DIR/${CA_HASH}.0 $SERVER_CN:~/.globus/certificates/"
echo "scp $CERTS_DIR/${CA_HASH}.signing_policy $SERVER_CN:~/.globus/certificates/"
echo ""
echo "# 2. Copy server certificate and key to remote server:"
echo "scp $WORK_DIR/server/server.pem $SERVER_CN:/etc/grid-security/hostcert.pem"
echo "scp $WORK_DIR/server/server.key $SERVER_CN:/etc/grid-security/hostkey.pem"
echo ""
echo "# 3. OR for user-level GridFTP (no root), copy to ~/.globus:"
echo "scp $WORK_DIR/server/server.pem $SERVER_CN:~/.globus/usercert.pem"
echo "scp $WORK_DIR/server/server.key $SERVER_CN:~/.globus/userkey.pem"
echo "scp $CERTS_DIR/${CA_HASH}.0 $SERVER_CN:~/.globus/certificates/"
echo "scp $CERTS_DIR/${CA_HASH}.signing_policy $SERVER_CN:~/.globus/certificates/"
echo ""
echo "# 4. On the remote server, create the certificates directory and set permissions:"
echo "ssh $SERVER_CN 'mkdir -p ~/.globus/certificates && chmod 755 ~/.globus/certificates'"
echo "ssh $SERVER_CN 'chmod 600 ~/.globus/userkey.pem && chmod 644 ~/.globus/usercert.pem'"
echo ""

echo -e "${GREEN}╔════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║                    TESTING INSTRUCTIONS                        ║${NC}"
echo -e "${GREEN}╠════════════════════════════════════════════════════════════════╣${NC}"
echo -e "${GREEN}║ Test grid-proxy-init (should not ask for password):            ║${NC}"
echo -e "${GREEN}║   grid-proxy-init -verify -debug                               ║${NC}"
echo -e "${GREEN}║                                                                ║${NC}"
echo -e "${GREEN}║ Check your proxy:                                              ║${NC}"
echo -e "${GREEN}║   grid-proxy-info                                              ║${NC}"
echo -e "${GREEN}║                                                                ║${NC}"
echo -e "${GREEN}║ Test GridFTP transfer:                                         ║${NC}"
echo -e "${GREEN}║   globus-url-copy -vb file:///tmp/test.txt \\                   ║${NC}"
echo -e "${GREEN}║       gsiftp://$SERVER_CN/tmp/test.txt                   ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════════════════╝${NC}"
echo ""

log_success "GSI environment setup complete!"
