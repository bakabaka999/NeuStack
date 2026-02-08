#!/usr/bin/env bash

# Usage: sudo ./scripts/setup_nat.sh --dev utun4 --ip 192.168.100.2 --iface en0
# This script sets up a NAT environment for NeuStack on macOS.

set -euo pipefail

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Default values
DEV="utun4"
STACK_IP="192.168.100.2"
IFACE="en0"

# Help message
usage() {
    echo "Usage: $0 [--dev <device>] [--ip <stack_ip>] [--iface <exit_interface>]"
    exit 1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --dev) DEV="$2"; shift 2 ;;
        --ip) STACK_IP="$2"; shift 2 ;;
        --iface) IFACE="$2"; shift 2 ;;
        *) usage ;;
    esac
done

# Root check
if [[ $EUID -ne 0 ]]; then
   echo -e "${RED}[FAIL] This script must be run as root (sudo)${NC}"
   exit 1
fi

HOST_IP="${STACK_IP%.*}.1"
PF_CONF="/tmp/neustack_pf.conf"

echo -e "--- ${GREEN}NeuStack NAT Setup${NC} ---"

# 1. Configure utun interface
echo -n "Configuring $DEV ($HOST_IP <-> $STACK_IP)... "
ifconfig "$DEV" "$HOST_IP" "$STACK_IP" up || { echo -e "${RED}FAILED${NC}"; exit 1; }
echo -e "${GREEN}OK${NC}"

# 2. Enable IP Forwarding
echo -n "Enabling IPv4 Forwarding... "
sysctl -w net.inet.ip.forwarding=1 > /dev/null
echo -e "${GREEN}OK${NC}"

# 3. Generate PF rules (main ruleset, not anchor — macOS NAT requires this)
echo -n "Generating PF rules... "
cat > "$PF_CONF" <<EOF
nat on $IFACE from $STACK_IP/32 to any -> ($IFACE)
pass all
EOF
echo -e "${GREEN}OK${NC}"

# 4. Load PF rules
echo -n "Loading PF rules... "
pfctl -f "$PF_CONF" 2>/dev/null
pfctl -e 2>/dev/null || true
echo -e "${GREEN}OK${NC}"

# 5. Route Check (Optional but recommended for point-to-point)
echo -n "Ensuring route to $STACK_IP... "
route add -host "$STACK_IP" -interface "$DEV" > /dev/null 2>&1 || true
echo -e "${GREEN}OK${NC}"

echo -e "--- ${GREEN}Setup Complete${NC} ---"
echo -e "Host IP:   ${GREEN}$HOST_IP${NC}"
echo -e "Stack IP:  ${GREEN}$STACK_IP${NC}"
echo -e "Out Interface: ${GREEN}$IFACE${NC}"
echo -e "\nTest command in NeuStack:"
echo -e "${GREEN}ping 8.8.8.8${NC} or ${GREEN}curl 44.206.113.197${NC}"