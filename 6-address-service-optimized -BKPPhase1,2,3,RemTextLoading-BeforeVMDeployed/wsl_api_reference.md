# Address Normalization Service — API Reference
## All endpoints, request/response format, architecture, examples

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
| Build | CMake 3.16+ / GCC 11+ | C++17, -O3, LTO |

---

## Service Overview

A C++ HTTP service that parses, normalizes and enriches raw address strings.

| What | Value |
|------|-------|
| Base URL (WSL) | `http://localhost:8080` |
| Framework | Drogon 1.9.3 |
| Parser | libpostal + Senzing v1.2 |
| Geo lookup | LMDB (6 databases, 249 countries) |
| Auth | Disabled by default |

---

## Processing Pipeline

Every address flows through up to 4 phases:

```
Raw input
  → InputValidator      (length ≤ 500, UTF-8, no null bytes)
  → PreProcessor        (lowercase, trim, expand abbreviations e.g. MG→Mahatma Gandhi)
  → CacheManager.get()  (return cached result if hit)
  → AddressParser       (libpostal parse with language/country hints)
  → ConfidenceScorer    (0.0–1.0 score: completeness + postcode + cross-field + tokens)
  → RuleEngine          (PIN→state, ZIP→country, misspelling fix, state normalize)
  → LLMFallback         (Phase 4 — disabled)
  → CacheManager.put()  (store if confidence ≥ 0.5)
  → Response
```

For `/api/v1/enrich/geo/lmdb` only — additional Phase 2 LMDB geo lookup:
```
  → Phase 2A: postcode → city + state  (postal.lmdb)
  → Phase 2B: city → state             (cities.lmdb)
  → Phase 2C: alias resolution         (aliases.lmdb — Bombay→Mumbai)
  → Phase 2D: country name fill        (countries.lmdb)
  → rescore after geo enrichment
```

---

## Confidence Score

| Range | Meaning | Action |
|-------|---------|--------|
| ≥ 0.85 | High confidence | Return immediately, skip Phase 3 |
| 0.70–0.85 | Medium | Run rule engine |
| < 0.70 | Low | Run rule engine + geo lookup override |
| < 0.50 | Very low | Not cached |

---

## Batch Over-limit Behaviour

All batch endpoints (`/api/v1/batch`, `/api/v1/enrich`, `/api/v1/enrich/geo/lmdb`) never reject over-limit requests. They process the first `max_size` records and discard the rest with a warning:

```json
{
  "total": 5000,
  "succeeded": 5000,
  "failed": 0,
  "warning": "Received 6000 records but maximum is 5000. Only first 5000 records processed. Remaining 1000 records were discarded.",
  "total_received": 6000,
  "total_processed": 5000,
  "total_discarded": 1000,
  "results": [...]
}
```

**To change max records — edit `config.json` only, restart, no rebuild:**
```json
"batch": {
    "max_size": 10000
}
```

---

## Adding a New Controller (checklist)

1. Create `src/controllers/MyController.h` — inherit `drogon::HttpController<MyController>`, define routes in `METHOD_LIST_BEGIN/END`
2. Create `src/controllers/MyController.cc` — declare globals with `extern`, implement handler
3. Add to `CMakeLists.txt` SOURCES: `src/controllers/MyController.cc`
4. Add banner line to `src/main.cc` `printBanner()` function
5. Run `cmake .. && make -j$(nproc)`

---

## Address Processing APIs

---

### POST /api/v1/parse

Parse a single address string into components.

**Request:**
```json
{
  "address": "123 MG Road, Bengaluru 560001",
  "language": "en",
  "country": "in"
}
```

**Response:**
```json
{
  "house_number": "123",
  "road": "mahatma gandhi road",
  "city": "bengaluru",
  "state": "karnataka",
  "postcode": "560001",
  "country": "india",
  "confidence": 0.853,
  "from_cache": false,
  "latency_ms": 18.1,
  "pipeline_phases": []
}

**Example — US address:**
```bash
curl -s -X POST http://localhost:8080/api/v1/parse \
  -H "Content-Type: application/json" \
  -d '{"address": "350 Fifth Avenue, New York, NY 10118"}' | python3 -m json.tool
