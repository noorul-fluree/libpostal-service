# GeoNames VM Deployment Guide
## Address Normalization Service — Phase 2 Geo Lookup on Amazon Linux 2

---

## VM Environment

| What | Value |
|------|-------|
| VM | AWS EC2, Amazon Linux 2 |
| Public IP | 100.31.167.228 |
| Port | 8090 |
| Service dir | `/home/flureelabs/address-parser-service/address-service` |
| GeoNames data | `/home/flureelabs/address-parser-service/address-service/data/geonames` |
| Active model | Senzing v1.2 |

---

## One-Time Deployment Steps

### Step 1 — Copy source files via WinSCP
From WSL → VM `address-service-build/src/`:

| WSL file | VM destination |
|---|---|
| `src/services/GeoNamesDB.h` | `address-service-build/src/services/` |
| `src/services/GeoNamesDB.cc` | `address-service-build/src/services/` |
| `src/controllers/GeoEnrichController.h` | `address-service-build/src/controllers/` |
| `src/controllers/GeoEnrichController.cc` | `address-service-build/src/controllers/` |
| `src/main.cc` | `address-service-build/src/` |
| `src/models/AddressModels.h` | `address-service-build/src/models/` |

### Step 2 — Copy GeoNames data files via WinSCP
From WSL `~/libpostal_data/geonames/` → VM `address-service/data/geonames/`:
- `admin1CodesASCII.txt`
- `cities15000.txt`
- `postal_codes.txt`

### Step 3 — Add GeoNames sources to VM CMakeLists.txt
The VM CMakeLists.txt must have these two lines in the SOURCES block:
```cmake
src/services/GeoNamesDB.cc
src/controllers/GeoEnrichController.cc
```

Verify:
```bash
grep "GeoNames\|GeoEnrich" \
    /home/flureelabs/address-parser-service/address-service-build/CMakeLists.txt
```

### Step 4 — Update live config.json (spaces only — no tabs)
```bash
nano /home/flureelabs/address-parser-service/address-service/config/config.json
```

Add after `libpostal` block:
```json
"geonames": {
    "data_dir": "/home/flureelabs/address-parser-service/address-service/data/geonames"
},
```

### Step 5 — Rebuild on VM
```bash
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
make -j$(nproc)
echo "Build done: $?"
```

### Step 6 — Deploy binary and restart
```bash
pkill -9 -f "address-service"
sleep 2
rm /home/flureelabs/address-parser-service/address-service/bin/address-service
cp /home/flureelabs/address-parser-service/address-service-build/build/address-service \
   /home/flureelabs/address-parser-service/address-service/bin/
chmod +x /home/flureelabs/address-parser-service/address-service/bin/address-service

cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
```

---

## How to Run, Stop, Check

### Run in background (production)
```bash
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
```

### Run in foreground (debugging — Ctrl+C to stop)
```bash
cd /home/flureelabs/address-parser-service/address-service
bash run.sh
```

### Stop
```bash
pkill -f "address-service"
# If binary is busy:
pkill -9 -f "address-service"
sleep 2
```

### Check if running
```bash
pgrep -a "address-service"
curl -s http://localhost:8090/health/live
```

### Check logs
```bash
# Live tail
tail -f /home/flureelabs/address-parser-service/address-service/logs/service.log

# Full log (shows banner + GeoNamesDB load)
cat /home/flureelabs/address-parser-service/address-service/logs/service.log
```

### Check from Postman
```
GET http://100.31.167.228:8090/health/live
GET http://100.31.167.228:8090/health/info
```

---

## Expected Startup Banner (VM)

