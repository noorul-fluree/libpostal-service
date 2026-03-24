# Address Normalization Service — Complete VM Runbook

---

## Project Overview

A high-performance C++ HTTP service that parses, normalizes and enriches raw address strings using libpostal. Built on the Drogon framework.

### Libraries Used
| Library | Purpose |
|---------|---------|
| Drogon v1.9.3 | HTTP server framework (C++) |
| libpostal 1.1.4 | Address parsing + normalization |
| JsonCpp | JSON serialization |
| OpenSSL 1.1 | TLS/crypto |
| pthread | Threading |
| OpenMP | Batch parallelism (WSL build) |

### Project Structure
```
address-service-optimized/          ← WSL source
├── src/
│   ├── main.cc                     ← startup, global services
│   ├── models/AddressModels.h      ← ParsedAddress, ServiceConfig etc
│   ├── services/
│   │   ├── AddressParser.cc/h      ← libpostal wrapper (thread-safe)
│   │   ├── PreProcessor.cc/h       ← clean + expand abbreviations
│   │   ├── RuleEngine.cc/h         ← PIN/ZIP inference, misspellings
│   │   ├── ConfidenceScorer.cc/h   ← 0.0-1.0 confidence score
│   │   ├── CacheManager.cc/h       ← 64-shard LRU cache
│   │   ├── MetricsCollector.cc/h   ← Prometheus metrics
│   │   └── LLMFallback.cc/h        ← Phase 4 LLM (disabled)
│   ├── controllers/
│   │   ├── ParseController.cc/h    ← POST /api/v1/parse, /normalize
│   │   ├── BatchController.cc/h    ← POST /api/v1/batch
│   │   ├── EnrichController.cc/h   ← POST /api/v1/enrich
│   │   ├── HealthController.cc/h   ← GET /health/*
│   │   ├── MetricsController.cc/h  ← GET /metrics
│   │   ├── AuthFilter.cc/h         ← unified auth dispatcher
│   │   ├── ApiKeyFilter.cc/h       ← API key validation
│   │   └── JwtAuthFilter.cc/h      ← JWT validation
│   └── utils/InputValidator.h      ← length, UTF-8, null byte checks
├── config/config.json              ← service configuration
├── CMakeLists.txt                  ← WSL build (with Arrow)
└── api_bundle_for_vm.sh            ← bundle script (not used for VM build)

/home/flureelabs/                        ← VM home
└── address-parser-service/              ← parent folder for this service
    ├── address-service/                 ← running service
    │   ├── bin/address-service          ← compiled binary
    │   ├── config/config.json           ← active config
    │   ├── data/
    │   │   ├── libpostal/               ← standard model data
    │   │   └── libpostal_senzing/       ← Senzing v1.2 model data
    │   ├── lib/                         ← bundled libs
    │   ├── logs/                        ← service.log lives here
    │   ├── uploads/
    │   └── run.sh                       ← start script
    ├── address-service-build/           ← VM source + build dir
    │   ├── src/
    │   ├── config/
    │   ├── CMakeLists.txt               ← VM build (Arrow removed)
    │   └── build/                       ← cmake build output
    ├── address-service-bundle.tar.gz    ← original WSL bundle
    ├── address-service-src.tar.gz       ← source tarball from WSL
    ├── drogon/                          ← Drogon source (compiled on VM)
    ├── libpostal/                       ← libpostal source (compiled twice)
    └── vm-setup.sh                      ← setup script
```

---

## All Endpoints

| Method | URL | Description |
|--------|-----|-------------|
| POST | `/api/v1/parse` | Parse single address |
| POST | `/api/v1/normalize` | Libpostal expansions |
| POST | `/api/v1/batch` | Parse up to 5000 addresses |
| POST | `/api/v1/enrich` | Enrich JSON records |
| GET | `/health/live` | Liveness probe |
| GET | `/health/ready` | Readiness probe |
| GET | `/health/startup` | Startup probe |
| GET | `/health/info` | Full stats + config |
| GET | `/metrics` | Prometheus metrics |

---

## Running the Service

### Run in foreground (Ctrl+C to stop — good for testing/debugging)
```bash
cd /home/flureelabs/address-parser-service/address-service
bash run.sh
```

### Run in background (survives Ctrl+C)
```bash
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
echo "PID: $!"
```

### View live logs
```bash
tail -f /home/flureelabs/address-parser-service/address-service/logs/service.log
```

### Stop the service
```bash
pkill -f "address-service"
# or
kill $(pgrep -f "address-service")
```

### Run as systemd service (auto-start on reboot, auto-restart on crash)
```bash
# Start
sudo systemctl start address-service

# Stop
sudo systemctl stop address-service

# Restart
sudo systemctl restart address-service

# Status
sudo systemctl status address-service

# Live logs
sudo journalctl -u address-service -f

# Enable auto-start on reboot
sudo systemctl enable address-service

# Disable auto-start
sudo systemctl disable address-service
```

---

## Switch Between Standard and Senzing Model

**One line change in config — no rebuild needed:**