```
```json
{
  "house_number": "350",
  "road": "fifth avenue",
  "city": "new york",
  "state": "new york",
  "postcode": "10118",
  "country": "united states",
  "confidence": 0.95,
  "from_cache": false,
  "latency_ms": 12.4
}
```

**Example — UK address:**
```bash
curl -s -X POST http://localhost:8080/api/v1/parse \
  -H "Content-Type: application/json" \
  -d '{"address": "10 Downing Street, London SW1A 2AA"}' | python3 -m json.tool
```
```json
{
  "house_number": "10",
  "road": "downing street",
  "city": "london",
  "postcode": "sw1a 2aa",
  "country": "united kingdom",
  "confidence": 0.88,
  "from_cache": false,
  "latency_ms": 14.2
}
```
```

**Notes:**
- `language` and `country` are optional hints that improve accuracy
- `from_cache: true` means libpostal was skipped — result served from LRU cache
- `confidence` is 0.0–1.0

---

### POST /api/v1/normalize

Return libpostal normalized expansions for an address.

**Request:**
```json
{
  "address": "123 MG Rd, Bengaluru 560001"
}
```

**Response:**
```json
{
  "input": "123 MG Rd, Bengaluru 560001",
  "normalizations": [
    "123 mahatma gandhi road bengaluru 560001",
    "123 mg road bengaluru 560001"
  ]
}
```

---

### POST /api/v1/batch

Parse up to 10,000 addresses in one call. Deduplicates before parsing.

**Request:**
```json
{
  "addresses": [
    "123 MG Road, Bengaluru 560001",
    "350 Fifth Ave, New York NY 10118",
    "x"
  ]
}
```

**Response:**
```json
{
  "total": 3,
  "succeeded": 2,
  "failed": 1,
  "total_latency_ms": 42.3,
  "results": [
    {
      "input": "123 MG Road, Bengaluru 560001",
      "city": "bengaluru",
      "state": "karnataka",
      "postcode": "560001",
      "country": "india",
      "confidence": 0.853,
      "from_cache": false
    },
    {
      "input": "350 Fifth Ave, New York NY 10118",
      "city": "new york",
      "state": "new york",
      "postcode": "10118",
      "country": "united states",
      "confidence": 0.95,
      "from_cache": false
    },
    {
      "input": "x",
      "error": "address too short (min 3 chars)"
    }
  ]
}
```

**Over-limit behaviour:** If more than `batch_max_size` records sent, only first N are processed. Rest are silently discarded with a warning field in the response.

---

### POST /api/v1/enrich

Enrich a JSON dataset by adding parsed address fields. Passes through all original fields untouched.

**Request:**
```json
{
  "data": [
    {"id": 1, "name": "Alice", "addr": "123 MG Road, Bengaluru 560001", "phone": "9876"},
    {"id": 2, "name": "Bob",   "addr": "350 Fifth Ave, New York NY 10118"}
  ],
  "metadata": {
    "address_column": "addr",
    "key_column": "id",
    "normalize_columns": "ALL"
  }
}
```

**`normalize_columns` options:**
- `"ALL"` — add all parsed fields
- `"city,state,postcode,country"` — add only specific fields
- `""` — add only `normalize_addr` (the combined normalized string)

**Response:**
```json
{
  "total": 2,
  "succeeded": 2,
  "failed": 0,
  "address_column": "addr",
  "key_column": "id",
  "normalize_columns": "ALL",
  "total_latency_ms": 22.4,
  "results": [
    {
      "id": 1,
      "name": "Alice",
      "addr": "123 MG Road, Bengaluru 560001",
      "phone": "9876",
      "house_number": "123",
      "road": "mahatma gandhi road",
      "city": "bengaluru",
      "state": "karnataka",
      "postcode": "560001",
      "country": "india",
      "confidence": 0.853,
      "from_cache": false,
      "latency_ms": 0.0,
      "normalize_addr": "123 mahatma gandhi road bengaluru karnataka 560001 india",
      "expansions": ["mahatma gandhi road"]
    }
  ]
}
```

