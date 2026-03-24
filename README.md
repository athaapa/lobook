# lobook

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

By default this builds **`server`**, **`server_cached`**, and **`server_nocached`** only (latency harness).

Optional targets (off by default):

```bash
cmake -S . -B build -DLOBOOK_BUILD_BENCHMARKS=ON -DLOBOOK_BUILD_BENCHMARK_SPARSE=ON -DLOBOOK_BUILD_TESTS=ON
cmake --build build
```

## Ladder Optimization