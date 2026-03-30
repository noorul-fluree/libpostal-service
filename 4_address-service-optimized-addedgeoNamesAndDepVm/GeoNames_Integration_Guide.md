# GeoNames Integration Guide
## Address Normalization Service — Phase 2 Geo Lookup

---

## What Is This?

The `/api/v1/enrich/geo` endpoint extends the standard `/api/v1/enrich` endpoint with a **Phase 2 GeoNames geo lookup layer** that fills in missing or low-confidence city/state fields using two GeoNames databases.

**Standard enrich pipeline:**
```
libpostal parse → confidence score → rule engine → response
```

**Geo enrich pipeline:**
```
libpostal parse → confidence score → rule engine
  → Phase 2A: postal code → city + state  (if postcode found)
  → Phase 2B: city name   → state         (if city found, state missing)
  → rescore → response
```

---

## When Does Geo Lookup Trigger?

| Condition | Action |
|-----------|--------|
| Field is blank | Always fill from GeoNames |
| Field exists + confidence ≥ 0.7 | Skip — trust libpostal |
| Field exists + confidence < 0.7 | Override with GeoNames result |

**Supported countries:** India (IN), United States (US), United Kingdom (GB)

---

## Setup — Data Files

### Step 1 — Download GeoNames files (one time only)

```bash
mkdir -p ~/libpostal_data/geonames
cd ~/libpostal_data/geonames

# Cities database (Phase 2B — city → state, global)
wget https://download.geonames.org/export/dump/cities15000.zip

# Admin1 codes (state name lookup)
wget https://download.geonames.org/export/dump/admin1CodesASCII.txt

# Postal codes database (Phase 2A — postal → city + state)
wget https://download.geonames.org/export/zip/allCountries.zip -O postal_codes.zip
```

### Step 2 — Extract files

```bash
cd ~/libpostal_data/geonames
unzip cities15000.zip

# Postal codes — rename to avoid conflict with places allCountries
unzip postal_codes.zip
# If it asks to overwrite, press 'r' and rename to: postal_codes.txt
```

### Step 3 — Verify files exist

```bash
ls -lh ~/libpostal_data/geonames/
# Expected:
# admin1CodesASCII.txt  (~148KB)
# cities15000.txt       (~7.5MB)
# postal_codes.txt      (~135MB)
```

### Step 4 — Add to config.json

```json
"geonames": {
    "data_dir": "/home/noorulk/libpostal_data/geonames"
}
```

### Step 5 — Build and run

```bash
# WSL build
cd ~/FAISS-Actual-26June25/address-service-optimized/build
make -j$(nproc)
cd ..
./build/address-service -c config/config.json
```

**Startup log confirms GeoNames loaded:**
```
[GeoNamesDB] admin1 codes loaded: 91
[GeoNamesDB] postal codes loaded: 63728
[GeoNamesDB] cities loaded: 7485
[GeoNamesDB] Ready | postal_entries=63728 | city_entries=7485
[main] GeoNamesDB ready (63728 postal, 7485 city entries)
```

---

## Response Fields (new in /enrich/geo)

| Field | Type | Description |
|-------|------|-------------|
| `geo_applied` | bool | true if geo lookup changed any field |
| `geo_source` | string | `postal_2a` / `city_2b` / `none` |
| `geo_applied_count` | int | total records where geo changed something |
| `geo_db_ready` | bool | whether GeoNames DB loaded successfully |

---

## Scenario 1 — libpostal works well (standard model)
*Showing what standard libpostal alone can do*

**Request:**
```json
{
  "data": [
    {"id": 1, "name": "Alice", "addr": "123 MG Road, Bengaluru 560001"},
    {"id": 2, "name": "Bob",   "addr": "350 Fifth Ave, New York NY 10118"},
    {"id": 3, "name": "Carol", "addr": "221B Baker Street, London W1U 8ED"}
  ],
  "metadata": {"address_column": "addr", "normalize_columns": "ALL"}
}
```

