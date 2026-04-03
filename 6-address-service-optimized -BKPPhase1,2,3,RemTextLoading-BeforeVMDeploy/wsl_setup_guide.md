# Address Normalization Service — WSL Setup Guide
## Everything from scratch: install, build, data prep, LMDB indexing

---

## Tech Stack

| Component | Technology | Purpose |
|-----------|-----------|---------|
| HTTP Framework | Drogon 1.9.3 | Async non-blocking HTTP, TechEmpower top performer |
| Core Parser | libpostal 1.1.4 | CRF-based address parsing, 233 countries, 100+ languages |
| Parser Model | Senzing v1.2 | 1.2B training records, better business/corporate addresses |
| Geo Lookup | LMDB 0.9.24 | 6 databases, 249 countries, ~5MB RAM, <1ms lookup |
| JSON | JsonCpp | Request/response serialization |
| Logging | loguru | Structured logging with timestamps + log rotation |
| Cache | 64-shard LRU | FNV-1a hash, cache-line aligned, TTL expiry |
| Allocator | mimalloc (optional) | 2–3x faster than glibc malloc for small strings |
| Build | CMake 3.16+ / GCC 11+ | C++17, -O3, LTO |
| Auth | JWT + API key | AuthFilter delegates to JWT or API key based on config |
| Monitoring | Prometheus format | Lock-free atomic histograms, per-phase counters |
| Data pipeline | Python 3 | GeoNames txt → CSV conversion (build_csv.py) |
| Index builder | C++17 | CSV → LMDB indexing (build_lmdb.cc) |

---

## Processing Pipeline

```
Raw address input
  → InputValidator      (length ≤ 500 chars, UTF-8, no null bytes, no control chars)
  → PreProcessor        (lowercase, trim, expand abbreviations: MG→Mahatma Gandhi, BLDG→Building)
  → CacheManager.get()  (return cached result immediately if hit — skips all phases below)
  → AddressParser       (libpostal parse with language/country hints, 16-slot mutex pool)
  → ConfidenceScorer    (0.0–1.0: completeness 35% + postcode 25% + cross-field 20% + tokens 20%)
  → RuleEngine          (PIN→state/city, ZIP→country, misspelling fix, state abbreviation expand)
  → LLMFallback         (Phase 4 — disabled by default, llama.cpp Mistral 7B Q4)
  → CacheManager.put()  (cache if confidence ≥ 0.5)

For /api/v1/enrich/geo/lmdb only:
  → Phase 2A: postcode → city + state  (postal.lmdb — 1M entries)
  → Phase 2B: city → state             (cities.lmdb — 3.2M entries)
  → Phase 2C: alias resolution         (aliases.lmdb — Bombay→Mumbai)
  → Phase 2D: country name fill        (countries.lmdb — 249 countries)
  → rescore after geo enrichment
```

---

## Environment

| What | Value |
|------|-------|
| OS | WSL Ubuntu 22.04 |
| GCC | 11+ (system default) |
| Port | 8080 |
| Project dir | `~/FAISS-Actual-26June25/address-service-optimized` |
| libpostal data | `~/libpostal_data/libpostal` |
| Senzing data | `~/libpostal_data/libpostal_senzing` |
| GeoNames raw data | `~/libpostal_data/geonames` |
| GeoNames LMDB indexes | `~/libpostal_data/geonames/lmdb` |

---

## Part 1 — System Dependencies

### Install build tools
```bash
sudo apt update
sudo apt install -y \
    build-essential cmake git pkg-config \
    libssl-dev zlib1g-dev uuid-dev \
    libjsoncpp-dev \
    liblmdb-dev \
    curl wget unzip python3
```

### Verify LMDB headers
```bash
find /usr -name "lmdb.h" 2>/dev/null
# Expected: /usr/include/lmdb.h
dpkg -l liblmdb-dev | grep liblmdb
# Expected: liblmdb-dev 0.9.24-1build2
```

