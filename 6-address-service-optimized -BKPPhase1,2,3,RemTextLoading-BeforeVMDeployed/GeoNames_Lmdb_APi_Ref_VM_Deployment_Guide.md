# GeoNames LMDB — VM Deployment Guide
## Address Normalization Service — Phase 2 Geo Lookup on Amazon Linux 2

---

## Overview

The LMDB geo lookup layer adds Phase 2 enrichment on top of libpostal:

| Phase | What | LMDB database |
|---|---|---|
| 2A | Postcode → city + state | `postal.lmdb` |
| 2B | City → state | `cities.lmdb` |
| 2C | City alias resolution (Bombay→Mumbai) | `aliases.lmdb` |
| 2D | Country name fill from country code | `countries.lmdb` |

**Coverage:** 249 countries, ~5MB RAM at runtime, <1ms lookup per record.

---

## LMDB Databases

| Database | Key format | Value | Size |
|---|---|---|---|
| `postal.lmdb` | `IN:560001` | `bengaluru\|karnataka` | ~3.7MB |
| `postal_reverse.lmdb` | `IN:bengaluru` | `560001,560002,...` | ~1.2MB |
| `cities.lmdb` | `IN:bengaluru` | `karnataka` | ~41MB |
| `countries.lmdb` | `IN` / `IND` / `356` / `india` | `india\|IN\|IND\|356\|asia\|southern asia` | ~0.1MB |
| `admin1.lmdb` | `IN:19` | `karnataka` | ~0.3MB |
| `admin2.lmdb` | `IN:19:1234` | `bengaluru urban` | ~1.5MB |
| `aliases.lmdb` | `IN:bombay` | `mumbai` | ~0.1MB |

---

## VM Data Location

```
/home/flureelabs/address-parser-service/address-service/data/geonames/
├── csv/                          ← intermediate CSV files (built from GeoNames txt)
│   ├── admin1_filtered.csv       — 3,862 rows
│   ├── admin2_filtered.csv       — 47,496 rows
│   ├── postal_filtered.csv       — 1,080,414 rows
│   ├── cities_filtered.csv       — 3,220,878 rows
│   ├── countries_filtered.csv    — 249 rows
│   └── city_aliases_filtered.csv — 2,661 rows
└── lmdb/                         ← 7 LMDB databases (what the service reads)
    ├── admin1.lmdb/
    ├── admin2.lmdb/
    ├── postal.lmdb/
    ├── postal_reverse.lmdb/
    ├── cities.lmdb/
    ├── countries.lmdb/
    └── aliases.lmdb/
```

---

## Initial Deployment — Copying LMDB Data to VM

The LMDB databases are built on WSL and copied to VM via WinSCP.

### Source (WSL)
```
~/libpostal_data/geonames/lmdb/
├── admin1.lmdb/         (data.mdb + lock.mdb)
├── admin2.lmdb/         (data.mdb + lock.mdb)
├── postal.lmdb/         (data.mdb + lock.mdb)
├── postal_reverse.lmdb/ (data.mdb + lock.mdb)
├── cities.lmdb/         (data.mdb + lock.mdb)
├── countries.lmdb/      (data.mdb + lock.mdb)
└── aliases.lmdb/        (data.mdb + lock.mdb)
```

### Destination (VM)
```
/home/flureelabs/address-parser-service/address-service/data/geonames/lmdb/
```

Copy all 7 folders with their contents via WinSCP.

### Verify on VM after copy
```bash
ls /home/flureelabs/address-parser-service/address-service/data/geonames/lmdb/
# Should show: admin1.lmdb  admin2.lmdb  postal.lmdb  postal_reverse.lmdb
#              cities.lmdb  countries.lmdb  aliases.lmdb

ls -lh /home/flureelabs/address-parser-service/address-service/data/geonames/lmdb/postal.lmdb/
# data.mdb  (~3.7MB)   lock.mdb  (~8KB)

ls -lh /home/flureelabs/address-parser-service/address-service/data/geonames/lmdb/cities.lmdb/
# data.mdb  (~41MB)    lock.mdb  (~8KB)
```

---

## Config — Pointing to LMDB Data

