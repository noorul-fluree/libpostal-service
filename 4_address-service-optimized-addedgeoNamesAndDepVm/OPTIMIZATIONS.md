# address-service — Optimization Guide

## What changed and why

### 1. CacheManager — Sharded LRU (biggest win)

**File:** `src/services/CacheManager.h/.cc`

**Before:** Single `std::mutex` + `std::unordered_map` + `std::list`.
Every request — parse, batch, or cached — acquires this one lock.
At 100 concurrent threads this is a full serialization point.

**After:** 64 independent shards, each with its own mutex, LRU list, and map.
A request only locks 1 of 64 shards, based on `hash & 63`.
Shards are `alignas(64)` so they live on separate cache lines — zero false sharing.
Hash is FNV-1a over the raw string → `uint64_t` before taking the lock,
so the map uses integer keys (no string compare inside the lock).

**Expected gain:** 10–30× throughput on the cache path under concurrency.

---

### 2. PreProcessor — Single-pass pipeline

**File:** `src/services/PreProcessor.h/.cc`

**Before:** 6 functions (`trimWhitespace`, `removeJunkChars`, `normalizeCase`,
`expandAbbreviations`, `collapseWhitespace`, `trimWhitespace` again) each
returning a new `std::string` = **6 heap allocations per address**.

**After:** `singlePassClean()` folds trim + junk removal + whitespace collapse
+ lowercasing into **one O(n) pass** with a single pre-reserved output buffer.
Then `expandAbbreviations()` does one tokenization pass and writes into a
second reserved buffer. Total: **2 allocations** instead of 6.

Input is taken as `std::string_view` throughout — no copy from caller's buffer.

**Expected gain:** 3–4× faster preprocessing; lower allocator pressure.

---

### 3. RuleEngine — No std::regex in hot path

**File:** `src/services/RuleEngine.h/.cc`

**Before:** `std::regex` compiled and matched on every call inside
`validateAndFixUSZip` and `inferCountry`. `std::regex` is notoriously slow —
construction alone takes microseconds.

**After:** `isIndianPIN()` and `isUSZip()` are hand-rolled `noexcept` character
checks — 6–10 comparisons, no heap, no state machine. Early-exit in `apply()`
means that if the postcode is an Indian PIN, US ZIP rules are never even checked.

**Expected gain:** 5–15× faster rule application for PIN/ZIP addresses.

---

### 4. ConfidenceScorer — No std::regex anywhere

**File:** `src/services/ConfidenceScorer.cc`

**Before:** 5 `static const std::regex` objects in `scorePostcodeValidity`,
matched on every call (even though `static` means they're compiled once,
the match itself runs each time).

**After:** Same logic as RuleEngine — hand-rolled validators for each postcode
format. `scoreTokenCoverage` now uses a single `isalnum` char-counting loop
instead of `std::istringstream` tokenization.

**Expected gain:** 3–8× faster scoring.

---

### 5. MetricsCollector — Lock-free atomic histograms

**File:** `src/services/MetricsCollector.h/.cc`

**Before:** `std::mutex latency_mutex_` + `std::vector<double>` per histogram.
Every `recordParse()` / `recordConfidence()` call acquires this mutex and
appends to a vector. Under load this is a hot contended lock. The vector also
grows unbounded (capped at 100k with a linear-time erase).

**After:** `AtomicHistogram` stores pre-bucketed `std::atomic<uint64_t>` counts.
`record(ms)` increments the right buckets with `fetch_add(relaxed)` — completely
lock-free. Sum is tracked as fixed-point microseconds in an atomic.
`serialize()` reads all atomics with no lock.

**Expected gain:** Eliminates the hot metrics mutex entirely. `recordParse()`
goes from lock+push_back to pure atomic increments.

---

### 6. BatchController — Parallel address processing

**File:** `src/controllers/BatchController.cc`

**Before:** Sequential `for` loop over all addresses. A batch of 2000 addresses
runs serially on one core, even though libpostal parse is thread-safe.

**After:** Batches above a threshold (`SERIAL_THRESHOLD = 8`) are dispatched
as parallel `std::async` tasks. Results collected in order via `futures[i].get()`.
The result vector is pre-allocated with `resize(N)` — no push_back races.
Small batches stay serial to avoid thread-spawn overhead.

If OpenMP is available (detected by CMake), `#pragma omp parallel for` is
even simpler and the compiler handles scheduling.

**Expected gain:** Near-linear speedup with core count on large batches.
A 2000-address batch on 8 cores → ~8× faster wall time.

---

### 7. JwtAuthFilter — Sharded token cache + integer hash key

**File:** `src/controllers/JwtAuthFilter.h/.cc`

**Before:** Single `std::mutex` over `std::unordered_map<std::string, ...>`.
Every authenticated request acquires this lock and does a string key lookup.

**After:** 16 shards (`alignas(64)`), integer keys (`uint64_t` FNV-1a hash of
token). Lock only the one relevant shard. Integer map lookup is branch-free.

**Expected gain:** 16× less lock contention on the JWT cache at high QPS.

---

### 8. CMakeLists.txt — Compiler flags + mimalloc + LTO

**File:** `CMakeLists.txt`

**Before:** No explicit optimization flags, no LTO, no custom allocator.

**After:**
- `-O3 -march=native -funroll-loops -ffast-math` — max optimization
- `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE` — LTO across all TUs
- `mimalloc` linked as global allocator — replaces system `malloc`/`free`.
  ParsedAddress has 15 `std::string` fields; every parse = many small allocs.
  mimalloc is 2–3× faster than glibc malloc for this workload profile.
- OpenMP detection for batch parallelism
- Per-file `-O3` on hot service files even in debug builds

**Expected gain:** 15–40% overall from flags alone; 10–20% from mimalloc.

---

## Summary table

| Component | Change | Concurrency gain | Per-request gain |
|---|---|---|---|
| CacheManager | 64-shard mutex + FNV hash | **10–30×** | 2× |
| BatchController | std::async parallel | **N× cores** | — |
| JwtAuthFilter | 16-shard cache | **16×** | 1.5× |
| MetricsCollector | Lock-free atomics | **eliminates lock** | 3× |
| PreProcessor | Single-pass pipeline | — | **3–4×** |
| RuleEngine | No regex | — | **5–15×** |
| ConfidenceScorer | No regex | — | **3–8×** |
| CMake flags | -O3 + LTO + mimalloc | — | **15–40%** |

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_MIMALLOC=ON -DENABLE_OPENMP=ON
make -j$(nproc)
```

## Dependencies added

| Library | Why | Install |
|---|---|---|
| mimalloc | Fast allocator | `apt install libmimalloc-dev` |
| OpenMP | Batch parallelism | `apt install libomp-dev` |

All other optimizations are zero-dependency (stdlib only).