---

## Part 2 — Build libpostal

### Clone and build with standard model
```bash
cd ~
git clone https://github.com/openvenues/libpostal
cd libpostal
./bootstrap.sh
./configure --datadir=/home/noorulk/libpostal_data/libpostal
make -j$(nproc)
sudo make install
sudo ldconfig
```

This downloads ~2.2GB of model data into `~/libpostal_data/libpostal`.

### Build again with Senzing v1.2 model (better for business addresses)
```bash
cd ~/libpostal
make clean
./configure \
    --datadir=/home/noorulk/libpostal_data/libpostal_senzing \
    MODEL=senzing
make -j$(nproc)
sudo make install
sudo ldconfig
```

Then download the Senzing parser data:
```bash
mkdir -p ~/libpostal_data/libpostal_senzing
# Copy base data from standard model
cp -r ~/libpostal_data/libpostal/* ~/libpostal_data/libpostal_senzing/

# Download Senzing parser
cd /tmp
curl -L -o parser.tar.gz \
    https://public-read-libpostal-data.s3.amazonaws.com/v1.2.0/parser.tar.gz
tar -xzf parser.tar.gz -C ~/libpostal_data/libpostal_senzing/
rm parser.tar.gz
```

---

## Part 3 — Build Drogon Framework

```bash
cd ~
git clone --depth 1 --branch v1.9.3 https://github.com/drogonframework/drogon
cd drogon
git submodule update --init
mkdir build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DBUILD_EXAMPLES=OFF
make -j$(nproc)
sudo make install
sudo ldconfig
```

---

## Part 4 — Build Apache Arrow (optional — for CSV export feature)

```bash
cd ~
git clone https://github.com/apache/arrow.git
cd arrow/cpp
mkdir build && cd build
cmake .. \
    -DARROW_CSV=ON \
    -DARROW_FILESYSTEM=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DARROW_BUILD_TESTS=OFF
make -j$(nproc)
```

Arrow is linked at: `~/FAISS-Actual-26June25/arrow/cpp/build/release/libarrow.so`

---

## Part 5 — loguru Logging Library

loguru is a header-only + one .cc file logging library.

```bash
# Files should be at:
ls ~/FAISS-Actual-26June25/loguru.hpp
ls ~/FAISS-Actual-26June25/loguru.cpp

# Copy to project
mkdir -p ~/FAISS-Actual-26June25/address-service-optimized/third_party_libs
cp ~/FAISS-Actual-26June25/loguru.hpp \
   ~/FAISS-Actual-26June25/loguru.cpp \
   ~/FAISS-Actual-26June25/address-service-optimized/third_party_libs/
```

---

## Part 6 — GeoNames Data Downloads

All GeoNames data lives in `~/libpostal_data/geonames/`.

```bash
mkdir -p ~/libpostal_data/geonames
cd ~/libpostal_data/geonames
```

### Download all required files

> ⚠️ **Important:** GeoNames has TWO different files both called `allCountries.txt`:
> - `geonames.org/export/dump/allCountries.zip` → places database (cities, rivers, mountains etc)
> - `geonames.org/export/zip/allCountries.zip`  → postal codes database
>
> We download the postal one first and immediately rename it to `postal_codes.txt` to avoid conflict.