---

### POST /api/v1/enrich/geo/lmdb

Same as `/api/v1/enrich` but adds Phase 2 LMDB geo lookup layer. Uses all 6 LMDB databases, all 249 countries.

**When geo triggers:**
- Field is blank → always fill
- Field exists + confidence < 0.7 → override with LMDB result
- Field exists + confidence ≥ 0.7 → skip (trust libpostal)

**Request:** Same format as `/api/v1/enrich`

**Extra response fields per record:**
```json
{
  "geo_applied": true,
  "geo_source": "postal_2a",
  "geo_backend": "lmdb"
}
```

**`geo_source` values:**
| Value | Meaning |
|-------|---------|
| `postal_2a` | Phase 2A — postcode → city + state |
| `city_2b` | Phase 2B — city → state |
| `alias_2c` | Phase 2C — city alias resolved (e.g. Bombay→Mumbai) |
| `country_2d` | Phase 2D — country name filled from country code |
| `none` | Geo lookup not applied |

**Extra response fields at batch level:**
```json
{
  "geo_applied_count": 3,
  "geo_db_ready": true,
  "geo_backend": "lmdb"
}
```

**Example — sparse addresses:**
```bash
curl -s -X POST http://localhost:8080/api/v1/enrich/geo/lmdb \
  -H "Content-Type: application/json" \
  -d '{
    "data": [
      {"id":1,"addr":"560001, India"},
      {"id":2,"addr":"10001, New York"},
      {"id":3,"addr":"M1 1AE, Manchester"},
      {"id":4,"addr":"Bombay, India"}
    ],
    "metadata": {
      "address_column": "addr",
      "key_column": "id",
      "normalize_columns": "ALL"
    }
  }' | python3 -m json.tool
```

---

## Reference Data APIs

All ref endpoints use the LMDB databases directly — no libpostal involved.

---

### GET /ref/v1/postal/{code}

Look up a postal code to get city + state.

**With country hint (recommended):**
```bash
curl "http://localhost:8080/ref/v1/postal/560001?country=IN"
```

**Without country hint (auto-detects from postcode pattern):**
```bash
curl "http://localhost:8080/ref/v1/postal/M1%201AE"
```

**Response (found):**
```json
{
  "found": true,
  "postal_code": "560001",
  "country_code": "IN",
  "city": "bangalore g.p.o.",
  "state": "karnataka"
}
```

**Response (not found):**
```json
{
  "found": false,
  "postal_code": "999999",
  "country_code": "IN"
}
```

**US postal code:**
```bash
curl "http://localhost:8080/ref/v1/postal/10001?country=US"
```
```json
{
  "found": true,
  "postal_code": "10001",
  "country_code": "US",
  "city": "new york",
  "state": "new york"
}
```

**UK postcode:**
```bash
curl "http://localhost:8080/ref/v1/postal/M1%201AE?country=GB"
```
```json
{
  "found": true,
  "postal_code": "M1 1AE",
  "country_code": "GB",
  "city": "manchester",
  "state": "england"
}
```

---

### POST /ref/v1/postal/batch

Batch postal code lookup.

**Request:**
```json
{
  "lookups": [
    {"postal_code": "560001", "country_code": "IN"},
    {"postal_code": "10001",  "country_code": "US"},
    {"postal_code": "SW1A",   "country_code": "GB"},
    {"postal_code": "999999", "country_code": "IN"}
  ]
}
```