**Response highlights:**
```json
{
  "geo_applied_count": 0,
  "results": [
    {
      "id": 1,
      "city": "bengaluru", "state": "karnataka", "postcode": "560001",
      "country": "india", "confidence": 0.97,
      "geo_applied": false, "geo_source": "none",
      "normalize_addr": "123 mahatma gandhi road bengaluru karnataka 560001 india"
    },
    {
      "id": 2,
      "city": "new york", "state": "new york", "postcode": "10118",
      "country": "united states", "confidence": 0.95,
      "geo_applied": false, "geo_source": "none",
      "normalize_addr": "350 fifth avenue new york new york 10118 united states"
    },
    {
      "id": 3,
      "city": "london", "state": "england", "postcode": "w1u 8ed",
      "confidence": 0.72,
      "geo_applied": false, "geo_source": "none"
    }
  ]
}
```

**What this shows:** libpostal handles well-formed addresses with full details correctly on its own. `geo_applied: false` means libpostal was confident enough — geo lookup correctly stepped aside.

---

## Scenario 2 — Senzing model improvement
*Senzing v1.2 parser vs standard model on business/suite addresses*

Switch to Senzing in config.json:
```json
"data_dir": "/home/noorulk/libpostal_data/libpostal_senzing"
```

**Request:**
```json
{
  "data": [
    {"id": 1, "addr": "350 Fifth Ave Ste 500, New York NY 10118"},
    {"id": 2, "addr": "Flat 4B, Tower 2, Prestige Shantiniketan, Whitefield, Bengaluru 560066"},
    {"id": 3, "addr": "Unit 12, Level 3, One Canada Square, London E14 5AB"}
  ],
  "metadata": {"address_column": "addr", "normalize_columns": "ALL"}
}
```

**Response highlights (Senzing):**
```json
{
  "results": [
    {
      "id": 1,
      "house_number": "350", "road": "fifth avenue", "unit": "suite 500",
      "city": "new york", "state": "new york", "postcode": "10118",
      "confidence": 0.96
    },
    {
      "id": 2,
      "unit": "flat 4b", "road": "prestige shantiniketan whitefield",
      "city": "bengaluru", "state": "karnataka", "postcode": "560066",
      "confidence": 0.91
    },
    {
      "id": 3,
      "unit": "unit 12", "level": "level 3",
      "road": "one canada square", "city": "london", "postcode": "e14 5ab",
      "state": "england", "confidence": 0.88
    }
  ]
}
```

**What this shows:** Senzing v1.2 correctly extracts `unit`, `level`, and suite numbers that the standard model misses on business/commercial addresses.

---

## Scenario 3 — GeoNames fills gaps
*Sparse or partial addresses where libpostal can't extract enough*

**Request:**
```json
{
  "data": [
    {"id": 1, "addr": "560001"},
    {"id": 2, "addr": "10001, New York"},
    {"id": 3, "addr": "SW1A 1AA, London"},
    {"id": 4, "addr": "M1 1AE, Manchester"},
    {"id": 5, "addr": "SW1 2AA"}
  ],
  "metadata": {"address_column": "addr", "normalize_columns": "ALL"}
}
```

**Response highlights:**
```json
{
  "geo_applied_count": 2,
  "results": [
    {
      "id": 1, "addr": "560001",
      "city": "bengaluru", "state": "karnataka", "postcode": "560001",
      "confidence": 0.74, "geo_applied": false, "geo_source": "none",
      "comment": "libpostal got this right on its own"
    },
    {
      "id": 2, "addr": "10001, New York",
      "city": "new york", "state": "new york", "postcode": "10001",
      "confidence": 0.69, "geo_applied": true, "geo_source": "postal_2a",
      "comment": "geo filled state (confidence < 0.7 triggered override)"
    },
    {
      "id": 3, "addr": "SW1A 1AA, London",
      "city": "westminster abbey", "state": "england", "postcode": "sw1a 1aa",
      "confidence": 0.66, "geo_applied": true, "geo_source": "postal_2a",
      "comment": "geo auto-detected GB from postcode pattern, filled city+state"
    },
    {
      "id": 4, "addr": "M1 1AE, Manchester",
      "city": "manchester", "state": "england", "postcode": "m1 1ae",
      "confidence": 0.60, "geo_applied": true, "geo_source": "postal_2a",
      "comment": "geo filled city + state from UK outward code"
    },
    {
      "id": 5, "addr": "SW1 2AA",
      "city": "belgravia", "state": "england", "postcode": "sw1 2aa",
      "confidence": 0.70, "geo_applied": true, "geo_source": "postal_2a",
      "comment": "no country in address, geo auto-detected GB from postcode"
    }
  ]
}
```

