# Address Normalization Service — VM Runbook
## Complete reference for deployment, operation, and maintenance on Amazon Linux 2

---

## Project Overview

A high-performance C++ HTTP service that parses, normalizes and enriches raw address strings using libpostal + LMDB geo lookup. Built on the Drogon framework.

### Libraries Used
| Library | Purpose |
|---------|---------|
| Drogon v1.9.3 | HTTP server framework (C++) |
| libpostal 1.1.4 | Address parsing + normalization |
| libpostal Senzing v1.2 | Enhanced parser model (1.2B training records) |
| LMDB 0.9.22 | Geo lookup databases (7 databases, 249 countries) |
| JsonCpp 1.7.2 | JSON serialization |
| OpenSSL 1.1 | TLS/crypto |
| loguru | Structured logging with timestamps + log rotation |
| pthread | Threading |

---

## VM Directory Layout

```
/home/flureelabs/address-parser-service/
├── address-service/                    ← RUNNING SERVICE (edit config here)
│   ├── bin/address-service             ← compiled binary (copy here after make)
│   ├── config/config.json              ← LIVE config — only ever edit this one
│   ├── data/
│   │   ├── libpostal/                  ← standard libpostal model data
│   │   ├── libpostal_senzing/          ← Senzing v1.2 model data (active)
│   │   └── geonames/
│   │       ├── csv/                    ← intermediate CSV files
│   │       └── lmdb/                   ← 7 LMDB databases (active)
│   ├── lib/                            ← bundled shared libraries
│   ├── logs/                           ← service_TIMESTAMP.log files here
│   ├── uploads/                        ← debug output files (if enabled)
│   └── run.sh                          ← start script
├── address-service-build/              ← SOURCE + BUILD
│   ├── src/                            ← drop .cc/.h files here via WinSCP
│   │   ├── main.cc
│   │   ├── models/AddressModels.h
│   │   ├── utils/Logger.h
│   │   ├── services/
│   │   │   ├── AddressParser.cc/h
│   │   │   ├── GeoNamesLMDB.cc/h
│   │   │   ├── PreProcessor.cc/h
│   │   │   ├── RuleEngine.cc/h
│   │   │   ├── ConfidenceScorer.cc/h
│   │   │   ├── CacheManager.cc/h
│   │   │   ├── MetricsCollector.cc/h
│   │   │   └── LLMFallback.cc/h
│   │   └── controllers/
│   │       ├── ParseController.cc/h
│   │       ├── BatchController.cc/h
│   │       ├── EnrichController.cc/h
│   │       ├── GeoEnrichLMDBController.cc/h
│   │       ├── RefPostalController.cc/h
│   │       ├── RefCountryController.cc/h
│   │       ├── RefStateController.cc/h
│   │       ├── RefCityController.cc/h
│   │       ├── RefEnrichController.cc/h
│   │       ├── RefAbbreviationController.cc/h
│   │       ├── HealthController.cc/h
│   │       ├── MetricsController.cc/h
│   │       ├── AuthFilter.cc/h
│   │       ├── ApiKeyFilter.cc/h
│   │       └── JwtAuthFilter.cc/h
│   ├── third_party_libs/
│   │   ├── loguru.hpp
│   │   └── loguru.cpp
│   ├── config/config.json              ← template only — NEVER edit this
│   ├── CMakeLists.txt
│   └── build/                          ← cmake output, run make here
├── drogon/                             ← Drogon framework source (compiled once)
├── libpostal/                          ← libpostal source (compiled once)
└── vm-setup.sh                         ← initial setup script
```

---

## Config Files — Which One to Edit

| File | Purpose | Edit? |
|------|---------|-------|
| `address-service/config/config.json` | ✅ LIVE config — what the running service reads | **YES — only edit this one** |
| `address-service-build/config/config.json` | Source template — used only during initial setup | **NO — never touch** |

**Rule: for any config change (port, data_dir, batch size, cache etc) — only ever edit:**
```
/home/flureelabs/address-parser-service/address-service/config/config.json
```

---

## Current Live Config