```
[main] Configuration loaded from ...config/config.json
====================================================
  Address Normalization Service v1.0.0
====================================================
  Port:           8090
  Worker threads: 3
  Cache:          enabled (max 5000000 entries, TTL 86400s)
  Rule engine:    enabled
  LLM fallback:   disabled
  Auth:           disabled
  Max addr len:   500 chars
  Batch max:      10000 addresses
  libpostal data: .../data/libpostal_senzing
====================================================
  Endpoints available:
  ┌─ POST  http://0.0.0.0:8090/api/v1/parse
  ├─ POST  http://0.0.0.0:8090/api/v1/normalize
  ├─ POST  http://0.0.0.0:8090/api/v1/batch   (max 10000 records)
  ├─ POST  http://0.0.0.0:8090/api/v1/enrich  (max 10000 records)
  ├─ POST  http://0.0.0.0:8090/api/v1/enrich/geo (max 10000 records)
  ├─ GET   http://0.0.0.0:8090/health/live
  ├─ GET   http://0.0.0.0:8090/health/ready
  ├─ GET   http://0.0.0.0:8090/health/startup
  ├─ GET   http://0.0.0.0:8090/health/info
  └─ GET   http://0.0.0.0:8090/metrics
====================================================
[main] Initializing libpostal (this takes 15-30s)...
[AddressParser] libpostal ready | data_dir=.../libpostal_senzing
[main] libpostal ready
[AddressParser] language classifier ready
[main] Cache initialized (max 5000000 entries)
[main] Initializing GeoNamesDB...
[GeoNamesDB] admin1 codes loaded: 91
[GeoNamesDB] postal codes loaded: 63728
[GeoNamesDB] cities loaded: 7485
[GeoNamesDB] Ready | postal_entries=63728 | city_entries=7485
[main] GeoNamesDB ready (63728 postal, 7485 city entries)
[main] Authentication: DISABLED
[main] All services initialized, starting HTTP server...
[main] Server listening on 0.0.0.0:8090 with 3 worker threads
```

---

## What to Run After Changes

| What changed | Steps |
|---|---|
| `config.json` only | restart only — `pkill` then `nohup bash run.sh` |
| `.cc` or `.h` files | `make -j$(nproc)` → `rm + cp binary` → restart |
| `CMakeLists.txt` | `cmake3 ..` → `make` → `rm + cp binary` → restart |

### After .cc/.h change (most common)
```bash
# 1. Build
cd /home/flureelabs/address-parser-service/address-service-build/build
make -j$(nproc)

# 2. Deploy
pkill -9 -f "address-service"
sleep 2
rm /home/flureelabs/address-parser-service/address-service/bin/address-service
cp address-service \
   /home/flureelabs/address-parser-service/address-service/bin/

# 3. Start
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
sleep 40
tail -20 logs/service.log
```

### After config.json change
```bash
pkill -f "address-service"
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
sleep 40
tail -20 logs/service.log
```

---

## Switch Libpostal Model

One line in config — GeoNames unchanged:

```bash
nano /home/flureelabs/address-parser-service/address-service/config/config.json
```

```json
"data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal"         ← standard
"data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal_senzing" ← senzing
```

Then restart.

---

## Common Issues

| Issue | Cause | Fix |
|---|---|---|
| `Text file busy` on cp | Old process still running | `pkill -9 -f "address-service"` then `rm` then `cp` |
| Empty log file | Service crashed before writing | Run `bash run.sh` in foreground to see error |
| `GeoNamesDB skipped` | Tab/space mix in config.json | Rewrite config with spaces only using `cat > config.json << 'EOF'` |
| `cities15000.txt not found` | Wrong file copied | Copy `cities15000.txt` specifically, not `allCountries.txt` |
| `ARROW_LIB-NOTFOUND` | WSL CMakeLists.txt copied | Run `sed -i '/ARROW/d'` and `sed -i '/arrow/d'` on CMakeLists.txt |
| `cannot find -lpostal` | libpostal not in linker path | Add `find_library(LIBPOSTAL_LIB postal HINTS /usr/local/lib)` |

---

## Verified Test Results on VM ✅

### Test 1 — Geo triggers on sparse UK + US addresses