```bash
nano /home/flureelabs/address-parser-service/address-service/config/config.json
```

```json
"data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal"          ← standard
"data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal_senzing"  ← senzing
```

Then restart:
```bash
# If running via nohup:
pkill -f "address-service"
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &

# If running via systemd:
sudo systemctl restart address-service
```

---

## What to Run After Code Changes

### Changed only `.cc` or `.h` files (no CMakeLists.txt change)
```bash
# On VM:
cd /home/flureelabs/address-parser-service/address-service-build/build
make -j$(nproc)
cp address-service /home/flureelabs/address-parser-service/address-service/bin/address-service

# Restart service
pkill -f "address-service"
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
```

### Changed CMakeLists.txt (new file, new option, new library)
```bash
# On VM:
cd /home/flureelabs/address-parser-service/address-service-build/build
cmake3 .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTS=OFF \
    -DENABLE_MIMALLOC=OFF \
    -DENABLE_OPENMP=OFF \
    -DCMAKE_C_COMPILER=/usr/bin/gcc10-gcc \
    -DCMAKE_CXX_COMPILER=/usr/bin/gcc10-g++ \
    -DOPENSSL_ROOT_DIR=/usr \
    -DOPENSSL_INCLUDE_DIR=/usr/include \
    -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so.1.1 \
    -DOPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so.1.1
make -j$(nproc)
cp address-service /home/flureelabs/address-parser-service/address-service/bin/address-service

# Restart service
pkill -f "address-service"
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
```

### Changed config.json only
```bash
# No rebuild needed — just restart
pkill -f "address-service"
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
```

### Quick reference table
| What changed | Command needed |
|---|---|
| `.cc` / `.h` only | `make -j$(nproc)` → copy binary → restart |
| `CMakeLists.txt` | `cmake3 ..` → `make` → copy binary → restart |
| `config.json` only | restart only, no rebuild |
| `data_dir` in config | restart only |

---

## Sample API Tests

### Parse single address
```bash
curl -s -X POST http://100.31.167.228:8090/api/v1/parse \
  -H "Content-Type: application/json" \
  -d '{"address": "123 MG Road, Bengaluru 560001"}' | python3 -m json.tool
```
```json
{
    "house_number": "123",
    "road": "mahatma gandhi road",
    "city": "bengaluru",
    "state": "karnataka",
    "postcode": "560001",
    "country": "india",
    "confidence": 0.97,
    "from_cache": false,
    "latency_ms": 18.1
}
```

### Normalize
```bash
curl -s -X POST http://100.31.167.228:8090/api/v1/normalize \
  -H "Content-Type: application/json" \
  -d '{"address": "123 MG Road, Bengaluru 560001"}' | python3 -m json.tool
```

### Batch
```bash
curl -s -X POST http://100.31.167.228:8090/api/v1/batch \
  -H "Content-Type: application/json" \
  -d '{
    "addresses": [
      "123 MG Road, Bengaluru 560001",
      "350 Fifth Ave, New York NY 10118",
      "x"
    ]
  }' | python3 -m json.tool
```

### Enrich — ALL columns
```bash
curl -s -X POST http://100.31.167.228:8090/api/v1/enrich \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"id": 1, "name": "Alice", "addr": "123 MG Road, Bengaluru 560001", "phone": "9876"},
      {"id": 2, "name": "Bob",   "addr": "350 Fifth Ave, New York NY 10118"}
    ],
    "metadata": {
      "address_column": "addr",
      "key_column": "id",
      "normalize_columns": "ALL"
    }
  }' | python3 -m json.tool
```

### Enrich — specific columns only
```bash
curl -s -X POST http://100.31.167.228:8090/api/v1/enrich \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"id": 1, "name": "Alice", "addr": "123 MG Road, Bengaluru 560001", "phone": "9876"}
    ],
    "metadata": {
      "address_column": "addr",
      "key_column": "id",
      "normalize_columns": "city,state,postcode,country"
    }
  }' | python3 -m json.tool
```

### Health info
```bash
curl -s http://100.31.167.228:8090/health/info | python3 -m json.tool
```

### Metrics (Prometheus)
```bash
curl -s http://100.31.167.228:8090/metrics
```

---

## Deployment Steps — WSL to VM (what we did)

### Problem
- WSL: Ubuntu 22.04, GLIBC 2.35, GCC 11
- VM: Amazon Linux 2, GLIBC 2.26, GCC 7.3
- Binary compiled on WSL cannot run on VM (GLIBC version mismatch)
- Solution: compile everything from source on VM

### Step 1 — Install build tools on VM
```bash
sudo yum install -y gcc gcc-c++ make git autoconf automake libtool \
    pkg-config openssl11-devel zlib-devel libuuid-devel curl-devel \
    jsoncpp-devel cmake3
sudo yum remove -y openssl-devel   # remove old 1.0 headers
sudo yum install -y openssl11-devel gcc10 gcc10-c++
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
echo 'export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH' >> ~/.bashrc
```