```json
{
    "server": {
        "port": 8090,
        "threads": 0,
        "max_connections": 100000
    },
    "libpostal": {
        "data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal_senzing",
        "default_country": "",
        "default_language": ""
    },
    "geonames_lmdb": {
        "lmdb_dir": "/home/flureelabs/address-parser-service/address-service/data/geonames/lmdb"
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
    "metrics": {
        "enabled": true,
        "port": 9090
    },
    "security": {
        "enabled": false,
        "mode": "api_key",
        "api_key": "",
        "keys_file": "",
        "max_address_length": 500,
        "cache_min_confidence": 0.5
    },
    "jwt": {
        "auth_url": "http://localhost:3000/auth/jwt",
        "timeout_ms": 3000,
        "cache_ttl_seconds": 300,
        "cache_max_entries": 10000
    }
}
```

---

## How to Start the Service

### Option 1 — Foreground (good for debugging — Ctrl+C to stop)
```bash
cd /home/flureelabs/address-parser-service/address-service
bash run.sh
```
You see all logs live on the console. Press Ctrl+C to stop.

### Option 2 — Background with nohup (survives terminal disconnect)
```bash
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > /dev/null 2>&1 &
echo "PID: $!"
```
All output goes to `logs/service_TIMESTAMP.log` only.

### Option 3 — Systemd (auto-start on reboot, auto-restart on crash)
```bash
# Start
sudo systemctl start address-service

# Enable auto-start on reboot
sudo systemctl enable address-service
```

---

## How to Stop the Service

```bash
# Graceful stop
pkill -f "address-service"

# Force kill if graceful doesn't work
pkill -9 -f "address-service"
sleep 2

# Verify it's stopped
pgrep -a "address-service"
# Should return nothing
```

### Via systemd
```bash
sudo systemctl stop address-service
sudo systemctl disable address-service   # also disable auto-start
```

---

## How to Restart the Service

```bash
pkill -9 -f "address-service" 2>/dev/null
sleep 2
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > /dev/null 2>&1 &
echo "PID: $!"
```

---

## Check Service Status

```bash
# Is it running?
pgrep -a "address-service"

# Quick liveness check
curl -s http://localhost:8090/health/live

# Full stats (cache hits, latency, config, uptime)
curl -s http://localhost:8090/health/info | python3 -m json.tool

# Systemd status
sudo systemctl status address-service
```

---

## View Logs

```bash
# Find latest log file
ls -lt /home/flureelabs/address-parser-service/address-service/logs/

# Watch live — real-time updates
tail -f /home/flureelabs/address-parser-service/address-service/logs/service_*.log

# View full log from start (banner + init + requests)
cat /home/flureelabs/address-parser-service/address-service/logs/service_$(ls -t /home/flureelabs/address-parser-service/address-service/logs/ | head -1)

# Search for errors
grep -i "error\|warn\|fatal" \
  /home/flureelabs/address-parser-service/address-service/logs/service_*.log

# Search for specific endpoint
grep "enrich/geo/lmdb" \
  /home/flureelabs/address-parser-service/address-service/logs/service_*.log
```

### Log file format
Every line has: `timestamp | uptime | thread | file:line | level | message`
```
2026-04-03 10:36:51.163 (   0.001s) [main thread     ] main.cc:82   INFO| [main] Port: 8090
2026-04-03 10:39:51.435 ( 180.272s) [8E759700        ] GeoEnrichLMDBController:517 INFO| [enrich/geo/lmdb] records=2005 succeeded=1449 failed=556 geo_applied=149 latency=265.69ms
```

### Log rotation
- One file per service start — named `service_YYYYMMDD_HHMMSS.log`
- Max 20 files kept — when exceeded, oldest 5 are deleted automatically
- No separate `service.log` or `drogon.*.log` — everything in one file

---

## Expected Startup Log