```bash
curl -s -X POST http://localhost:8090/api/v1/enrich/geo \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"id": 1, "addr": "M1 1AE, Manchester"},
      {"id": 2, "addr": "EC1A 1BB"},
      {"id": 3, "addr": "10118, New York"}
    ],
    "metadata": {"address_column": "addr", "normalize_columns": "ALL"}
  }' | python3 -m json.tool
```

**Response:**
```json
{
    "geo_applied_count": 3,
    "geo_db_ready": true,
    "total": 3, "succeeded": 3,
    "total_latency_ms": 0.695,
    "results": [
        {
            "id": 1, "addr": "M1 1AE, Manchester",
            "city": "manchester", "state": "england",
            "postcode": "m1 1ae", "confidence": 0.598,
            "from_cache": false,
            "geo_applied": true, "geo_source": "postal_2a",
            "normalize_addr": "manchester england m1 1ae"
        },
        {
            "id": 2, "addr": "EC1A 1BB",
            "city": "london", "state": "england",
            "postcode": "ec1a 1bb", "confidence": 0.675,
            "from_cache": false,
            "geo_applied": true, "geo_source": "postal_2a",
            "normalize_addr": "london england ec1a 1bb"
        },
        {
            "id": 3, "addr": "10118, New York",
            "city": "new york", "state": "new york",
            "country": "united states", "postcode": "10118",
            "confidence": 0.688,
            "from_cache": false,
            "geo_applied": true, "geo_source": "postal_2a",
            "normalize_addr": "new york new york 10118 united states"
        }
    ]
}
```

**What this proves:**
- `geo_applied_count: 3` — all 3 records enriched by GeoNames
- `EC1A 1BB` had no city — geo filled it completely from postcode alone
- UK postcodes auto-detected as GB without "UK" in address
- `geo_source: postal_2a` — Phase 2A triggered on all

---

### Test 2 — Geo correctly skips when libpostal is confident

```bash
curl -s -X POST http://localhost:8090/api/v1/enrich/geo \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"id": 1, "addr": "123 MG Road, Bengaluru 560001"},
      {"id": 2, "addr": "560001, India"},
      {"id": 3, "addr": "400001"}
    ],
    "metadata": {"address_column": "addr", "normalize_columns": "ALL"}
  }' | python3 -m json.tool
```

**Response:**
```json
{
    "geo_applied_count": 0,
    "geo_db_ready": true,
    "total": 3, "succeeded": 3,
    "results": [
        {
            "id": 1,
            "city": "bengaluru", "state": "karnataka",
            "postcode": "560001", "country": "india",
            "confidence": 0.853,
            "from_cache": false,
            "geo_applied": false, "geo_source": "none",
            "normalize_addr": "123 mahatma gandhi road bengaluru karnataka 560001 india"
        },
        {
            "id": 2,
            "city": "bengaluru", "state": "karnataka",
            "postcode": "560001", "country": "india",
            "confidence": 0.743,
            "from_cache": true,
            "geo_applied": false, "geo_source": "none"
        },
        {
            "id": 3,
            "city": "mumbai", "state": "maharashtra",
            "postcode": "400001", "country": "india",
            "confidence": 0.794,
            "from_cache": false,
            "geo_applied": false, "geo_source": "none",
            "normalize_addr": "mumbai maharashtra 400001 india"
        }
    ]
}
```

**What this proves:**
- `geo_applied_count: 0` — libpostal confidence ≥ 0.7 on all, geo correctly skipped
- `from_cache: true` on record 2 — same postcode seen before, served from cache
- GeoNames is a fallback — it doesn't interfere when libpostal is doing well

---

## Service Log — Request Tracking

Every request is logged with timing:
```
[GeoEnrichController] ▶ POST /api/v1/enrich/geo | records_in=3 | processing=3 | geo_db=ready
[GeoEnrichController] ✔ POST /api/v1/enrich/geo | records_in=3 | succeeded=3 | failed=0 | geo_applied=3 | latency=0.695ms
```
