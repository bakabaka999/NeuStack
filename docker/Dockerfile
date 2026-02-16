# Multi-stage build for NeuStack Playground

# --- Stage 1: Build ---
FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    g++ \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy source code
COPY . .

# Build NeuStack (Minimal: No AI, No Tests)
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEUSTACK_ENABLE_AI=OFF \
    -DNEUSTACK_BUILD_TESTS=OFF \
    && cmake --build build -j$(nproc)

# --- Stage 2: Runtime ---
FROM ubuntu:22.04

# Install runtime tools
# Added: iputils-ping (for user testing)
RUN apt-get update && apt-get install -y \
    iproute2 \
    curl \
    iputils-ping \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy binary from builder
COPY --from=builder /build/build/examples/neustack_demo .

# Copy entrypoint script
COPY docker/entrypoint.sh .

# Set execution permissions
RUN chmod +x entrypoint.sh neustack_demo

# Define entrypoint
ENTRYPOINT ["./entrypoint.sh"]