# Address Normalization Service v1.0.0
## Project Summary & Run Guide

---

## What This Service Does

A high-performance C++ HTTP service that takes raw address strings and returns:
- Structured parsed components (house number, road, city, state, postcode, country)
- Confidence score (0.0 – 1.0)
- Libpostal normalized expansions
- Full passthrough of original fields

**Tech stack:** C++17 · Drogon · libpostal · Apache Arrow · JsonCpp · OpenMP · mimalloc

---

## Project Structure

```
address-service-optimized/
├── CMakeLists.txt
├── config/config.json
├── src/
│   ├── main.cc                          ← startup, global services, Drogon config
│   ├── models/
│   │   └── AddressModels.h              ← ParsedAddress, BatchRequest/Response, ServiceConfig, ScopedTimer
│   ├── services/
│   │   ├── AddressParser.cc/h           ← libpostal wrapper (thread-safe, 16-slot mutex pool)
│   │   ├── PreProcessor.cc/h            ← single-pass clean + abbreviation expansion
│   │   ├── RuleEngine.cc/h              ← PIN/ZIP inference, state normalization, misspellings
│   │   ├── ConfidenceScorer.cc/h        ← completeness + postcode + cross-field + token coverage
│   │   ├── CacheManager.cc/h            ← 64-shard LRU cache (FNV-1a hash, TTL, LRU eviction)
│   │   ├── MetricsCollector.cc/h        ← lock-free atomic counters + Prometheus histograms
│   │   └── LLMFallback.cc/h             ← Phase 4 llama.cpp (disabled by default)
│   ├── controllers/
│   │   ├── ParseController.cc/h         ← POST /api/v1/parse, POST /api/v1/normalize
│   │   ├── BatchController.cc/h         ← POST /api/v1/batch (dedup + parallel)
│   │   ├── EnrichController.cc/h        ← POST /api/v1/enrich (JSON records enrichment)
│   │   ├── HealthController.cc/h        ← GET /health/live|ready|startup|info
│   │   ├── MetricsController.cc/h       ← GET /metrics (Prometheus format)
│   │   ├── AuthFilter.cc/h              ← unified auth dispatcher
│   │   ├── ApiKeyFilter.cc/h            ← API key validation (header / Bearer / query param)
│   │   └── JwtAuthFilter.cc/h           ← JWT validation (sharded token cache, calls Node.js)
│   └── utils/
│       └── InputValidator.h             ← length, UTF-8, null byte, control char checks
├── tests/
└── uploads/                             ← debug_results.json / debug_results.csv written here
```

---

## Pipeline (per address)

```
Raw input
  → InputValidator      (length, UTF-8, null bytes)
  → PreProcessor        (lowercase, collapse whitespace, expand abbreviations)
  → CacheManager.get()  (skip pipeline if cache hit)
  → AddressParser       (libpostal parse_address with language/country hints)
  → ConfidenceScorer    (completeness + postcode + cross-field + token coverage)
  → RuleEngine          (PIN→state/city, ZIP→country, misspellings, state normalize)
  → LLMFallback         (Phase 4 — disabled, set llm_enabled=true to activate)
  → CacheManager.put()  (store if confidence >= cache_min_confidence)
  → Response JSON
```

---

## All Endpoints

| Method | URL | Description |
|--------|-----|-------------|
| POST | `/api/v1/parse` | Parse a single address |
| POST | `/api/v1/normalize` | Libpostal expansions for a single address |
| POST | `/api/v1/batch` | Parse up to 2000 addresses (dedup + parallel) |
| POST | `/api/v1/enrich` | Enrich JSON records with parsed address fields |
| GET  | `/health/live` | Liveness probe |
| GET  | `/health/ready` | Readiness probe |
| GET  | `/health/startup` | Startup probe |
| GET  | `/health/info` | Full service info + stats |
| GET  | `/metrics` | Prometheus metrics |

---

## Build Commands
#### Note: 
* If Records (json objects) is greater than the MAX_BATCH_SIZE (current 10K) thne only 10k will be returned as parse remaing will be discarded. With message.
```text
"total_processed": 2000,
total_received": 2005,
"warning": "Received 2005 records but maximum is 2000. Only first 2000 records processed. Remaining 5 records were discarded."
```
### First time / clean build
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
rm -rf build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DENABLE_MIMALLOC=OFF
make -j$(nproc)
```

### After changing only .cc or .h files (no CMakeLists.txt change)
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized/build
make -j$(nproc)
```

### After changing CMakeLists.txt (new file added, new option, new library)
```bash
cd ~/FAISS-Actual-26June25/address-service-optimized/build
cmake ..
make -j$(nproc)
```

---

## When Do You Need cmake .. vs make?