```bash
# Admin1 state codes (all countries)
wget https://download.geonames.org/export/dump/admin1CodesASCII.txt

# Admin2 district codes (all countries)
wget https://download.geonames.org/export/dump/admin2Codes.txt

# All cities with population > 15000 (global)
wget https://download.geonames.org/export/dump/cities15000.zip
unzip cities15000.zip
rm cities15000.zip
# Result: cities15000.txt (~7.5MB)

# Postal codes — download from /export/zip/ and rename immediately
# MUST rename before downloading allCountries from /export/dump/ — same filename conflict
wget https://download.geonames.org/export/zip/allCountries.zip -O postal_codes.zip
unzip postal_codes.zip
mv allCountries.txt postal_codes.txt
rm postal_codes.zip
# Result: postal_codes.txt (~135MB)

# All GeoNames places — download AFTER postal rename is done
# This is from /export/dump/ — produces allCountries.txt (places database)
wget https://download.geonames.org/export/dump/allCountries.zip
unzip allCountries.zip
rm allCountries.zip
# Result: allCountries.txt (~1.7GB) — places database, referenced as-is in build_csv.py

# ISO 3166 country codes (249 countries)
wget -O iso3166.csv \
    https://raw.githubusercontent.com/lukes/ISO-3166-Countries-with-Regional-Codes/master/all/all.csv
# Result: iso3166.csv (~21KB)

# Alternate city names (English aliases — Bombay→Mumbai etc)
wget https://download.geonames.org/export/dump/alternateNamesV2.zip
unzip alternateNamesV2.zip
rm alternateNamesV2.zip
# Result: alternateNamesV2.txt (~750MB) + iso-languagecodes.txt
```

### Verify all files
```bash
ls -lh ~/libpostal_data/geonames/
# Expected files:
# admin1CodesASCII.txt  (~148KB)
# admin2Codes.txt       (~2.3MB)
# allCountries.txt      (~1.7GB)  ← places database
# alternateNamesV2.txt  (~750MB)  ← city aliases
# cities15000.txt       (~7.5MB)
# iso3166.csv           (~21KB)
# postal_codes.txt      (~135MB)
```

---

## Part 7 — Build CSV Converter Tool

This converts GeoNames txt files → clean CSVs ready for LMDB indexing.

```bash
mkdir -p ~/FAISS-Actual-26June25/address-service-optimized/tools
# Save build_csv.py to tools/ directory (see project source)
```

### Run the CSV converter
```bash
python3 ~/FAISS-Actual-26June25/address-service-optimized/tools/build_csv.py \
    --data-dir ~/libpostal_data/geonames \
    --out-dir  ~/libpostal_data/geonames/csv
```

This takes ~60 seconds. Expected output:
```
[build_csv] Building admin1: ... → admin1_filtered.csv
[build_csv]   admin1 rows: 3862
[build_csv] Building admin2: ... → admin2_filtered.csv
[build_csv]   admin2 rows: 47496
[build_csv] Building postal: ... → postal_filtered.csv
[build_csv]   postal rows: 1080414
[build_csv] Building cities → cities_filtered.csv
[build_csv]   cities total: 3220878
[build_csv] Building countries: ... → countries_filtered.csv
[build_csv]   countries rows: 249
[build_csv] Building city aliases → city_aliases_filtered.csv
[build_csv]   city aliases written: 2661
[build_csv] Done in 56.9s
```

### Verify CSVs
```bash
ls -lh ~/libpostal_data/geonames/csv/
# admin1_filtered.csv       — 3,862 rows  (all countries state codes)
# admin2_filtered.csv       — 47,496 rows (district codes)
# postal_filtered.csv       — 1,080,414 rows (all countries postal codes)
# cities_filtered.csv       — 3,220,878 rows (all countries cities)
# countries_filtered.csv    — 249 rows (ISO 3166)
# city_aliases_filtered.csv — 2,661 rows (English city aliases)
```

---

## Part 8 — Build LMDB Index Builder Tool

This converts the CSVs → LMDB binary index files for fast lookup.

```bash
# Compile the tool
g++ -O2 -std=c++17 \
    ~/FAISS-Actual-26June25/address-service-optimized/tools/build_lmdb.cc \
    -o ~/FAISS-Actual-26June25/address-service-optimized/tools/build_lmdb \
    -llmdb

echo "Build: $?"
```

### Run the LMDB builder
```bash
~/FAISS-Actual-26June25/address-service-optimized/tools/build_lmdb \
    --csv-dir  ~/libpostal_data/geonames/csv \
    --lmdb-dir ~/libpostal_data/geonames/lmdb
```

