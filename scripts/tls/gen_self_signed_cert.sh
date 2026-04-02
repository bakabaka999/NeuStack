#!/bin/bash
# scripts/tls/gen_self_signed_cert.sh
#
# Generate a self-signed certificate + key for testing HTTPS.
#
# Usage:
#   bash scripts/tls/gen_self_signed_cert.sh [output_dir]
#
# Output:
#   <output_dir>/server_cert.pem   — self-signed X.509 certificate
#   <output_dir>/server_key.pem    — RSA 2048-bit private key

set -e

OUTPUT_DIR="${1:-.}"
mkdir -p "$OUTPUT_DIR"

CERT="$OUTPUT_DIR/server_cert.pem"
KEY="$OUTPUT_DIR/server_key.pem"

if [ -f "$CERT" ] && [ -f "$KEY" ]; then
    echo "Certificate already exists: $CERT"
    echo "Key already exists:         $KEY"
    echo "Delete them first to regenerate."
    exit 0
fi

echo "Generating self-signed certificate..."

openssl req -x509 -newkey rsa:2048 \
    -keyout "$KEY" \
    -out "$CERT" \
    -days 3650 \
    -nodes \
    -subj "/CN=NeuStack/O=NeuStack/C=US" \
    2>/dev/null

echo "  Certificate: $CERT"
echo "  Private key: $KEY"
echo "Done."
