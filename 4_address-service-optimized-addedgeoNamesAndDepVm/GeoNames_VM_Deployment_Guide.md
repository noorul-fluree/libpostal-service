# GeoNames VM Deployment Guide
## Address Normalization Service — Phase 2 Geo Lookup on Amazon Linux 2

---

## What Was Deployed

The `/api/v1/enrich/geo` endpoint adds a **Phase 2 GeoNames geo lookup layer** on top of libpostal parsing. It fills in missing or low-confidence city/state fields using GeoNames postal and city databases.

**Countries supported:** India (IN), United States (US), United Kingdom (GB)

---

## VM Environment

| What | Value |
|------|-------|
| VM | AWS EC2, Amazon Linux 2 |
| Public IP | 100.31.167.228 |
| Port | 8090 |
| Service dir | /home/flureelabs/address-parser-service/address-service |
| GeoNames data | /home/flureelabs/address-parser-service/address-service/data/geonames |

---

## Deployment Steps (What We Did)

### Step 1 — Copy source files via WinSCP
From WSL → VM `address-service-build/src/`:

| File | Destination |
|------|-------------|
| `src/services/GeoNamesDB.h` | `address-service-build/src/services/` |
| `src/services/GeoNamesDB.cc` | `address-service-build/src/services/` |
| `src/controllers/GeoEnrichController.h` | `address-service-build/src/controllers/` |
| `src/controllers/GeoEnrichController.cc` | `address-service-build/src/controllers/` |
| `src/main.cc` | `address-service-build/src/` |
| `src/models/AddressModels.h` | `address-service-build/src/models/` |

### Step 2 — Copy GeoNames data files via WinSCP
From WSL `~/libpostal_data/geonames/` → VM `/home/flureelabs/address-parser-service/address-service/data/geonames/`:
- `admin1CodesASCII.txt` (~148KB)
- `cities15000.txt` (~7.5MB)
- `postal_codes.txt` (~135MB)

### Step 3 — Add GeoNames sources to VM CMakeLists.txt
```bash
# Verify new files are in SOURCES block
grep "GeoNames\|GeoEnrich" \
    /home/flureelabs/address-parser-service/address-service-build/CMakeLists.txt
```
Should show:
```
src/services/GeoNamesDB.cc
src/controllers/GeoEnrichController.cc
```

### Step 4 — Update live config.json
```bash
nano /home/flureelabs/address-parser-service/address-service/config/config.json
```
Add after `libpostal` block (spaces only, no tabs):
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
```

### Step 6 — Deploy and restart
```bash
cp /home/flureelabs/address-parser-service/address-service-build/build/address-service \
   /home/flureelabs/address-parser-service/address-service/bin/
pkill -f "address-service"
cd /home/flureelabs/address-parser-service/address-service
nohup bash run.sh > logs/service.log 2>&1 &
sleep 40
tail -20 logs/service.log
```

### Step 7 — Verify startup log
```
[GeoNamesDB] admin1 codes loaded: 91
[GeoNamesDB] postal codes loaded: 63728
[GeoNamesDB] cities loaded: 7485
[GeoNamesDB] Ready | postal_entries=63728 | city_entries=7485
[main] GeoNamesDB ready (63728 postal, 7485 city entries)
```

---

## Common Issues We Hit

| Issue | Cause | Fix |
|-------|-------|-----|
| `ARROW_LIB-NOTFOUND` linker error | WSL CMakeLists.txt copied to VM | `sed -i '/ARROW/d'` and `sed -i '/arrow/d'` on CMakeLists.txt |
| `set_target_properties` illegal arguments | Arrow RPATH lines removed but block remained empty | Remove the entire empty block with sed |
| `cannot find -lpostal` | libpostal in `/usr/local/lib` not found by linker | Add `find_library(LIBPOSTAL_LIB postal HINTS /usr/local/lib)` |
| `GeoNamesDB skipped` in startup | Tab/space mix in config.json broke JSON parsing | Rewrite config.json with consistent spaces only |
| `cities15000.txt not found` | Wrong file copied (allCountries.txt instead) | Copy correct file from WSL geonames folder |

---

## Test Results — VM Verified ✅

### Test 1 — Geo triggers on sparse UK addresses (cold cache)

```bash
curl -s -X POST http://localhost:8090/api/v1/enrich/geo \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"id": 1, "addr": "M1 1AE, Manchester"},
      {"id": 2, "addr": "EC1A 1BB"},
      {"id": 3, "addr": "10118, New York"}
    ],
    "metadata": {
      "address_column": "addr",
      "normalize_columns": "ALL"
    }
  }' | python3 -m json.tool