**Response:**
```json
{
  "total": 4,
  "found": 3,
  "not_found": 1,
  "results": [
    {"found": true,  "postal_code": "560001", "country_code": "IN", "city": "bangalore g.p.o.", "state": "karnataka"},
    {"found": true,  "postal_code": "10001",  "country_code": "US", "city": "new york",          "state": "new york"},
    {"found": true,  "postal_code": "SW1A",   "country_code": "GB", "city": "westminster abbey", "state": "england"},
    {"found": false, "postal_code": "999999", "country_code": "IN", "city": null, "state": null}
  ]
}
```

### GET /ref/v1/postal/reverse

Reverse postal lookup — given a city name, returns its known postal codes.

**With country hint (recommended):**
```bash
curl "http://localhost:8080/ref/v1/postal/reverse?city=bengaluru&country=IN"
curl "http://localhost:8080/ref/v1/postal/reverse?city=bombay&country=IN"
curl "http://localhost:8080/ref/v1/postal/reverse?city=manchester&country=GB"
```

**Without country hint (auto-detects from known countries IN/US/GB/DE/FR/AU/CA/JP/CN/SG):**
```bash
curl "http://localhost:8080/ref/v1/postal/reverse?city=manchester"
```

**Response (found):**
```json
{
  "found": true,
  "city": "bengaluru",
  "country_code": "IN",
  "returned": 5,
  "total_stored": 5,
  "note": "",
  "postal_codes": ["560001", "560002", "560009", "560025", "560300"]
}
```

**Response (alias resolved — Bombay → Mumbai codes):**
```json
{
  "found": true,
  "city": "bombay",
  "country_code": "IN",
  "returned": 6,
  "total_stored": 6,
  "note": "",
  "postal_codes": ["400007", "400024", "400031", "400035", "400043", "400066"]
}
```

**Response (not found):**
```json
{
  "found": false,
  "city": "xyzabc"
}
```

**Notes:**
- Returns max 20 postal codes per response
- `total_stored` shows how many are in the database (capped at 50 per city during indexing)
- City aliases are resolved automatically — `bombay` returns Mumbai postal codes
- India postal data uses post office names, so coverage varies by city
- US and GB have full city-level coverage

---

Look up a country by alpha2 code, alpha3 code, numeric code, or name.

```bash
curl "http://localhost:8080/ref/v1/country/IN"
curl "http://localhost:8080/ref/v1/country/IND"
curl "http://localhost:8080/ref/v1/country/india"
curl "http://localhost:8080/ref/v1/country/356"
```

**Response (found):**
```json
{
  "found": true,
  "alpha2": "IN",
  "alpha3": "IND",
  "numeric_code": "356",
  "name": "india",
  "region": "asia",
  "sub_region": "southern asia"
}
```

**Response (not found):**
```json
{
  "found": false,
  "query": "XYZ"
}
```

---

### GET /ref/v1/state/{country}/{abbrev}

Look up a state/province by country code and admin1 code.

```bash
curl "http://localhost:8080/ref/v1/state/IN/19"
curl "http://localhost:8080/ref/v1/state/US/CA"
curl "http://localhost:8080/ref/v1/state/GB/ENG"
```

**Response (found):**
```json
{
  "found": true,
  "country_code": "IN",
  "admin1_code": "19",
  "state_name": "karnataka"
}
```

**US California:**
```bash
curl "http://localhost:8080/ref/v1/state/US/CA"
```
```json
{
  "found": true,
  "country_code": "US",
  "admin1_code": "CA",
  "state_name": "california"
}
```

---

### GET /ref/v1/city/search

Search for a city by name. Resolves aliases (Bombay→Mumbai).

```bash
curl "http://localhost:8080/ref/v1/city/search?q=bombay&country=IN"
curl "http://localhost:8080/ref/v1/city/search?q=bengaluru&country=IN"
curl "http://localhost:8080/ref/v1/city/search?q=manchester&country=GB"
```

**Response (alias resolved):**
```json
{
  "found": true,
  "query": "bombay",
  "canonical_name": "mumbai",
  "country_code": "IN",
  "state": "maharashtra",
  "alias_resolved": true
}
```