Takes ~7 seconds. Expected output:
```
[build_lmdb] Building admin1.lmdb...
[build_lmdb]   .../admin1.lmdb → 3862 entries
[build_lmdb] Building admin2.lmdb...
[build_lmdb]   .../admin2.lmdb → 47496 entries
[build_lmdb] Building postal.lmdb (1M entries)...
[build_lmdb]   .../postal.lmdb → 1080414 entries
[build_lmdb] Building cities.lmdb (3M+ entries)...
[build_lmdb]   .../cities.lmdb → 3220878 entries
[build_lmdb] Building countries.lmdb...
[build_lmdb]   .../countries.lmdb → 996 entries
[build_lmdb] Building aliases.lmdb...
[build_lmdb]   .../aliases.lmdb → 2661 entries
[build_lmdb] Done in 6.3s
```

### Verify LMDB files
```bash
ls ~/libpostal_data/geonames/lmdb/
# admin1.lmdb/   admin2.lmdb/   postal.lmdb/
# cities.lmdb/   countries.lmdb/ aliases.lmdb/

ls -lh ~/libpostal_data/geonames/lmdb/postal.lmdb/
# data.mdb  (~3.7MB)  lock.mdb  (~8KB)

ls -lh ~/libpostal_data/geonames/lmdb/cities.lmdb/
# data.mdb  (~41MB)   lock.mdb  (~8KB)
```

---

## Part 9 — Build the Address Service

### First time / clean build
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
rm -rf build
mkdir build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTS=OFF \
    -DENABLE_MIMALLOC=OFF
make -j$(nproc)
echo "Build: $?"
```

### Incremental build (after .cc/.h changes only)
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized/build
make -j$(nproc)
echo "Build: $?"
```

### After CMakeLists.txt changes (new file added etc)
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized/build
cmake ..
make -j$(nproc)
```

---

## Part 10 — config.json Setup

The config file lives at:
```
~/FAISS-Actual-26June25/address-service-optimized/config/config.json
```

Key sections:
```json
{
    "server": {
        "port": 8080,
        "threads": 0
    },
    "libpostal": {
        "data_dir": "/home/noorulk/libpostal_data/libpostal_senzing"
    },
    "geonames": {
        "lmdb_dir": "/home/noorulk/libpostal_data/geonames/lmdb"
    },
    "cache": {
        "enabled": true,
        "max_entries": 5000000,
        "ttl_seconds": 86400
    },
    "batch": {
        "max_size": 10000,
        "timeout_ms": 30000
    },
    "rules_engine": {
        "enabled": true
    },
    "llm": {
        "enabled": false
    },
    "security": {
        "enabled": false,
        "max_address_length": 500
    }
}
```

### Switch libpostal model (no rebuild needed)
```json
// Standard model:
"data_dir": "/home/noorulk/libpostal_data/libpostal"

// Senzing v1.2 (better for business/corporate addresses):
"data_dir": "/home/noorulk/libpostal_data/libpostal_senzing"
```

---

## Part 11 — Running the Service

### Start
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
./build/address-service -c config/config.json
```