```
2026-04-03 10:36:51.163 ... INFO| === Address Normalization Service starting ===
2026-04-03 10:36:51.163 ... INFO| Log file: ./logs/service_20260403_103651.log
2026-04-03 10:36:51.163 ... INFO| [main] Configuration loaded from .../config/config.json
2026-04-03 10:36:51.163 ... INFO| [main] port=8090 cache=1 geonames_lmdb='...' batch_max=10000
2026-04-03 10:36:51.163 ... INFO| ====================================================
2026-04-03 10:36:51.163 ... INFO|   Address Normalization Service v1.0.0
2026-04-03 10:36:51.163 ... INFO| ====================================================
2026-04-03 10:36:51.163 ... INFO|   Port:           8090
2026-04-03 10:36:51.163 ... INFO|   Worker threads: 3
2026-04-03 10:36:51.163 ... INFO|   Cache:          enabled (max 5000000 entries, TTL 86400s)
2026-04-03 10:36:51.163 ... INFO|   Rule engine:    enabled
2026-04-03 10:36:51.163 ... INFO|   LLM fallback:   disabled
2026-04-03 10:36:51.163 ... INFO|   Auth:           disabled
2026-04-03 10:36:51.163 ... INFO|   Batch max:      10000 addresses
2026-04-03 10:36:51.163 ... INFO|   libpostal data: .../data/libpostal_senzing
2026-04-03 10:36:51.163 ... INFO| ====================================================
2026-04-03 10:36:51.163 ... INFO|   Endpoints available:
2026-04-03 10:36:51.163 ... INFO|   POST  /api/v1/parse
2026-04-03 10:36:51.163 ... INFO|   POST  /api/v1/normalize
2026-04-03 10:36:51.163 ... INFO|   POST  /api/v1/batch   (max 10000 records)
2026-04-03 10:36:51.163 ... INFO|   POST  /api/v1/enrich  (max 10000 records)
2026-04-03 10:36:51.163 ... INFO|   POST  /api/v1/enrich/geo/lmdb (max 10000 records)
2026-04-03 10:36:51.163 ... INFO|   GET   /ref/v1/postal/{code}
2026-04-03 10:36:51.163 ... INFO|   POST  /ref/v1/postal/batch
2026-04-03 10:36:51.163 ... INFO|   GET   /ref/v1/country/{code}
2026-04-03 10:36:51.163 ... INFO|   GET   /ref/v1/state/{country}/{abbrev}
2026-04-03 10:36:51.163 ... INFO|   GET   /ref/v1/city/search
2026-04-03 10:36:51.163 ... INFO|   POST  /ref/v1/enrich
2026-04-03 10:36:51.163 ... INFO|   POST  /ref/v1/validate
2026-04-03 10:36:51.163 ... INFO|   GET   /ref/v1/postal/reverse
2026-04-03 10:36:51.163 ... INFO|   POST  /ref/v1/abbreviation/expand
2026-04-03 10:36:51.163 ... INFO|   GET   /health/live
2026-04-03 10:36:51.163 ... INFO|   GET   /health/ready
2026-04-03 10:36:51.163 ... INFO|   GET   /health/startup
2026-04-03 10:36:51.163 ... INFO|   GET   /health/info
2026-04-03 10:36:51.163 ... INFO|   GET   /metrics
2026-04-03 10:36:51.163 ... INFO| ====================================================
2026-04-03 10:36:51.163 ... INFO| [main] Initializing libpostal (this takes 15-30s)...
2026-04-03 10:36:57.620 ... INFO| [AddressParser] libpostal ready | data_dir=.../libpostal_senzing | parser only, no classifier
2026-04-03 10:36:57.620 ... INFO| [main] libpostal ready
2026-04-03 10:36:57.709 ... INFO| [main] language classifier ready
2026-04-03 10:36:57.709 ... INFO| [main] Cache initialized (max 5000000 entries)
2026-04-03 10:36:57.709 ... INFO| [main] Pre-processor, Rule engine, Confidence scorer ready
2026-04-03 10:36:57.709 ... INFO| [main] LLM fallback disabled — Phase 4 inactive
2026-04-03 10:36:57.710 ... INFO| [main] Initializing GeoNamesLMDB...
2026-04-03 10:36:57.710 ... INFO| [GeoNamesLMDB] Opening 7 LMDB databases from .../geonames/lmdb/
2026-04-03 10:36:57.710 ... INFO| [GeoNamesLMDB] Ready — admin1 + admin2 + postal + postal_reverse + cities + countries + aliases
2026-04-03 10:36:57.710 ... INFO| [main] GeoNamesLMDB ready | dir=.../geonames/lmdb
2026-04-03 10:36:57.710 ... INFO| [main] Authentication: DISABLED
2026-04-03 10:36:57.710 ... INFO| All services initialized, starting HTTP server...
```

---

## What to Run After Changes

### config.json only — no rebuild needed
```bash
nano /home/flureelabs/address-parser-service/address-service/config/config.json
# make your changes, save

pkill -9 -f "address-service" 2>/dev/null
sleep 2
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > /dev/null 2>&1 &
```