### Step 2 — Build libpostal on VM (standard model)
```bash
cd ~
git clone https://github.com/openvenues/libpostal
cd libpostal
./bootstrap.sh
./configure --datadir="/home/flureelabs/address-service/data/libpostal"
make -j$(nproc)
sudo make install
sudo ldconfig
```

### Step 3 — Build Drogon on VM
```bash
cd ~
git clone --depth 1 --branch v1.9.3 https://github.com/drogonframework/drogon
cd drogon
git submodule update --init
mkdir build && cd build
cmake3 .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DCMAKE_C_COMPILER=/usr/bin/gcc10-gcc \
    -DCMAKE_CXX_COMPILER=/usr/bin/gcc10-g++ \
    -DOPENSSL_ROOT_DIR=/usr \
    -DOPENSSL_INCLUDE_DIR=/usr/include \
    -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so.1.1 \
    -DOPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so.1.1
make -j$(nproc)
sudo make install
sudo ldconfig
```

### Step 4 — Copy source to VM
```bash
# On WSL — create source tarball (Arrow removed from CMakeLists.txt)
cd ~/FAISS-Actual-26June25/address-service-optimized
tar -czf address-service-src.tar.gz src/ config/ CMakeLists.txt
# Drag address-service-src.tar.gz to VM via WinSCP → /home/flureelabs/
```

### Step 5 — Build service on VM
```bash
mkdir -p ~/address-service-build
tar -xzf address-service-src.tar.gz -C ~/address-service-build/
cd ~/address-service-build
mkdir -p build && cd build
cmake3 .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTS=OFF \
    -DENABLE_MIMALLOC=OFF \
    -DENABLE_OPENMP=OFF \
    -DCMAKE_C_COMPILER=/usr/bin/gcc10-gcc \
    -DCMAKE_CXX_COMPILER=/usr/bin/gcc10-g++ \
    -DOPENSSL_ROOT_DIR=/usr \
    -DOPENSSL_INCLUDE_DIR=/usr/include \
    -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so.1.1 \
    -DOPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so.1.1
make -j$(nproc)
```

### Step 6 — Copy libpostal data via WinSCP
```
WSL: ~/libpostal_data/libpostal/  →  VM: /home/flureelabs/address-service-data
```
Then on VM:
```bash
mkdir -p ~/address-service/data
mv ~/address-service-data ~/address-service/data/libpostal
```

### Step 7 — Set up service directory
```bash
mkdir -p ~/address-service/bin ~/address-service/logs ~/address-service/uploads
cp ~/address-service-build/build/address-service ~/address-service/bin/
sed -i 's/"port": 8080/"port": 8090/' ~/address-service/config/config.json

cat > ~/address-service/run.sh << 'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="/usr/local/lib:${SCRIPT_DIR}/lib:${LD_LIBRARY_PATH:-}"
cd "${SCRIPT_DIR}"
exec "${SCRIPT_DIR}/bin/address-service" -c "${SCRIPT_DIR}/config/config.json"
EOF
chmod +x ~/address-service/run.sh
```

### Step 8 — Install systemd service
```bash
sudo cp ~/address-service/address-service.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable address-service
sudo systemctl start address-service
```

### Step 9 — Add Senzing model (optional)
```bash
# Recompile libpostal with Senzing
cd ~/libpostal
make clean
./configure \
    --datadir="/home/flureelabs/address-service/data/libpostal_senzing" \
    MODEL=senzing
make -j$(nproc)
sudo make install && sudo ldconfig

# Copy base data, download Senzing parser
cp -r ~/address-service/data/libpostal \
      ~/address-service/data/libpostal_senzing
cd /tmp
curl -L -o parser.tar.gz \
    https://public-read-libpostal-data.s3.amazonaws.com/v1.2.0/parser.tar.gz
tar -xzf parser.tar.gz -C ~/address-service/data/libpostal_senzing/
# Fix nested dir if needed:
mv ~/address-service/data/libpostal_senzing/libpostal/* \
   ~/address-service/data/libpostal_senzing/ 2>/dev/null || true
rm -rf ~/address-service/data/libpostal_senzing/libpostal
rm parser.tar.gz
```

### Step 10 — Open AWS port
```
AWS Console → EC2 → Security Groups → Inbound Rules → Add Rule
Type: Custom TCP | Port: 8090 | Source: 0.0.0.0/0
```

---

## VM Environment Summary

| What | Value |
|------|-------|
| VM | AWS EC2, Amazon Linux 2 |
| Public IP | 100.31.167.228 |
| Port | 8090 |
| User | flureelabs |
| Service dir | /home/flureelabs/address-parser-service/address-service |
| Standard data | /home/flureelabs/address-parser-service/address-service/data/libpostal |
| Senzing data | /home/flureelabs/address-parser-service/address-service/data/libpostal_senzing |
| Source + build | /home/flureelabs/address-parser-service/address-service-build |
| libpostal source | /home/flureelabs/address-parser-service/libpostal |
| Drogon source | /home/flureelabs/address-parser-service/drogon |
