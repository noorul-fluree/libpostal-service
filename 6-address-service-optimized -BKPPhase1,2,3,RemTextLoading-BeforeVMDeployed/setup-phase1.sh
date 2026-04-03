#!/usr/bin/env bash
# ==============================================================================
#  Address Normalization Service — Phase 1 Setup Script
#  Target: Ubuntu 22.04 LTS, 4 Core, 16 GB RAM
# ==============================================================================
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${GREEN}[SETUP]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }
step() { echo -e "\n${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"; echo -e "${CYAN}  Step $1: $2${NC}"; echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"; }

INSTALL_DIR="/opt/address-service"
LIBPOSTAL_DIR="/opt/libpostal"
DATA_DIR="${LIBPOSTAL_DIR}/data"
CONFIG_DIR="/etc/address-service"
LOG_DIR="/var/log/address-service"
SERVICE_USER="addrservice"

# ==============================================================================
step "1/8" "Installing system dependencies"
# ==============================================================================
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake git curl wget \
    autoconf automake libtool pkg-config \
    libssl-dev libjsoncpp-dev uuid-dev zlib1g-dev \
    libcurl4-openssl-dev \
    nginx \
    prometheus-node-exporter

log "System dependencies installed"

# ==============================================================================
step "2/8" "Creating service user and directories"
# ==============================================================================
if ! id -u "${SERVICE_USER}" &>/dev/null; then
    sudo useradd -r -s /bin/false -m -d /opt/address-service "${SERVICE_USER}"
    log "Created user: ${SERVICE_USER}"
else
    log "User ${SERVICE_USER} already exists"
fi

sudo mkdir -p "${INSTALL_DIR}/bin" "${INSTALL_DIR}/logs"
sudo mkdir -p "${LIBPOSTAL_DIR}" "${DATA_DIR}"
sudo mkdir -p "${CONFIG_DIR}"
sudo mkdir -p "${LOG_DIR}"

sudo chown -R "${SERVICE_USER}:${SERVICE_USER}" "${INSTALL_DIR}" "${LOG_DIR}"

# ==============================================================================
step "3/8" "Building libpostal from source"
# ==============================================================================
if [ ! -f /usr/local/lib/libpostal.so ]; then
    cd /tmp
    if [ ! -d libpostal ]; then
        git clone https://github.com/openvenues/libpostal
    fi
    cd libpostal
    ./bootstrap.sh
    ./configure --datadir="${DATA_DIR}" MODEL=senzing
    make -j$(nproc)
    sudo make install
    sudo ldconfig
    log "libpostal compiled and installed (Senzing v1.2 model)"
else
    log "libpostal already installed"
fi

# ==============================================================================
step "4/8" "Downloading Senzing v1.2 libpostal data (~2.2 GB)"
# ==============================================================================
if [ ! -f "${DATA_DIR}/address_expansions.dat" ]; then
    log "Downloading Senzing v1.2 data files..."
    cd /tmp
    curl -L -o language_classifier.tar.gz \
        https://public-read-libpostal-data.s3.amazonaws.com/v1.1.0/language_classifier.tar.gz
    curl -L -o libpostal_data.tar.gz \
        https://public-read-libpostal-data.s3.amazonaws.com/v1.1.0/libpostal_data.tar.gz
    curl -L -o parser.tar.gz \
        https://public-read-libpostal-data.s3.amazonaws.com/v1.2.0/parser.tar.gz

    sudo tar -xzf language_classifier.tar.gz -C "${DATA_DIR}"
    sudo tar -xzf libpostal_data.tar.gz -C "${DATA_DIR}"
    sudo tar -xzf parser.tar.gz -C "${DATA_DIR}"
    rm -f language_classifier.tar.gz libpostal_data.tar.gz parser.tar.gz
    sudo chown -R "${SERVICE_USER}:${SERVICE_USER}" "${DATA_DIR}"
    log "libpostal data downloaded to ${DATA_DIR}"
else
    log "libpostal data already present"
fi

# ==============================================================================
step "5/8" "Building Drogon framework"
# ==============================================================================
if [ ! -f /usr/local/lib/libdrogon.a ]; then
    cd /tmp
    if [ ! -d drogon ]; then
        git clone --depth 1 --branch v1.9.3 https://github.com/drogonframework/drogon
    fi
    cd drogon
    git submodule update --init
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
    make -j$(nproc)
    sudo make install
    sudo ldconfig
    log "Drogon framework installed"
else
    log "Drogon framework already installed"
fi

# ==============================================================================
step "6/8" "Building address-service"
# ==============================================================================
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}"

mkdir -p build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LLM=OFF \
    -DENABLE_TESTS=ON
make -j$(nproc)

# Run tests
log "Running tests..."
if ./address-service-tests; then
    log "All tests passed"
else
    warn "Some tests failed — check output above"
fi

# Install binary
sudo cp address-service "${INSTALL_DIR}/bin/"
sudo chown "${SERVICE_USER}:${SERVICE_USER}" "${INSTALL_DIR}/bin/address-service"
log "Binary installed to ${INSTALL_DIR}/bin/"

# ==============================================================================
step "7/8" "Installing configuration and systemd service"
# ==============================================================================
# Config file
sudo cp "${SCRIPT_DIR}/config/config.json" "${CONFIG_DIR}/config.json"

# Update config with correct data directory
sudo sed -i "s|/usr/share/libpostal|${DATA_DIR}|g" "${CONFIG_DIR}/config.json"

# systemd service
sudo cp "${SCRIPT_DIR}/config/address-service.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable address-service
log "systemd service installed and enabled"

# Nginx config
sudo cp "${SCRIPT_DIR}/config/nginx.conf" /etc/nginx/nginx.conf
sudo nginx -t && sudo systemctl restart nginx
log "Nginx configured and restarted"

# ==============================================================================
step "8/8" "Starting the service"
# ==============================================================================
sudo systemctl start address-service

# Wait for startup (libpostal takes ~15-30 seconds to load)
log "Waiting for service to initialize (libpostal data loading)..."
for i in $(seq 1 60); do
    if curl -sf http://localhost:8080/health/startup > /dev/null 2>&1; then
        log "Service is UP and READY"
        break
    fi
    if [ "$i" -eq 60 ]; then
        error "Service failed to start within 60 seconds"
    fi
    sleep 1
    echo -n "."
done

echo ""

# ==============================================================================
echo -e "\n${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  Setup Complete!${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo -e "  Service URL:     ${CYAN}http://localhost:8080${NC}"
echo -e "  Nginx proxy:     ${CYAN}http://localhost:80${NC}"
echo -e "  Metrics:         ${CYAN}http://localhost:8080/metrics${NC}"
echo -e "  Health:          ${CYAN}http://localhost:8080/health/info${NC}"
echo ""
echo -e "  Service status:  ${YELLOW}sudo systemctl status address-service${NC}"
echo -e "  View logs:       ${YELLOW}sudo journalctl -u address-service -f${NC}"
echo -e "  Restart:         ${YELLOW}sudo systemctl restart address-service${NC}"
echo ""
echo -e "  Test parse:"
echo -e "  ${YELLOW}curl -X POST http://localhost:8080/api/v1/parse \\${NC}"
echo -e "  ${YELLOW}  -H 'Content-Type: application/json' \\${NC}"
echo -e "  ${YELLOW}  -d '{\"address\": \"123 MG Road, Bengaluru, Karnataka 560001\"}'${NC}"
echo ""