**What this shows:**
- `geo_source: postal_2a` = Phase 2A triggered (postal → city + state)
- `geo_source: city_2b` = Phase 2B triggered (city → state only)
- `geo_source: none` = geo lookup found nothing or wasn't needed
- UK postcodes auto-detected as GB even without "UK" or "England" in the address
- GeoNames is a **fallback** — it only activates when libpostal is unsure or blank

---

## How libpostal, Senzing, and GeoNames Work Together

```
Raw address input
       │
       ▼
┌─────────────────────────────────────────────┐
│  Phase 1: libpostal (standard OR Senzing)   │
│  Parses: house_number, road, city, state,   │
│          postcode, country, unit etc.        │
│                                             │
│  Standard model → better for residential    │
│  Senzing v1.2   → better for commercial     │
└─────────────────┬───────────────────────────┘
                  │ confidence score calculated
                  ▼
┌─────────────────────────────────────────────┐
│  Phase 2: GeoNames lookup (only on          │
│  /api/v1/enrich/geo)                        │
│                                             │
│  2A: postcode → city + state lookup         │
│      (triggers if confidence < 0.7          │
│       OR field is blank)                    │
│                                             │
│  2B: city → state lookup                   │
│      (triggers if state still missing)      │
│                                             │
│  Supported: IN / US / GB                   │
└─────────────────┬───────────────────────────┘
                  │
                  ▼
            Final response
      with geo_applied + geo_source
```

**Use `/api/v1/enrich`** when addresses are well-formed and complete.
**Use `/api/v1/enrich/geo`** when addresses are sparse, partial, or postcode-only.

---

## Switching Between Models

**One line in config.json — restart service, no rebuild:**

```json
// Standard libpostal:
"data_dir": "/home/noorulk/libpostal_data/libpostal"

// Senzing v1.2 (better for business addresses):
"data_dir": "/home/noorulk/libpostal_data/libpostal_senzing"
```

GeoNames lookup works with **both models** — it's independent of which libpostal model is active.

---

## Files Added for GeoNames Integration

| File | Purpose |
|------|---------|
| `src/services/GeoNamesDB.h` | In-memory lookup table declarations |
| `src/services/GeoNamesDB.cc` | Loads postal_codes.txt + cities15000.txt + admin1CodesASCII.txt |
| `src/controllers/GeoEnrichController.h` | Endpoint declaration |
| `src/controllers/GeoEnrichController.cc` | Phase 2A + 2B pipeline logic |

**Changes to existing files:**
- `src/main.cc` — GeoNamesDB initialization at startup
- `src/models/AddressModels.h` — added `geonames_data_dir` to ServiceConfig
- `CMakeLists.txt` — added 2 new source files
- `config/config.json` — added `geonames.data_dir`

---

## GeoNames Data Files Summary

| File | Source | Size | Purpose |
|------|--------|------|---------|
| `postal_codes.txt` | geonames.org/export/zip/ | ~135MB | Phase 2A: postal→city+state |
| `cities15000.txt` | geonames.org/export/dump/ | ~7.5MB | Phase 2B: city→state |
| `admin1CodesASCII.txt` | geonames.org/export/dump/ | ~148KB | State code→name mapping |

All files are loaded once at startup into memory (filtered to IN/US/GB only):
- 63,728 postal code entries
- 7,485 city entries
- 91 admin1 state mappings
