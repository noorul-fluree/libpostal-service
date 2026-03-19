# Address Normalization Service

High-performance address parsing and normalization service built with C++17, Drogon framework, and libpostal.

## Architecture

```
Request → Pre-processor → libpostal → Confidence Check
                                          │
                               ┌──────────┼──────────┐
                               │ ≥0.85    │ 0.70-0.85│ <0.70
                               ▼          ▼          ▼
                            Return    Rule Engine   LLM Fallback
                                          │          │
                                          ▼          ▼
                                       Return     Return
```

## Quick Start

### Prerequisites

- GCC 11+ or Clang 14+
- CMake 3.16+
- libpostal (compiled from source)
- Drogon framework (v1.9+)
- libjsoncpp-dev, libssl-dev, uuid-dev, zlib1g-dev

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Build with LLM support

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_LLM=ON
make -j$(nproc)
```

### Run

```bash
./address-service --config ../config/config.json
```

### Run tests

```bash
cmake .. -DENABLE_TESTS=ON
make -j$(nproc)
./address-service-tests
```

### Docker

```bash
# First time: download libpostal data
docker compose --profile init up libpostal-data-init

# Start the service
docker compose up -d address-service nginx

# With monitoring stack
docker compose up -d
```

## API

### Parse single address

```bash
curl -X POST http://localhost:8080/api/v1/parse \
  -H "Content-Type: application/json" \
  -d '{"address": "123 MG Road, Bengaluru, Karnataka 560001"}'
```

### Normalize

```bash
curl -X POST http://localhost:8080/api/v1/normalize \
  -H "Content-Type: application/json" \
  -d '{"address": "123 Main St NYC"}'
```

### Batch parse

```bash
curl -X POST http://localhost:8080/api/v1/batch \
  -H "Content-Type: application/json" \
  -d '{
    "addresses": [
      "123 MG Road, Bengaluru 560001",
      "456 Park Ave, New York NY 10022",
      "789 Oxford Street, London W1D 1BS"
    ]
  }'
```

### Health checks

```bash
curl http://localhost:8080/health/live     # Liveness
curl http://localhost:8080/health/ready    # Readiness
curl http://localhost:8080/health/startup  # Startup
curl http://localhost:8080/health/info     # Detailed info + stats
```

### Metrics

```bash
curl http://localhost:8080/metrics    # Prometheus format
```

## Configuration

Configuration is loaded from `config.json` and can be overridden with environment variables:

| Environment Variable | Description | Default |
|---------------------|-------------|---------|
| PORT | HTTP listen port | 8080 |
| LIBPOSTAL_DATA_DIR | Path to libpostal data | /usr/share/libpostal |
| DROGON_THREADS | Worker thread count | auto (cores - 1) |
| CACHE_MAX_SIZE | Max cache entries | 5000000 |
| LLM_ENABLED | Enable LLM fallback | false |
| LLM_MODEL_PATH | Path to GGUF model | - |

## Project Structure

```
address-service/
├── CMakeLists.txt              # Build system
├── Dockerfile                  # Multi-stage Docker build
├── docker-compose.yml          # Local dev environment
├── config/
│   ├── config.json             # Service configuration
│   ├── nginx.conf              # Reverse proxy config
│   └── prometheus.yml          # Metrics scraping
├── src/
│   ├── main.cc                 # Entry point, init, Drogon setup
│   ├── models/
│   │   └── AddressModels.h     # Data models, config, JSON serialization
│   ├── services/
│   │   ├── PreProcessor.h/cc   # Phase 1: cleanup, abbreviation expansion
│   │   ├── AddressParser.h/cc  # Phase 2: libpostal integration
│   │   ├── ConfidenceScorer.h/cc # Confidence scoring (0.0-1.0)
│   │   ├── RuleEngine.h/cc     # Phase 3: PIN/ZIP validation, misspellings
│   │   ├── LLMFallback.h/cc    # Phase 4: llama.cpp integration
│   │   ├── CacheManager.h/cc   # Thread-safe LRU cache with TTL
│   │   └── MetricsCollector.h/cc # Prometheus metrics
│   └── controllers/
│       ├── ParseController.h/cc    # /api/v1/parse, /api/v1/normalize
│       ├── BatchController.h/cc    # /api/v1/batch
│       ├── HealthController.h/cc   # /health/*
│       └── MetricsController.h/cc  # /metrics
└── tests/
    ├── test_main.cc            # Test runner
    ├── test_preprocessor.cc    # Pre-processor tests
    ├── test_parser.cc          # Parser tests (needs libpostal)
    ├── test_rule_engine.cc     # Rule engine tests
    ├── test_confidence.cc      # Confidence scorer tests
    └── test_cache.cc           # Cache tests (incl. thread safety)
```