### .cc or .h files changed
```bash
# 1. Build
cd /home/flureelabs/address-parser-service/address-service-build/build
make -j$(nproc) && echo "Build: $?"

# 2. Deploy (kill first — binary cannot be overwritten while running)
pkill -9 -f "address-service" 2>/dev/null
sleep 2
cp /home/flureelabs/address-parser-service/address-service-build/build/address-service \
   /home/flureelabs/address-parser-service/address-service/bin/address-service

# 3. Start
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > /dev/null 2>&1 &
sleep 40
tail -20 /home/flureelabs/address-parser-service/address-service/logs/service_*.log
```

### CMakeLists.txt changed (new file added, new library)
```bash
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
make -j$(nproc) && echo "Build: $?"

pkill -9 -f "address-service" 2>/dev/null
sleep 2
cp /home/flureelabs/address-parser-service/address-service-build/build/address-service \
   /home/flureelabs/address-parser-service/address-service/bin/address-service
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > /dev/null 2>&1 &
```

### Quick reference table
| What changed | cmake3 needed | make needed | restart needed |
|---|---|---|---|
| `config.json` only | ❌ | ❌ | ✅ |
| `.cc` or `.h` file | ❌ | ✅ | ✅ |
| `.cc`/`.h` + `config.json` | ❌ | ✅ | ✅ |
| `CMakeLists.txt` | ✅ | ✅ | ✅ |
| New `.cc` file added | ✅ | ✅ | ✅ |
| `libpostal data_dir` in config | ❌ | ❌ | ✅ |
| `geonames lmdb_dir` in config | ❌ | ❌ | ✅ |
| `batch max_size` in config | ❌ | ❌ | ✅ |

---

## Switch Libpostal Model

One line change in config — no rebuild needed:

```bash
nano /home/flureelabs/address-parser-service/address-service/config/config.json
```

```json
"data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal"          ← standard
"data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal_senzing"  ← senzing (active)
```

Then restart.

---

## Change Max Records (batch_max_size)

Edit config only — no rebuild:

```bash
nano /home/flureelabs/address-parser-service/address-service/config/config.json
```

```json
"batch": {
    "max_size": 10000
}
```

All three endpoints (`/api/v1/batch`, `/api/v1/enrich`, `/api/v1/enrich/geo/lmdb`) read from this single value.

**Over-limit behaviour:** Records over the limit are silently discarded. A `warning` field is added to the response:
```json
{
  "warning": "Received 15000 records but maximum is 10000. Only first 10000 records processed. Remaining 5000 records were discarded.",
  "total_received": 15000,
  "total_processed": 10000,
  "total_discarded": 5000
}
```

---

## Systemd Service Management

```bash
# Start
sudo systemctl start address-service

# Stop
sudo systemctl stop address-service

# Restart
sudo systemctl restart address-service

# Status
sudo systemctl status address-service

# Live logs via journalctl
sudo journalctl -u address-service -f

# Enable auto-start on reboot
sudo systemctl enable address-service

# Disable auto-start
sudo systemctl disable address-service
```

---

## Rebuild LMDB Indexes (if GeoNames data updated)

Only needed if GeoNames source data is refreshed. The LMDB files are built on WSL and copied to VM.

### On WSL — rebuild CSVs (~60s)
```bash
python3 ~/FAISS-Actual-26June25/address-service-optimized/tools/build_csv.py \
    --data-dir ~/libpostal_data/geonames \
    --out-dir  ~/libpostal_data/geonames/csv
```

### On WSL — rebuild LMDB (~7s)
```bash
rm -rf ~/libpostal_data/geonames/lmdb/*
~/FAISS-Actual-26June25/address-service-optimized/tools/build_lmdb \
    --csv-dir  ~/libpostal_data/geonames/csv \
    --lmdb-dir ~/libpostal_data/geonames/lmdb
```

### Copy new LMDB to VM
Copy all 7 folders via WinSCP:
```
WSL:  ~/libpostal_data/geonames/lmdb/
VM:   /home/flureelabs/address-parser-service/address-service/data/geonames/lmdb/
```
Then restart the service — no rebuild needed.

---

## Geo Trigger Logic

Geo enrichment only fires when libpostal needs help:

| Condition | Geo triggers? |
|---|---|
| Field is blank | ✅ Always fill |
| Field exists + confidence < 0.7 | ✅ Override with LMDB result |
| Field exists + confidence ≥ 0.7 | ❌ Skip — trust libpostal |