In the **live config** only:
```bash
nano /home/flureelabs/address-parser-service/address-service/config/config.json
```

Required section:
```json
"geonames_lmdb": {
    "lmdb_dir": "/home/flureelabs/address-parser-service/address-service/data/geonames/lmdb"
}
```

**Important:** The key must be `geonames_lmdb` at the top level — not nested inside `geonames`. An empty `geonames: {}` block causes jsoncpp to stop parsing on Amazon Linux 2.

---

## Install LMDB on VM (one-time)

```bash
sudo amazon-linux-extras install epel -y
sudo yum install -y lmdb-devel
echo "Install: $?"

# Verify
find /usr -name "lmdb.h" 2>/dev/null
# Expected: /usr/include/lmdb.h
```

---

## Build with LMDB Support

The `CMakeLists.txt` must have these entries. Verify:

```bash
grep -n "GeoNamesLMDB\|lmdb\|third_party\|loguru" \
  /home/flureelabs/address-parser-service/address-service-build/CMakeLists.txt
```

Required SOURCES entries:
```cmake
src/controllers/GeoEnrichLMDBController.cc
src/controllers/RefPostalController.cc
src/controllers/RefCountryController.cc
src/controllers/RefStateController.cc
src/controllers/RefCityController.cc
src/controllers/RefEnrichController.cc
src/controllers/RefAbbreviationController.cc
src/services/GeoNamesLMDB.cc
third_party_libs/loguru.cpp
```

Required in `target_link_libraries`:
```cmake
lmdb
```

Required in `target_include_directories`:
```cmake
${CMAKE_SOURCE_DIR}/third_party_libs
```

---

## Verify LMDB is Loading at Startup

After starting the service, check the log:
```bash
grep -i "geonames\|lmdb" \
  /home/flureelabs/address-parser-service/address-service/logs/service_*.log | head -10
```

Expected output:
```
INFO| [main] Initializing GeoNamesLMDB...
INFO| [GeoNamesLMDB] Opening 7 LMDB databases from .../geonames/lmdb/
INFO| [GeoNamesLMDB] Ready — admin1 + admin2 + postal + postal_reverse + cities + countries + aliases
INFO| [main] GeoNamesLMDB ready | dir=.../geonames/lmdb
```

If you see `GeoNamesLMDB skipped — lmdb_dir not set` — the config key is wrong. Check that `geonames_lmdb` is a top-level key and not nested.

---

## Test Endpoints After Deployment

```bash
# Test ref/validate — postcode + city validation
curl -s -X POST http://localhost:8090/ref/v1/validate \
  -H "Content-Type: application/json" \
  -d '{"postcode":"560001","city":"Bengaluru","country_code":"IN"}' | python3 -m json.tool

# Test ref/enrich — fill missing fields
curl -s -X POST http://localhost:8090/ref/v1/enrich \
  -H "Content-Type: application/json" \
  -d '{"postcode":"10001","city":"New York","country_code":"US"}' | python3 -m json.tool

# Test enrich/geo/lmdb with small batch
python3 -c "
import json
data = [{'id': i, 'addr': f'{i} MG Road Bengaluru 560001'} for i in range(10)]
payload = {'data': data, 'metadata': {'address_column': 'addr', 'key_column': 'id', 'normalize_columns': 'ALL'}}
print(json.dumps(payload))
" | curl -s -X POST http://localhost:8090/api/v1/enrich/geo/lmdb \
  -H "Content-Type: application/json" \
  -d @- | python3 -c "
import json,sys
r=json.load(sys.stdin)
print('total:', r.get('total'))
print('succeeded:', r.get('succeeded'))
print('geo_applied:', r.get('geo_applied_count'))
print('latency_ms:', r.get('total_latency_ms'))
"

# Test ref/postal lookup
curl -s "http://localhost:8090/ref/v1/postal/560001?country=IN" | python3 -m json.tool

# Test ref/postal/reverse
curl -s "http://localhost:8090/ref/v1/postal/reverse?city=bengaluru&country=IN" | python3 -m json.tool

# Test ref/country
curl -s "http://localhost:8090/ref/v1/country/IN" | python3 -m json.tool

# Test ref/city/search with alias
curl -s "http://localhost:8090/ref/v1/city/search?q=bombay&country=IN" | python3 -m json.tool
```