**Response (direct match):**
```json
{
  "found": true,
  "query": "bengaluru",
  "canonical_name": "bengaluru",
  "country_code": "IN",
  "state": "karnataka",
  "alias_resolved": false
}
```

---

### POST /ref/v1/enrich

Enrich a parsed address object by filling missing fields using LMDB reference data.

**Request:**
```json
{
  "postcode": "560001",
  "city": "",
  "state": "",
  "country": "",
  "country_code": "IN"
}
```

**Response:**
```json
{
  "postcode": "560001",
  "city": "bangalore g.p.o.",
  "state": "karnataka",
  "country": "india",
  "country_code": "IN",
  "geo_source": "postal_2a",
  "confidence_boost": 0.25,
  "enriched_fields": ["city", "state", "country"]
}
```

**Enrichment logic:**
1. If postcode present → look up city + state (postal.lmdb)
2. If city present but state missing → look up state (cities.lmdb)
3. If city is an alias → resolve to canonical name (aliases.lmdb)
4. If country empty → fill from country code (countries.lmdb)

**`enriched_fields` values:**
- `city` — city was filled from postal lookup
- `state` — state was filled
- `country` — country name was filled
- `city_normalized` — city alias was resolved

---

### POST /ref/v1/validate

Same as `/ref/v1/enrich` but also validates the address and returns conflicts.

**Request:**
```json
{
  "postcode": "560001",
  "city": "Mumbai",
  "country_code": "IN"
}
```

**Response (conflict detected):**
```json
{
  "postcode": "560001",
  "city": "Mumbai",
  "state": "karnataka",
  "country": "india",
  "country_code": "IN",
  "geo_source": "postal_2a",
  "confidence_boost": 0.15,
  "enriched_fields": ["state", "country"],
  "valid": false,
  "conflicts": [
    "city 'mumbai' (state: maharashtra) does not match postal code '560001' (state: karnataka)"
  ],
  "validation_notes": []
}
```

**Response (valid):**
```json
{
  "postcode": "560001",
  "city": "bangalore g.p.o.",
  "state": "karnataka",
  "country": "india",
  "country_code": "IN",
  "valid": true,
  "conflicts": [],
  "validation_notes": ["postcode matches city and state"]
}
```

**Response (alias + valid):**
```json
{
  "postcode": "400001",
  "city": "mumbai",
  "state": "maharashtra",
  "country": "india",
  "country_code": "IN",
  "valid": true,
  "conflicts": [],
  "validation_notes": [
    "postal area name 'tajmahal' differs from city 'mumbai' but state matches — accepted",
    "city alias resolved: 'bombay' → 'mumbai'",
    "postcode matches city and state"
  ]
}
```
**Request — city/postcode conflict:**
```bash
curl -s -X POST http://localhost:8080/ref/v1/validate \
  -H "Content-Type: application/json" \
  -d '{"postcode":"560001","city":"Mumbai","country_code":"IN"}' | python3 -m json.tool
```
```json
{
  "postcode": "560001",
  "city": "Mumbai",
  "state": "karnataka",
  "country": "india",
  "country_code": "IN",
  "valid": false,
  "conflicts": [
    "city 'mumbai' (state: maharashtra) does not match postal code '560001' (state: karnataka)"
  ]
}
```

**Request — US validation:**
```bash
curl -s -X POST http://localhost:8080/ref/v1/validate \
  -H "Content-Type: application/json" \
  -d '{"postcode":"10001","city":"New York","country_code":"US"}' | python3 -m json.tool
```
```json
{
  "postcode": "10001",
  "city": "new york city",
  "state": "new york",
  "country": "united states of america",
  "country_code": "US",
  "valid": true,
  "conflicts": [],
  "validation_notes": ["postcode matches city and state"]
}
```
---

### POST /ref/v1/abbreviation/expand

Expand abbreviations in a text string using the same pipeline the address parser uses internally.

**Request:**
```json
{
  "text": "123 MG RD, BLDG 4, Bengaluru"
}
```