### Expected startup log
```
[main] Configuration loaded from config/config.json
====================================================
  Address Normalization Service v1.0.0
====================================================
  Port:           8080
  Worker threads: 7
  Cache:          enabled (max 5000000 entries, TTL 86400s)
  Rule engine:    enabled
  LLM fallback:   disabled
  Auth:           disabled
====================================================
  Endpoints available:
  ┌─ POST  http://0.0.0.0:8080/api/v1/parse
  ├─ POST  http://0.0.0.0:8080/api/v1/normalize
  ├─ POST  http://0.0.0.0:8080/api/v1/batch
  ├─ POST  http://0.0.0.0:8080/api/v1/enrich
  ├─ POST  http://0.0.0.0:8080/api/v1/enrich/geo/lmdb
  ├─ GET   http://0.0.0.0:8080/ref/v1/postal/{code}
  ├─ POST  http://0.0.0.0:8080/ref/v1/postal/batch
  ├─ GET   http://0.0.0.0:8080/ref/v1/country/{code}
  ├─ GET   http://0.0.0.0:8080/ref/v1/state/{country}/{abbrev}
  ├─ GET   http://0.0.0.0:8080/ref/v1/city/search
  ├─ POST  http://0.0.0.0:8080/ref/v1/enrich
  ├─ POST  http://0.0.0.0:8080/ref/v1/validate
  ├─ GET   http://0.0.0.0:8080/health/live
  ...
====================================================
[main] Initializing libpostal (this takes 15-30s)...
[AddressParser] libpostal ready
[main] GeoNamesLMDB ready | dir=.../geonames/lmdb
[main] All services initialized, starting HTTP server...
```

### Stop
```bash
# Ctrl+C if running in foreground
# Or:
pkill -f "address-service"
```

### Quick health check
```bash
curl http://localhost:8080/health/live
# {"status":"alive","timestamp":...}
```

---

## Part 12 — Changing Max Records (single place)

All endpoints (`/api/v1/batch`, `/api/v1/enrich`, `/api/v1/enrich/geo/lmdb`) now read from one place:

```
config/config.json → "batch" → "max_size"
```

**To change max records — edit config.json only, then restart (no rebuild):**

```bash
nano ~/FAISS-Actual-26June25/address-service-optimized/config/config.json
```

Find and change:
```json
"batch": {
    "max_size": 10000,
    "timeout_ms": 60000
}
```

Then restart:
```bash
pkill -f "address-service"
cd ~/FAISS-Actual-26June25/address-service-optimized
./build/address-service -c config/config.json
```

**Do NOT change these — they no longer control anything:**
- `EnrichController.h` — `MAX_RECORDS` removed
- `GeoEnrichLMDBController.h` — `MAX_RECORDS` removed
- Both now read `g_config.batch_max_size` which comes from config.json

---

## Part 13 — What to Run After Each Type of Change

### Changed only `.cc` or `.h` files
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized/build
make -j$(nproc)
echo "Build: $?"
# Then restart:
pkill -f "address-service"
cd ~/FAISS-Actual-26June25/address-service-optimized
./build/address-service -c config/config.json
```

### Changed `config.json` only
```bash
# No rebuild needed — just restart
pkill -f "address-service"
cd ~/FAISS-Actual-26June25/address-service-optimized
./build/address-service -c config/config.json
```

### Added a new `.cc` file (new controller, new service)
```bash
# Step 1 — add to CMakeLists.txt SOURCES block:
nano ~/FAISS-Actual-26June25/address-service-optimized/CMakeLists.txt
# Add line: src/controllers/MyNewController.cc

# Step 2 — full cmake + make:
cd ~/FAISS-Actual-26June25/address-service-optimized/build
cmake ..
make -j$(nproc)
echo "Build: $?"

# Step 3 — restart
pkill -f "address-service"
cd ~/FAISS-Actual-26June25/address-service-optimized
./build/address-service -c config/config.json
```

### Changed `CMakeLists.txt` (options, libraries, flags)
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized/build
cmake ..
make -j$(nproc)
# Then restart
```

