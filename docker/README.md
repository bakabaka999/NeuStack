# NeuStack Docker Playground

This directory contains a self-contained Docker environment for experiencing the NeuStack user-space TCP/IP stack without needing a complex host setup.

## Prerequisites

- **Docker** and **Docker Compose**
- **Linux Host** (Required for TUN/TAP support)
  > **Note for macOS/Windows users:** Docker Desktop typically runs inside a Linux VM. While basic containers work, exposing the host's `/dev/net/tun` or effectively bridging the TUN device can be problematic or require specific VM configurations. This playground is best experienced on a native Linux machine or a Linux Cloud VPS.

## Quick Start

Run the following command from the **project root directory**:

```bash
docker compose -f docker/docker-compose.yml up --build
```

Once the container is running and you see `=== NeuStack is running ===`, open a new terminal window and connect to the container to test:

```bash
# Enter the container
docker exec -it neustack-playground bash

# Inside the container, access the HTTP server running on the stack
curl http://192.168.100.2/
```

You should see a response from the NeuStack HTTP server.

## Configuration

You can configure the network settings via environment variables in `docker/docker-compose.yml`:

| Variable | Default | Description |
|----------|---------|-------------|
| `STACK_IP` | `192.168.100.2` | The IP address bound by NeuStack (on the TUN interface). |
| `HOST_IP` | `192.168.100.1` | The IP address assigned to the host side of the TUN interface. |
| `LOG_LEVEL` | `info` | NeuStack logging level (`trace`, `debug`, `info`, `warn`, `error`). |

## Architecture

This playground uses a **single-container** approach. The TUN device is a point-to-point interface.

```
┌───────────────────────────────────────────────┐
│ Docker Container (neustack-playground)        │
│                                               │
│  [NeuStack Process]      [Curl / Shell]       │
│         ↕                      ↕              │
│     IP: 192.168.100.2      IP: 192.168.100.1  │
│         └────── tun0 ──────────┘              │
│             (Point-to-Point)                  │
└───────────────────────────────────────────────┘
```

1. **neustack_demo** opens `/dev/net/tun` and creates `tun0`.
2. **entrypoint.sh** waits for `tun0` to appear.
3. **entrypoint.sh** configures `tun0` with the `HOST_IP` and sets the peer as `STACK_IP`.
4. Traffic routed to `STACK_IP` goes through `tun0` to the NeuStack process.

## Cleanup

To stop and remove the container:

```bash
docker compose -f docker/docker-compose.yml down
```

## Troubleshooting

### "ERROR: TUN device not created after 15s"

1. Check if `/dev/net/tun` exists on your host machine:
   ```bash
   ls -la /dev/net/tun
   ```
   If not, try loading the kernel module:
   ```bash
   sudo modprobe tun
   ```
2. Ensure the container has the `NET_ADMIN` capability (already set in `docker-compose.yml`).
