#!/usr/bin/env bash

# Usage: sudo ./scripts/teardown_nat.sh
# This script cleans up the NAT environment for NeuStack.

set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

# Root check
if [[ $EUID -ne 0 ]]; then
   echo -e "${RED}[FAIL] This script must be run as root (sudo)${NC}"
   exit 1
fi

PF_CONF="/tmp/neustack_pf.conf"

echo -e "--- ${RED}NeuStack NAT Teardown${NC} ---"

# 1. Disable PF
echo -n "Disabling PF... "
pfctl -d > /dev/null 2>&1 || echo -n "(Already disabled) "
echo -e "${GREEN}OK${NC}"

# 2. Disable IP Forwarding
echo -n "Disabling IPv4 Forwarding... "
sysctl -w net.inet.ip.forwarding=0 > /dev/null
echo -e "${GREEN}OK${NC}"

# 3. Clean up files
echo -n "Removing $PF_CONF... "
rm -f "$PF_CONF"
echo -e "${GREEN}OK${NC}"

echo -e "--- ${GREEN}Teardown Finished${NC} ---"