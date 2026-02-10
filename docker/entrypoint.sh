#!/bin/bash
set -e

# Configuration with defaults
STACK_IP="${STACK_IP:-192.168.100.2}"
HOST_IP="${HOST_IP:-192.168.100.1}"
LOG_LEVEL="${LOG_LEVEL:-info}"

echo "=== NeuStack Docker Playground ==="
echo "  Stack IP: $STACK_IP"
echo "  Host IP:  $HOST_IP"
echo "  Log Level: $LOG_LEVEL"
echo ""

# 1. Start NeuStack in background
# We need to run it in background first to let it create the TUN device
echo "Starting neustack_demo..."
./neustack_demo --ip "$STACK_IP" --log "$LOG_LEVEL" &
NEUSTACK_PID=$!

# 2. Wait for TUN device to be created
# NeuStack creates the device upon startup
echo "Waiting for TUN device creation..."
TUN_DEV=""
for i in $(seq 1 30); do
    # Try to find a tun device (usually tun0)
    TUN_DEV=$(ip link show type tun 2>/dev/null | grep -oP 'tun\d+' | head -1)
    
    if [ -n "$TUN_DEV" ]; then
        break
    fi
    sleep 0.5
done

if [ -z "$TUN_DEV" ]; then
    echo "ERROR: TUN device not created after 15s."
    echo "Please ensure the container has NET_ADMIN capability and access to /dev/net/tun."
    kill $NEUSTACK_PID 2>/dev/null
    exit 1
fi

echo "  TUN device found: $TUN_DEV"

# 3. Configure TUN device (Host side)
# Since we are in a container, we configure the interface that NeuStack just created.
# We set the 'peer' address to NeuStack's IP, and our local address to HOST_IP.
echo "Configuring network interface..."
ip addr add "$HOST_IP" peer "$STACK_IP" dev "$TUN_DEV"
ip link set "$TUN_DEV" up

echo "  Route configured: $HOST_IP <--> $STACK_IP"
echo ""
echo "=== NeuStack is running ==="
echo "  You can now interact with the stack from inside this container."
echo "  Test Command:  curl http://$STACK_IP/"
echo "  Interactive:   docker exec -it neustack-playground bash"
echo ""

# 4. Wait for NeuStack process
# This keeps the container running
wait $NEUSTACK_PID