---

## Expected Test Results

### ref/validate
```json
{
    "city": "Bengaluru",
    "confidence_boost": 0.15,
    "conflicts": [],
    "country": "india",
    "country_code": "IN",
    "enriched_fields": ["state", "country"],
    "geo_source": "postal_2a",
    "postcode": "560001",
    "state": "karnataka",
    "valid": true,
    "validation_notes": [
        "postal area name 'bangalore g.p.o.' differs from city 'bengaluru' but state matches - accepted",
        "postcode matches city and state"
    ]
}
```

### ref/enrich (US)
```json
{
    "city": "new york city",
    "confidence_boost": 0.15,
    "country": "united states of america",
    "country_code": "US",
    "enriched_fields": ["state", "country", "city_normalized"],
    "geo_source": "postal_2a",
    "postcode": "10001",
    "state": "new york"
}
```

### enrich/geo/lmdb batch
```json
{
    "total": 10,
    "succeeded": 10,
    "failed": 0,
    "geo_applied_count": 0,
    "total_latency_ms": 20.5
}
```

---

## Geo Trigger Logic

Geo enrichment only fires when libpostal needs help:

| Condition | Geo triggers? |
|---|---|
| Field is blank | ✅ Always fill |
| Field exists + confidence < 0.7 | ✅ Override with LMDB result |
| Field exists + confidence ≥ 0.7 | ❌ Skip — trust libpostal |

This means well-formed addresses like `123 MG Road Bengaluru 560001` will have `geo_applied=false` because libpostal already gets them right at high confidence.

---

## Rebuild LMDB Indexes (if needed)

Only needed if GeoNames source data is updated. The LMDB files are built on WSL and copied to VM.

### On WSL — rebuild CSVs
```bash
python3 ~/FAISS-Actual-26June25/address-service-optimized/tools/build_csv.py \
    --data-dir ~/libpostal_data/geonames \
    --out-dir  ~/libpostal_data/geonames/csv
```

### On WSL — rebuild LMDB
```bash
rm -rf ~/libpostal_data/geonames/lmdb/*
~/FAISS-Actual-26June25/address-service-optimized/tools/build_lmdb \
    --csv-dir  ~/libpostal_data/geonames/csv \
    --lmdb-dir ~/libpostal_data/geonames/lmdb
```

### Copy new LMDB to VM
Copy all 7 folders via WinSCP from WSL to VM, then restart service.

---

## All LMDB-Powered Endpoints

| Endpoint | LMDB databases used |
|---|---|
| `POST /api/v1/enrich/geo/lmdb` | postal, cities, aliases, countries |
| `GET /ref/v1/postal/{code}` | postal |
| `POST /ref/v1/postal/batch` | postal |
| `GET /ref/v1/postal/reverse` | postal_reverse, aliases |
| `GET /ref/v1/country/{code}` | countries |
| `GET /ref/v1/state/{country}/{abbrev}` | admin1 |
| `GET /ref/v1/city/search` | cities, aliases |
| `POST /ref/v1/enrich` | postal, cities, aliases, countries |
| `POST /ref/v1/validate` | postal, cities, aliases, countries |

## All Service Endpoints

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
| `GeoNamesLMDB skipped — lmdb_dir not set` | Config key wrong or empty | Ensure `geonames_lmdb.lmdb_dir` is set correctly as top-level key |
| `GeoNamesLMDB not initialized` | Service not loading LMDB | Check startup log for error message, verify lmdb path exists |
| `error while loading shared libraries: liblmdb.so.0` | LMDB runtime not installed | `sudo yum install -y lmdb` |
| `geo_applied_count: 0` on real data | Normal for high-confidence addresses | Expected — geo only fires when libpostal confidence < 0.7 |
| LMDB databases missing | Copy incomplete | Verify all 7 folders exist with `data.mdb` inside each |
| Config stops parsing after `geonames: {}` | jsoncpp bug with empty object + sibling key | Remove empty `geonames: {}` block from config |