**Response:**
```json
{
  "input":    "123 MG RD, BLDG 4, Bengaluru",
  "expanded": "123 mahatma gandhi road, building 4, bengaluru",
  "changed":  true
}
```

**Banking jargon example:**
```bash
curl -s -X POST http://localhost:8080/ref/v1/abbreviation/expand \
  -H "Content-Type: application/json" \
  -d '{"text": "ATTN: John, SUBBRANCH INDT Area, PKWY 560001"}'
```
```json
{
  "input":    "ATTN: John, SUBBRANCH INDT Area, PKWY 560001",
  "expanded": "attn john, subbranch indt area, parkway 560001",
  "changed":  true
}
```

**No change example:**
```json
{
  "input":    "123 mahatma gandhi road bengaluru",
  "expanded": "123 mahatma gandhi road bengaluru",
  "changed":  false
}
```

**Error (missing field):**
```json
{ "error": "Missing or invalid 'text' field" }
```

**Notes:**
- Runs full `PreProcessor.process()` — trim + lowercase + abbreviation expand
- This is identical to what the pipeline applies internally before libpostal
- Useful for debugging what the pre-processor does to your input
- `changed: false` means no abbreviations were found to expand

---

---

### GET /health/live

Liveness probe — always returns alive if process is running.

```bash
curl http://localhost:8080/health/live
```
```json
{"status": "alive", "timestamp": 1743600000}
```

---

### GET /health/ready

Readiness probe — returns ready only when all services initialized.

```bash
curl http://localhost:8080/health/ready
```
```json
{"status": "ready", "timestamp": 1743600000}
```
---

### GET /health/startup
```bash
curl http://localhost:8080/health/startup
```
```json
{"status": "started", "timestamp": 1743600000}
```

---

### GET /health/info

Full service stats and config.

```bash
curl http://localhost:8080/health/info | python3 -m json.tool
```

---

### GET /metrics

Prometheus-format metrics.

```bash
curl http://localhost:8080/metrics
```

---

## LMDB Databases — What Powers Each Endpoint

| LMDB Database | Key format | Value | Used by |
|---|---|---|---|
| `postal.lmdb` | `IN:560001` | `bengaluru\|karnataka` | `/ref/v1/postal`, `/enrich/geo/lmdb` Phase 2A |
| `postal_reverse.lmdb` | `IN:bengaluru` | `560001,560002,...` | `/ref/v1/postal/reverse` |
| `cities.lmdb` | `IN:bengaluru` | `karnataka` | `/ref/v1/city/search`, Phase 2B |
| `countries.lmdb` | `IN` / `IND` / `356` / `india` | `india\|IN\|IND\|356\|asia\|southern asia` | `/ref/v1/country`, Phase 2D |
| `admin1.lmdb` | `IN:19` | `karnataka` | `/ref/v1/state` |
| `admin2.lmdb` | `IN:19:1234` | `bengaluru urban` | future district lookup |
| `aliases.lmdb` | `IN:bombay` | `mumbai` | `/ref/v1/city/search`, Phase 2C, reverse lookup |

**Coverage:** All 249 countries, no filter. ~5MB RAM at runtime. 7 databases total.

---

## All Endpoints Summary

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/v1/parse` | Parse single address |
| POST | `/api/v1/normalize` | libpostal expansions |
| POST | `/api/v1/batch` | Batch parse up to 10,000 |
| POST | `/api/v1/enrich` | Enrich JSON records |
| POST | `/api/v1/enrich/geo/lmdb` | Enrich + LMDB geo lookup (all 249 countries) |
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


## Geo Trigger Logic

Geo enrichment only fires when libpostal needs help:

| Condition | Geo triggers? |
|---|---|
| Field is blank | ✅ Always fill |
| Field exists + confidence < 0.7 | ✅ Override with LMDB result |
| Field exists + confidence ≥ 0.7 | ❌ Skip — trust libpostal |

Well-formed addresses like `123 MG Road Bengaluru 560001` will have `geo_applied=false` because libpostal already gets them right at high confidence.