| What you changed | Command needed |
|-----------------|----------------|
| `.cc` or `.h` file only | `make -j$(nproc)` only |
| Added a new `.cc` file to SOURCES in CMakeLists.txt | `cmake ..` then `make -j$(nproc)` |
| Changed a CMake option (ENABLE_LLM, ENABLE_REDIS etc) | `cmake ..` then `make -j$(nproc)` |
| Changed config.json only | Just restart the binary, no rebuild needed |
| Changed a `static constexpr` flag in a header (e.g. WRITE_JSON_DEBUG) | `make -j$(nproc)` only |

---

## Run the Service

```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
./build/address-service -c config/config.json
```

Wait for:
```
[main] Server listening on 0.0.0.0:8080 with N worker threads
```

---

## Environment Variable Overrides (no rebuild needed)

```bash
PORT=9090                        # override port
LIBPOSTAL_DATA_DIR=/custom/path  # override libpostal data path
CACHE_MAX_SIZE=1000000           # override max cache entries
AUTH_ENABLED=true                # enable auth at runtime
AUTH_MODE=api_key                # api_key | jwt | both
API_KEY=mysecretkey              # set master API key
DISABLE_LLM=true                 # kill-switch for Phase 4 LLM
ENABLE_LANGUAGE_CLASSIFIER=false # skip libpostal language classifier (~300MB RAM saved)
```

Example with overrides:
```bash
PORT=9090 AUTH_ENABLED=false ./build/address-service -c config/config.json
```

---

## Quick Test — All Endpoints

```bash
# Health
curl http://localhost:8080/health/live
curl http://localhost:8080/health/ready
curl http://localhost:8080/health/info | python3 -m json.tool

# Metrics
curl http://localhost:8080/metrics

# Parse single
curl -s -X POST http://localhost:8080/api/v1/parse \
  -H "Content-Type: application/json" \
  -d '{"address": "123 MG Road, Bengaluru 560001"}' | python3 -m json.tool

# Normalize single
curl -s -X POST http://localhost:8080/api/v1/normalize \
  -H "Content-Type: application/json" \
  -d '{"address": "123 MG Road, Bengaluru 560001"}' | python3 -m json.tool

# Batch
curl -s -X POST http://localhost:8080/api/v1/batch \
  -H "Content-Type: application/json" \
  -d '{
    "addresses": [
      "123 MG Road, Bengaluru 560001",
      "350 Fifth Ave, New York NY 10118"
    ]
  }' | python3 -m json.tool

# Enrich — ALL columns
curl -s -X POST http://localhost:8080/api/v1/enrich \
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

# Enrich — specific parsed columns only
curl -s -X POST http://localhost:8080/api/v1/enrich \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"id": 101, "name": "Alice", "addr": "123 MG Road, Bengaluru 560001", "phone": "9876"}
    ],
    "metadata": {
      "address_column": "addr",
      "key_column": "id",
      "normalize_columns": "city,state,postcode,country"
    }
  }' | python3 -m json.tool

# Enrich — auto-detect columns, one bad address
curl -s -X POST http://localhost:8080/api/v1/enrich \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"uid": "A1", "full_address": "456 Park Street, Chennai 600001", "dept": "Sales"},
      {"uid": "A2", "full_address": "x", "dept": "HR"},
      {"uid": "A3", "full_address": "1600 Amphitheatre Pkwy, Mountain View CA 94043", "dept": "Eng"}
    ],
    "metadata": {
      "address_column": "full_address",
      "key_column": "uid",
      "normalize_columns": ""
    }
  }' | python3 -m json.tool
```

---

## Enable Debug File Output (EnrichController)

In `src/controllers/EnrichController.h`, change the flags and rebuild with `make -j$(nproc)`:

```cpp
// Write both debug files:
static constexpr bool WRITE_JSON_DEBUG = true;
static constexpr bool WRITE_CSV_DEBUG  = true;

// CSV only:
static constexpr bool WRITE_JSON_DEBUG = false;
static constexpr bool WRITE_CSV_DEBUG  = true;

// Production (no file I/O):
static constexpr bool WRITE_JSON_DEBUG = false;
static constexpr bool WRITE_CSV_DEBUG  = false;
```

Files are written to:
- `./uploads/debug_results.json`
- `./uploads/debug_results.csv`

---

## Adding a New Controller (checklist)

1. Create `src/controllers/MyController.h` — inherit `drogon::HttpController<MyController>`, define `METHOD_LIST_BEGIN/END`
2. Create `src/controllers/MyController.cc` — extern the globals, implement handler
3. Add to `CMakeLists.txt` SOURCES: `src/controllers/MyController.cc`
4. Run `cmake .. && make -j$(nproc)`

## API Endpoints

The service exposes **9 endpoints**:

- `/api/v1/parse` — Parse a **single address**  
- `/api/v1/normalize` — Return **libpostal normalized expansions**  
- `/api/v1/batch` — Process **up to 2000 addresses** with deduplication and parallel processing  
- `/api/v1/enrich` — Enrich JSON records by adding `normalize_<addr_col>` fields  
- `/health/live` — Liveness check  
- `/health/ready` — Readiness check  
- `/health/startup` — Startup check  
- `/health/info` — Service info  
- `/metrics` — Expose metrics in **Prometheus format**