### Full clean rebuild (weird linker errors, after major changes)
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
rm -rf build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DENABLE_MIMALLOC=OFF
make -j$(nproc)
echo "Build: $?"
```

### Quick reference table
| What changed | cmake needed | make needed | restart needed |
|---|---|---|---|
| `.cc` or `.h` file | ❌ | ✅ | ✅ |
| `config.json` only | ❌ | ❌ | ✅ |
| New `.cc` file added | ✅ | ✅ | ✅ |
| `CMakeLists.txt` changed | ✅ | ✅ | ✅ |
| max_size in config.json | ❌ | ❌ | ✅ |
| libpostal data_dir in config | ❌ | ❌ | ✅ |
| LMDB lmdb_dir in config | ❌ | ❌ | ✅ |

---

## Part 14 — How to Check Status and Logs

### Is the service running?
```bash
pgrep -a "address-service"
# Shows PID and command if running, nothing if stopped
```

### Quick health check
```bash
curl -s http://localhost:8080/health/live
# {"status":"alive","timestamp":...}
```

### Full stats (cache hits, latency, config)
```bash
curl -s http://localhost:8080/health/info | python3 -m json.tool
```

### View current log file
```bash
# Find latest log
ls -lt ~/FAISS-Actual-26June25/address-service-optimized/logs/ | head -3

# Tail live
tail -f ~/FAISS-Actual-26June25/address-service-optimized/logs/service_*.log
```

### View startup banner in log
```bash
cat ~/FAISS-Actual-26June25/address-service-optimized/logs/service_*.log | head -50
```

### Stop the service
```bash
pkill -f "address-service"
# If that doesn't work:
pkill -9 -f "address-service"
```

---

## Part 15 — Log Rotation

Logs are written to `./logs/` with timestamp filename:
```
logs/service_20260402_151600.log
```

- Max 20 log files kept
- When 20 is exceeded, oldest 5 are deleted automatically
- Each service restart creates a new log file

```bash
# View current log
ls -lt ~/FAISS-Actual-26June25/address-service-optimized/logs/

# Tail live
tail -f ~/FAISS-Actual-26June25/address-service-optimized/logs/service_*.log
```

---

## Part 16 — Environment Variable Overrides

These override `config.json` values at runtime — no rebuild needed, just restart:

```bash
PORT=9090                          # override port (default: 8080)
LIBPOSTAL_DATA_DIR=/custom/path    # override libpostal data path
CACHE_MAX_SIZE=1000000             # override max cache entries
AUTH_ENABLED=true                  # enable auth at runtime
AUTH_MODE=api_key                  # api_key | jwt | both
ENABLE_LANGUAGE_CLASSIFIER=false   # skip language classifier (~300MB RAM saved)
```

Example:
```bash
PORT=9090 AUTH_ENABLED=false \
  ./build/address-service -c config/config.json
```

---

## Part 17 — Debug File Output (EnrichController)

In `src/controllers/EnrichController.h` change these flags and rebuild with `make -j$(nproc)`:

```cpp
// Production (no file I/O — default):
static constexpr bool WRITE_JSON_DEBUG = false;
static constexpr bool WRITE_CSV_DEBUG  = false;

// Write JSON debug file after each request:
static constexpr bool WRITE_JSON_DEBUG = true;
static constexpr bool WRITE_CSV_DEBUG  = false;