```

**Response:**
```json
{
    "geo_applied_count": 3,
    "geo_db_ready": true,
    "total": 3,
    "succeeded": 3,
    "results": [
        {
            "id": 1, "addr": "M1 1AE, Manchester",
            "city": "manchester", "state": "england",
            "postcode": "m1 1ae",
            "confidence": 0.598,
            "geo_applied": true, "geo_source": "postal_2a",
            "normalize_addr": "manchester england m1 1ae"
        },
        {
            "id": 2, "addr": "EC1A 1BB",
            "city": "london", "state": "england",
            "postcode": "ec1a 1bb",
            "confidence": 0.675,
            "geo_applied": true, "geo_source": "postal_2a",
            "normalize_addr": "london england ec1a 1bb"
        },
        {
            "id": 3, "addr": "10118, New York",
            "city": "new york", "state": "new york",
            "country": "united states", "postcode": "10118",
            "confidence": 0.688,
            "geo_applied": true, "geo_source": "postal_2a",
            "normalize_addr": "new york new york 10118 united states"
        }
    ]
}
```

**What this proves:**
- `geo_applied: true` on all 3 — GeoNames filled city+state from postal code alone
- `geo_source: postal_2a` — Phase 2A postal lookup triggered
- UK postcodes auto-detected as GB even without "UK" or "England" in the address
- `EC1A 1BB` had no city at all — geo filled it completely

---

### Test 2 — Geo correctly skips when libpostal is confident (warm cache)

```bash
curl -s -X POST http://localhost:8090/api/v1/enrich/geo \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"id": 1, "addr": "123 MG Road, Bengaluru 560001"},
      {"id": 2, "addr": "350 Fifth Ave, New York NY 10118"}
    ],
    "metadata": {
      "address_column": "addr",
      "normalize_columns": "ALL"
    }
  }' | python3 -m json.tool
```

**Response:**
```json
{
    "geo_applied_count": 0,
    "results": [
        {
            "id": 1,
            "city": "bengaluru", "state": "karnataka",
            "postcode": "560001", "country": "india",
            "confidence": 0.853,
            "geo_applied": false, "geo_source": "none",
            "normalize_addr": "123 mahatma gandhi road bengaluru karnataka 560001 india"
        },
        {
            "id": 2,
            "city": "new york", "state": "new york",
            "postcode": "10118", "country": "united states",
            "confidence": 0.95,
            "geo_applied": false, "geo_source": "none",
            "normalize_addr": "350 fifth avenue new york new york 10118 united states"
        }
    ]
}
```

**What this proves:**
- `geo_applied: false` — libpostal confidence ≥ 0.7, geo correctly stepped aside
- `geo_source: none` — no override needed, results already accurate
- GeoNames is a fallback, not a replacement

---

## How geo_applied and geo_source Work

| `geo_source` | Meaning |
|---|---|
| `postal_2a` | Phase 2A triggered — postal code → city + state lookup |
| `city_2b` | Phase 2B triggered — city name → state lookup only |
| `none` | Geo lookup not needed or country not supported |

| `geo_applied` | Meaning |
|---|---|
| `true` | Geo lookup changed at least one field (city or state) |
| `false` | Libpostal was confident enough OR geo found nothing new |

**Trigger rule:** geo overrides a field only if:
- The field is blank, OR
- Libpostal confidence < 0.7

---

## Switching Models With GeoNames

GeoNames works with **both** libpostal models. Just change `data_dir` in config — geo lookup is independent:

```json
"libpostal": {
    "data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal"
}
```
or:
```json
"libpostal": {
    "data_dir": "/home/flureelabs/address-parser-service/address-service/data/libpostal_senzing"
}
```

Then restart — GeoNames data stays the same either way.
