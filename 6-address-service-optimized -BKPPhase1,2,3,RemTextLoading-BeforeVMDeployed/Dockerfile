# ==============================================================================
#  Stage 1: Build everything from source
# ==============================================================================
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git curl ca-certificates \
    autoconf automake libtool pkg-config \
    libssl-dev libjsoncpp-dev uuid-dev zlib1g-dev \
    libcurl4-openssl-dev && \
    rm -rf /var/lib/apt/lists/*

# ---- Build libpostal ----
RUN git clone https://github.com/openvenues/libpostal /tmp/libpostal && \
    cd /tmp/libpostal && \
    ./bootstrap.sh && \
    ./configure --datadir=/usr/share/libpostal MODEL=senzing && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# ---- Build Drogon framework ----
RUN git clone --depth 1 --branch v1.9.3 \
    https://github.com/drogonframework/drogon /tmp/drogon && \
    cd /tmp/drogon && \
    git submodule update --init && \
    mkdir build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DBUILD_EXAMPLES=OFF && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# ---- Build llama.cpp (optional, for LLM fallback) ----
ARG ENABLE_LLM=OFF
RUN if [ "$ENABLE_LLM" = "ON" ]; then \
    git clone --depth 1 https://github.com/ggerganov/llama.cpp /tmp/llama.cpp && \
    cd /tmp/llama.cpp && \
    mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DLLAMA_NATIVE=OFF && \
    make -j$(nproc) && \
    make install && \
    ldconfig; \
    fi

# ---- Build address-service ----
COPY . /app
WORKDIR /app

RUN mkdir build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_LLM=$ENABLE_LLM \
        -DENABLE_TESTS=OFF && \
    make -j$(nproc)

# ---- Run tests (build with tests enabled) ----
RUN mkdir build-test && cd build-test && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_LLM=OFF \
        -DENABLE_TESTS=ON && \
    make -j$(nproc) && \
    ./address-service-tests

# ==============================================================================
#  Stage 2: Slim production runtime
# ==============================================================================
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libcurl4 libjsoncpp25 libuuid1 zlib1g \
    curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# Copy built libraries
COPY --from=builder /usr/local/lib/libpostal* /usr/local/lib/
COPY --from=builder /usr/local/lib/libdrogon* /usr/local/lib/
COPY --from=builder /usr/local/lib/libtrantor* /usr/local/lib/
COPY --from=builder /usr/local/lib/pkgconfig/libpostal.pc /usr/local/lib/pkgconfig/
RUN ldconfig

# Copy binary
COPY --from=builder /app/build/address-service /usr/local/bin/address-service

# Copy default config
COPY --from=builder /app/config/config.json /etc/address-service/config.json

# Create log directory
RUN mkdir -p /var/log/address-service /opt/libpostal/data

# Non-root user
RUN groupadd -r addrservice && \
    useradd -r -g addrservice -s /bin/false addrservice && \
    chown -R addrservice:addrservice /var/log/address-service

USER addrservice

# Expose ports
EXPOSE 8080 9090

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=45s --retries=3 \
    CMD curl -sf http://localhost:8080/health/live || exit 1

ENTRYPOINT ["address-service"]
CMD ["--config", "/etc/address-service/config.json"]