// Write both JSON + CSV debug files:
static constexpr bool WRITE_JSON_DEBUG = true;
static constexpr bool WRITE_CSV_DEBUG  = true;
```

Debug files are written to:
```
./uploads/debug_results.json
./uploads/debug_results.csv
```

Same flags exist in `GeoEnrichLMDBController.h` — writes to:
```
./uploads/lmdb_debug_results.json
./uploads/lmdb_debug_results.csv
```

---

## Part 18 — Adding a New Controller (checklist)

1. Create `src/controllers/MyController.h` — inherit `drogon::HttpController<MyController>`, define routes in `METHOD_LIST_BEGIN/END`
2. Create `src/controllers/MyController.cc` — declare globals with `extern`, implement handler
3. Add to `CMakeLists.txt` SOURCES block: `src/controllers/MyController.cc`
4. Add banner line to `src/main.cc` `printBanner()` function
5. Run `cmake .. && make -j$(nproc)`

The LMDB indexes are built once and reused forever. Only rebuild if:

| Reason | Command |
|--------|---------|
| New GeoNames data downloaded | Re-run `build_csv.py` then `build_lmdb` |
| Want to add more countries (already all countries) | Not needed — all 249 already included |
| LMDB files corrupted | Delete `lmdb/` folder and re-run `build_lmdb` |

```bash
# Full rebuild of LMDB from existing CSVs (fast — ~7s)
rm -rf ~/libpostal_data/geonames/lmdb/*
~/FAISS-Actual-26June25/address-service-optimized/tools/build_lmdb \
    --csv-dir  ~/libpostal_data/geonames/csv \
    --lmdb-dir ~/libpostal_data/geonames/lmdb
```

---

## Part 14 — Project Source File Summary

```
address-service-optimized/
├── CMakeLists.txt
├── config/config.json
├── third_party_libs/
│   ├── loguru.hpp               ← logging library
│   └── loguru.cpp
├── tools/
│   ├── build_csv.py             ← txt → CSV converter
│   └── build_lmdb.cc            ← CSV → LMDB index builder (7 databases)
└── src/
    ├── main.cc                  ← startup, init all services, Drogon config
    ├── models/
    │   └── AddressModels.h      ← ParsedAddress, ServiceConfig, ScopedTimer
    ├── utils/
    │   ├── InputValidator.h     ← length, UTF-8, null byte checks
    │   └── Logger.h             ← loguru wrapper (ADDR_LOG_INFO etc)
    ├── services/
    │   ├── AddressParser.cc/h   ← libpostal wrapper (16-slot mutex pool)
    │   ├── PreProcessor.cc/h    ← clean + abbreviation expansion
    │   ├── RuleEngine.cc/h      ← PIN/ZIP inference, state normalize
    │   ├── ConfidenceScorer.cc/h← weighted 0.0-1.0 confidence score
    │   ├── CacheManager.cc/h    ← 64-shard LRU, FNV-1a hash, TTL
    │   ├── MetricsCollector.cc/h← lock-free Prometheus histograms
    │   ├── LLMFallback.cc/h     ← Phase 4 llama.cpp (disabled)
    │   ├── GeoNamesLMDB.cc/h    ← LMDB geo lookup (all 249 countries, 7 databases)
    │   └── GeoNamesDB.h         ← GeoEntry struct (kept for compatibility)
    └── controllers/
        ├── ParseController.cc/h          ← /api/v1/parse, /normalize
        ├── BatchController.cc/h          ← /api/v1/batch
        ├── EnrichController.cc/h         ← /api/v1/enrich
        ├── GeoEnrichLMDBController.cc/h  ← /api/v1/enrich/geo/lmdb
        ├── RefPostalController.cc/h      ← /ref/v1/postal + /ref/v1/postal/reverse
        ├── RefCountryController.cc/h     ← /ref/v1/country
        ├── RefStateController.cc/h       ← /ref/v1/state
        ├── RefCityController.cc/h        ← /ref/v1/city/search
        ├── RefEnrichController.cc/h      ← /ref/v1/enrich, /ref/v1/validate
        ├── RefAbbreviationController.cc/h← /ref/v1/abbreviation/expand
        ├── HealthController.cc/h         ← /health/*
        ├── MetricsController.cc/h        ← /metrics
        ├── AuthFilter.cc/h               ← unified auth dispatcher
        ├── ApiKeyFilter.cc/h             ← API key validation
        └── JwtAuthFilter.cc/h            ← JWT validation
```

---

## Quick Reference — What to Run After Changes

| What changed | Command |
|---|---|
| `.cc` or `.h` file | `cd build && make -j$(nproc)` |
| `CMakeLists.txt` | `cd build && cmake .. && make -j$(nproc)` |
| `config.json` only | Just restart — no rebuild |
| GeoNames txt files updated | Re-run `build_csv.py` → `build_lmdb` → restart |
| LMDB only (CSVs unchanged) | Re-run `build_lmdb` only → restart |