Well-formed addresses like `123 MG Road Bengaluru 560001` will have `geo_applied=false` because libpostal already gets them right at high confidence.

---

## All Endpoints

| Method | Endpoint | Description |
|---|---|---|
| POST | `/api/v1/parse` | Parse single address |
| POST | `/api/v1/normalize` | libpostal expansions |
| POST | `/api/v1/batch` | Batch parse up to 10,000 |
| POST | `/api/v1/enrich` | Enrich JSON records |
| POST | `/api/v1/enrich/geo/lmdb` | Enrich + LMDB geo lookup (249 countries) |
| GET | `/ref/v1/postal/{code}` | Postal code → city + state |
| POST | `/ref/v1/postal/batch` | Batch postal lookup |
| GET | `/ref/v1/postal/reverse` | City → list of postal codes |
| GET | `/ref/v1/country/{code}` | Country code normalization |
| GET | `/ref/v1/state/{country}/{abbrev}` | State code → state name |
| GET | `/ref/v1/city/search` | City search + alias resolution |
| POST | `/ref/v1/enrich` | Fill missing address fields |
| POST | `/ref/v1/validate` | Validate address + detect conflicts |
| POST | `/ref/v1/abbreviation/expand` | Expand abbreviations in text |
| GET | `/health/live` | Liveness probe |
| GET | `/health/ready` | Readiness probe |
| GET | `/health/startup` | Startup probe |
| GET | `/health/info` | Full stats |
| GET | `/metrics` | Prometheus metrics |

---

## Common Issues

| Issue | Cause | Fix |
|---|---|---|
| `Text file busy` on cp | Old process still running | `pkill -9 -f "address-service"` then wait 2s then cp |
| `GeoNamesLMDB skipped` in log | Config has wrong key or empty `lmdb_dir` | Check `geonames_lmdb.lmdb_dir` in live config |
| `GeoNamesLMDB not initialized` | Service not loading LMDB | Check startup log for error, verify lmdb path exists |
| `curl: GLIBC version not found` | `LD_LIBRARY_PATH` set in shell from old run | Run `unset LD_LIBRARY_PATH` in current session |
| Empty log file | Service crashed before writing | Run `bash run.sh` in foreground to see error |
| Service not starting | Port 8090 already in use | `pkill -9 -f "address-service"` then retry |
| `error while loading shared libraries: liblmdb.so.0` | LMDB not installed | `sudo yum install -y lmdb-devel` |
| `geo_applied_count: 0` on real data | Normal for high-confidence addresses | Expected — geo only fires when libpostal confidence < 0.7 |
| LMDB databases missing | Copy incomplete | Verify all 7 folders exist with `data.mdb` inside each |
| Config stops parsing after `geonames: {}` | jsoncpp bug with empty object + sibling key | Remove empty `geonames: {}` block from config |
| Log not updating in open editor | Normal buffering | Use `tail -f logs/service_*.log` instead |

---

## Full Clean Rebuild (when something is broken)

```bash
pkill -9 -f "address-service" 2>/dev/null
sleep 2

cd /home/flureelabs/address-parser-service/address-service-build/build
rm -rf *

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

make -j$(nproc) && echo "Build: $?"

cp /home/flureelabs/address-parser-service/address-service-build/build/address-service \
   /home/flureelabs/address-parser-service/address-service/bin/address-service

cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > /dev/null 2>&1 &
sleep 40
tail -30 logs/service_*.log
```

---

## VM Environment Summary

| What | Value |
|------|-------|
| VM | AWS EC2, Amazon Linux 2 |
| Port | 8090 |
| User | flureelabs |
| Service dir | `/home/flureelabs/address-parser-service/address-service` |
| Source + build | `/home/flureelabs/address-parser-service/address-service-build` |
| Active model | Senzing v1.2 |
| Standard model data | `.../data/libpostal` |
| Senzing model data | `.../data/libpostal_senzing` |
| LMDB geo data | `.../data/geonames/lmdb` |
| libpostal source | `/home/flureelabs/address-parser-service/libpostal` |
| Drogon source | `/home/flureelabs/address-parser-service/drogon` |
| GCC | gcc10 (`/usr/bin/gcc10-gcc`, `/usr/bin/gcc10-g++`) |
| LMDB version | 0.9.22 (installed via yum) |
