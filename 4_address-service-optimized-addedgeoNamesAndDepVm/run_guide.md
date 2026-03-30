# Address Service — Run Guide

## Normal run (LLM disabled, recommended)

```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
export DISABLE_LLM=true
./build/address-service --config config/config.json
```

## Run with LLM enabled (Phase 4 active)

```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
unset DISABLE_LLM                          # or: export DISABLE_LLM=false
export LLM_ENABLED=true
export LLM_MODEL_PATH=/models/mistral-7b-q4.gguf   # path to your .gguf model
./build/address-service --config config/config.json
```

## Rebuild after ANY code change

```bash
cd ~/FAISS-Actual-26June25/address-service-optimized/build
make -j$(nproc)
cd ..
./build/address-service --config config/config.json
```

## Full clean rebuild (when CMakeLists.txt changes or weird linker errors)

```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
rm -rf build && mkdir build && cd build

# Without LLM:
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DENABLE_MIMALLOC=OFF
make -j$(nproc)

# With LLM compiled in:
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DENABLE_MIMALLOC=OFF -DENABLE_LLM=ON
make -j$(nproc)
```

## Run tests

```bash
cd ~/FAISS-Actual-26June25/address-service-optimized
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DENABLE_MIMALLOC=OFF
make -j$(nproc)
./address-service-tests
```

## Toggle LLM at runtime (no recompile needed)

| Want | Command |
|------|---------|
| Disable LLM | `export DISABLE_LLM=true` then restart |
| Enable LLM  | `unset DISABLE_LLM` then restart |
| Check status | `curl http://localhost:8080/health/info` → look at `llm.kill_switch_active` |

## Enable language classifier (fixes normalize endpoint warning)

```bash
export ENABLE_LANGUAGE_CLASSIFIER=true
./build/address-service --config config/config.json
```

## When to do full clean rebuild vs incremental

| Change made | Action needed |
|-------------|---------------|
| `.cc` or `.h` file edited | `cd build && make -j$(nproc)` only |
| New file added to `src/` | Add to `CMakeLists.txt` → full clean rebuild |
| `CMakeLists.txt` changed | Full clean rebuild |
| `config/config.json` changed | Just restart — no rebuild needed |
| Environment variable changed | Just restart — no rebuild needed |
