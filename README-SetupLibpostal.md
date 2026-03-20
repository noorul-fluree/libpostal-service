# libpostal C++ Address Normalization Pipeline

A full **C++17** address normalization and validation pipeline using **libpostal** —
covering expansion, parsing, component extraction, type detection, and multi-country validation.

---

## Architecture

```
Raw Address String
      │
      ▼
┌─────────────────────┐
│  EXPAND             │  libpostal_expand_address()
│  "st" → "street"   │  Handles abbreviations, typos,
│  "apt" → "apartment"│  transliterations, 60+ languages
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  PARSE              │  libpostal_parse_address()
│  Extract components │  ML-based labelling:
│  house_number, road │  house_number, road, city,
│  city, state, zip   │  state, postcode, country...
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  VALIDATE           │  AddressValidator
│  - Required fields  │  - Type detection (residential/
│  - Postcode format  │    commercial/PO Box)
│  - US state codes   │  - Country-specific postcode regex
│  - Type-specific    │  - Errors vs warnings
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  NORMALIZE OUTPUT   │  ParsedAddress::normalized()
│  Canonical string   │  Title-cased, ordered, clean
└─────────────────────┘
```

---

## File Structure

```
address_normalizer/
├── CMakeLists.txt
├── main.cpp                    # Demo & entry point
└── include/
    ├── address_types.hpp       # ParsedAddress, ValidationResult, enums
    ├── validator.hpp           # AddressValidator (rules engine)
    └── normalizer.hpp          # AddressNormalizer (libpostal wrapper)
```

---

## Installation

### Step 1: Install libpostal (C library)

```bash
# Ubuntu/Debian
sudo apt-get install curl autoconf automake libtool pkg-config

git clone https://github.com/openvenues/libpostal
cd libpostal
./bootstrap.sh

# Download ~2GB training data (one-time)
./configure --datadir=/usr/share/libpostal
make -j$(nproc)
sudo make install
sudo ldconfig
```

### Step 2: Build this project

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Step 3: Run

```bash
./address_normalizer

# Optional: specify custom data directory
./address_normalizer /custom/path/to/libpostal/data
```

---

## Key Classes

### `AddressNormalizer`
Main pipeline class. Wraps libpostal lifecycle (setup/teardown).

| Method | Description |
|--------|-------------|
| `process(string)` | Full pipeline: expand → parse → validate → normalize |
| `process_batch(vector<string>)` | Process multiple addresses |
| `deduplicate(vector<ParsedAddress>)` | Remove duplicate normalized forms |
| `print_report(addr)` | Pretty-print single result |
| `print_batch_report(results)` | Summary report for batch |

### `AddressValidator`
Stateless rules engine. Called automatically by `AddressNormalizer`.

| Rule | Description |
|------|-------------|
| Type detection | Residential / Commercial / PO Box |
| Required fields | Checks house number, road, city per type |
| Postcode format | Regex per country (US, CA, GB, DE, AU, IN, FR) |
| US state codes | Validates against all 50 states + territories |

### `ParsedAddress`
Result struct with all components + validation status.

```cpp
struct ParsedAddress {
    string raw;              // Original input
    string house_number;     // "123"
    string road;             // "Main Street"
    string unit;             // "Apt 4B"
    string city;             // "New York"
    string state;            // "NY"
    string postcode;         // "10001"
    string country;          // "United States"
    AddressType type;        // RESIDENTIAL / COMMERCIAL / PO_BOX
    ValidationResult validation;
    vector<string> expanded_forms;

    string normalized();     // Canonical output string
    string best_expansion(); // Top libpostal expansion
};
```

### `ValidationResult`

```cpp
struct ValidationResult {
    ValidationStatus status; // VALID / PARTIAL / INVALID / AMBIGUOUS
    vector<string> errors;   // Blocking issues
    vector<string> warnings; // Non-blocking issues
};
```

---

## Usage Example

```cpp
#include "include/normalizer.hpp"

int main() {
    AddressNormalizer normalizer;

    // Single address
    auto result = normalizer.process("123 main st apt 4b new york ny 10001");

    std::cout << result.normalized();
    // → "123, Main Street, Unit 4B, New York, NY, 10001"

    std::cout << result.validation.summary();
    // → [VALID]

    // Batch
    auto results = normalizer.process_batch({
        "221b baker street london uk",
        "Flat 3, Oxford Rd, Manchester M13 9PL",
        "incomplete address"
    });

    AddressNormalizer::print_batch_report(results);

    // Filter valid only
    for (const auto& r : results) {
        if (r.validation.is_valid()) {
            std::cout << r.normalized() << "\n";
        }
    }
}
```

---

## Supported Countries (Postcode Validation)

| Country | Pattern | Example |
|---------|---------|---------|
| US | `\d{5}(-\d{4})?` | `10001`, `90210-1234` |
| Canada | `[A-Z]\d[A-Z] \d[A-Z]\d` | `M5V 3A8` |
| UK | `[A-Z]{1,2}\d[A-Z\d]? \d[A-Z]{2}` | `NW1 6XE` |
| Germany | `\d{5}` | `10117` |
| Australia | `\d{4}` | `2000` |
| India | `\d{6}` | `122003` |
| France | `\d{5}` | `75001` |

---

## Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| libpostal | ≥1.1 | Core NLP parsing & expansion |
| C++ stdlib | C++17 | regex, set, map, optional |
| CMake | ≥3.16 | Build system |
| pthread | system | libpostal threading |

---

## Notes

- libpostal initializes **once** per process (singleton pattern enforced)
- First run downloads ~2GB of training data
- Processing is **CPU-only**, no GPU required
- Handles 60+ languages and scripts out of the box
- The `deduplicate()` method compares **normalized forms**, so
  `"123 main st NY"` and `"123 Main Street New York"` will be deduped
