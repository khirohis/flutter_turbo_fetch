# Linux Benchmark Performance Report (`linux.md`)

This document records the empirical benchmark results, statistical verification, and technical analysis for **Linux Desktop**, evaluated in **Release** mode using the `libcurl` (`curl_multi` + `CURLPIPE_MULTIPLEX`) C++17 native batch reception engine of `flutter_turbo_fetch`.

For cross-platform comparisons and common evaluation methodology, see [`docs/benchmark-results.md`](../benchmark-results.md) and [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md).

---

## 1. Measurement Environment & Controlled Protocol

### 1.1 Desktop Hardware & OS Environment

| Item | Specification / Observed Value |
| :--- | :--- |
| **OS** | Linux Mint 22.3 (Zena) / Ubuntu 24.04 (`noble`) base, Kernel `7.0.0-43-generic` (64-bit `linux-x64`) |
| **CPU** | AMD Ryzen 7 8845HS w/ Radeon 780M Graphics (8 physical cores / 16 logical processors) |
| **Native Engine** | `libcurl` `8.5.0` (`OpenSSL/3.0.13`, `nghttp2/1.59.0`, `CURL_HTTP_VERSION_2_0`, `CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX`, `CURLOPT_PIPEWAIT = 1L`) on a dedicated single background event loop (`curl_multi_poll` / `curl_multi_wakeup`) |
| **Flutter / Dart SDK** | Flutter 3.47.8 / Dart 3.13.7 (FVM) |
| **Mock Server** | Node.js v24.20.0 Dual-Stack HTTP/2 TLS Server (`example/server/http2_mock_server.mjs`) |
| **Network Interface** | Localhost loopback (`https://127.0.0.1:8443`) |
| **Execution Mode** | Linux Desktop Release AOT (`fvm flutter build linux --release -t lib/main.dart`) |
| **Evidence & Integrity** | 90 measured trials, 8,700 measured requests, 0 failures (excluding 9 warmup trials / 870 requests) |

### 1.2 Split-Pipeline Rigorous Measurement Protocol

Following the common standard established in [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md), the Linux Desktop benchmark employs a **controlled split-pipeline protocol** to eliminate timing asymmetries between streaming and batch adapters:

1. **Warmup & Sample Size (N=10 Trials)**:
   - For each scenario (A, B, C), each adapter executes **1 warmup trial** (90–100 requests) to stabilize TCP/TLS sessions and connection pools, followed by **10 measured trials** (total 90 measured trials / 8,700 measured requests). Warmup data is logged separately and strictly excluded from statistical reporting.
2. **Round-Robin Rotation**:
   - Execution order rotates cyclically across trials (`IO -> dio_http2 -> Turbo`, `dio_http2 -> Turbo -> IO`, `Turbo -> IO -> dio_http2`) to eliminate bias from OS thread scheduling, thermal state, and CPU frequency scaling.
3. **Common Reception Boundary (`ResponseType.bytes`)**:
   - Every adapter receives the complete response payload as bytes (`ResponseType.bytes`). `Network Time` ends strictly upon **complete full-body reception**.
   - Those bytes are then passed to each preset's configured transformer: `BackgroundTransformer` for IO and `dio_http2`, and `TurboTransformer` for `TurboFetch`. `Parse Time` measures stream consolidation, UTF-8/JSON decoding, and any associated Isolate/semaphore handoff.
4. **Server Pre-generation & End-to-End Audit**:
   - All response payloads (5KiB–200KiB) are pre-generated on the mock server at startup.
   - Client request sequences, response byte lengths, HTTP status codes, and negotiated protocols (`1.1` vs `2.0` + ALPN `h2`) are audited 1-to-1 against independent server-side event logs (`server.csv`).

---

## 2. Benchmark Scenario Results

### 2.1 Total Wall-Clock Time and Throughput (10 Trials per Preset)

*All times in milliseconds. Each cell summarizes ten measured trials. RPS is the median requests per second (successful requests / trial wall time).*

| Scenario | Adapter | Median Total | Mean Total | Sample SD | Min | Max | Median req/s |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Scenario A**<br>*(5–45KiB, 3 workers)* | (1) IO (HTTP/1.1) | 841.80 ms | 839.25 ms | 9.46 ms | 820.84 ms | 849.41 ms | 106.91 req/s |
| | (2) dio_http2 | 2,031.97 ms | 2,052.74 ms | 54.74 ms | 1,998.18 ms | 2,152.47 ms | 44.29 req/s |
| | **(3) TurboFetch** | **330.45 ms** | **319.60 ms** | **44.88 ms** | **193.77 ms** | **344.46 ms** | **272.36 req/s** |
| **Scenario B**<br>*(10–100KiB, 5 workers)* | (1) IO (HTTP/1.1) | 442.32 ms | 484.82 ms | 72.55 ms | 416.61 ms | 580.22 ms | 226.08 req/s |
| | (2) dio_http2 | 2,489.34 ms | 2,542.13 ms | 183.63 ms | 2,357.00 ms | 2,850.08 ms | 40.17 req/s |
| | **(3) TurboFetch** | **137.67 ms** | **145.37 ms** | **15.31 ms** | **131.17 ms** | **174.84 ms** | **726.56 req/s** |
| **Scenario C**<br>*(20–200KiB, 10 workers)* | (1) IO (HTTP/1.1) | 317.96 ms | 309.75 ms | 62.79 ms | 208.86 ms | 383.15 ms | 314.51 req/s |
| | (2) dio_http2 | 4,698.20 ms | 4,671.11 ms | 189.59 ms | 4,313.51 ms | 4,883.86 ms | 21.28 req/s |
| | **(3) TurboFetch** | **82.67 ms** | **81.03 ms** | **12.14 ms** | **60.23 ms** | **96.50 ms** | **1,209.82 req/s** |

---

### 2.2 Phase Latency: Network Time vs. Parse Time

*Values in milliseconds; each cell shows the median across 10 trials of that trial's per-request **Mean / P50 / P95**.*

| Scenario | Adapter | Network Time (Mean / P50 / P95) | Parse Time (Mean / P50 / P95) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 27.406 ms / 27.535 ms / 37.876 ms | 0.293 ms / 0.287 ms / 0.554 ms |
| | (2) dio_http2 | 67.036 ms / 74.284 ms / 91.561 ms | 0.302 ms / 0.285 ms / 0.575 ms |
| | **(3) TurboFetch** | **9.689 ms / 9.655 ms / 11.620 ms** | **1.182 ms / 1.631 ms / 2.731 ms** |
| **Scenario B** | (1) IO (HTTP/1.1) | 19.912 ms / 19.795 ms / 30.271 ms | 1.149 ms / 1.232 ms / 2.533 ms |
| | (2) dio_http2 | 119.024 ms / 117.123 ms / 190.579 ms | 1.406 ms / 1.451 ms / 3.087 ms |
| | **(3) TurboFetch** | **5.333 ms / 5.239 ms / 8.287 ms** | **1.456 ms / 1.518 ms / 2.713 ms** |
| **Scenario C** | (1) IO (HTTP/1.1) | 26.520 ms / 26.214 ms / 45.174 ms | 1.686 ms / 1.607 ms / 3.473 ms |
| | (2) dio_http2 | 441.486 ms / 466.104 ms / 521.431 ms | 2.232 ms / 2.490 ms / 4.123 ms |
| | **(3) TurboFetch** | **5.599 ms / 5.069 ms / 9.529 ms** | **2.202 ms / 1.865 ms / 5.003 ms** |

---

### 2.3 Memory Footprint Observations

*RSS values are medians of ten per-trial observations in MiB, sampled via `ProcessInfo.currentRss` every 25 ms. Jank Frames and Max Frame Duration are recorded as N/A because this Release protocol does not instrument frame timing.*

| Scenario | Adapter | Sampled Peak RSS | RSS Delta (Net Allocation) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 217.96 MiB | +1.23 MiB |
| | (2) dio_http2 | 216.84 MiB | +1.54 MiB |
| | **(3) TurboFetch** | **217.54 MiB** | **+4.33 MiB** |
| **Scenario B** | (1) IO (HTTP/1.1) | 283.62 MiB | +5.17 MiB |
| | (2) dio_http2 | 282.35 MiB | +2.26 MiB |
| | **(3) TurboFetch** | **283.52 MiB** | **+14.53 MiB** |
| **Scenario C** | (1) IO (HTTP/1.1) | 395.98 MiB | +9.31 MiB |
| | (2) dio_http2 | 386.85 MiB | +8.88 MiB |
| | **(3) TurboFetch** | **390.69 MiB** | **+19.32 MiB** |

---

## 3. Technical Analysis (Linux / `libcurl` `curl_multi` Event Loop)

### 3.1 Strong Concurrency Scaling: 2.5x–56.8x End-to-End Speedup

Under strict split-pipeline isolation (`N=10` median total wall time), `TurboFetch` consistently outperforms both standard `IO (HTTP/1.1)` and pure-Dart `dio_http2`, with throughput scaling sharply as concurrency rises:

- **Scenario A (5–45KiB, 3 workers)**: **330.45 ms** — **2.5x faster** than `IO` (841.80 ms, -60.7%) and **6.1x faster** than `dio_http2` (2,031.97 ms, -83.7%), reaching **272 req/s**.
- **Scenario B (10–100KiB, 5 workers)**: **137.67 ms** — **3.2x faster** than `IO` (442.32 ms, -68.9%) and **18.1x faster** than `dio_http2` (2,489.34 ms, -94.5%), climbing to **727 req/s**.
- **Scenario C (20–200KiB, 10 workers)**: **82.67 ms** — **3.8x faster** than `IO` (317.96 ms, -74.0%) and **56.8x faster** than `dio_http2` (4,698.20 ms, -98.2%), peaking at **1,210 req/s** while transferring ~11 MiB of JSON per 83 ms trial.

### 3.2 Flat Native Network Latency via Single-Thread `curl_multi` Multiplexing

The split-pipeline `Network Time` isolation highlights the efficiency of the `libcurl` (`OpenSSL` + `nghttp2`) single-thread `curl_multi` event loop:

- In **Scenario C**, per-request median `Network Time` for `(1) IO` scaled linearly with payload size from **8.31 ms (20KiB)** to **43.14 ms (200KiB)**, while `(2) dio_http2` saturated at **461.8–466.5 ms** across all sizes due to single-isolate Dart HTTP/2 framing and its 64KiB initial receive window.
- In contrast, `(3) TurboFetch` remained completely flat from **5.05 ms (20KiB)** to **5.27 ms (200KiB)** — **8.2x faster than `IO`** and **88.5x faster than `dio_http2`** at 200KiB.
- Two architectural properties of the Linux native layer drive this behavior:
  1. **True Single-Connection HTTP/2 Multiplexing (`CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX` + `CURLOPT_PIPEWAIT = 1L`)**: A single `CURLM` handle owned exclusively by one background event-loop thread shares DNS, TLS sessions, and multiplexed HTTP/2 connections with zero lock contention and zero per-request thread spawn overhead.
  2. **Batch Buffer Assembly + GLib Main-Loop Dispatch**: `WriteCallback` appends incoming `DATA` frames into a contiguous `std::vector<uint8_t>` on the event-loop thread and dispatches one complete `Uint8List` via `g_idle_add` over Pigeon, eliminating per-chunk Dart event-loop wakeups.

### 3.3 `TurboTransformer` 24KiB Threshold Behavior

Comparing isolated `Parse Time` across payload steps illustrates the trade-off between `TurboTransformer`'s **24KiB hybrid threshold** and `BackgroundTransformer`'s **50KiB `compute` threshold**:

- **Below 24KiB (5–20KiB)**: `TurboFetch` decodes synchronously on the Main Isolate via fused `utf8.decoder.fuse(json.decoder)`, outperforming `BackgroundTransformer` (e.g., **0.075 ms vs. 0.082 ms** at 5KiB, **0.160 ms vs. 0.276 ms** at 20KiB in Scenario A).
- **25–45KiB Transition Band**: Offloading to a Worker Isolate incurs ~1.85–1.93 ms dispatch overhead versus ~0.32–0.45 ms Main Isolate decoding in `BackgroundTransformer`, though this is outweighed by the large `Network Time` reduction.
- **Above 50KiB (60–200KiB in Scenario C)**: Once `BackgroundTransformer` crosses its 50KiB `compute` threshold, `TurboTransformer`'s zero-copy `TransferableTypedData` handoff and semaphore governance match or beat both baselines (**1.309 ms vs. 1.502 ms / 1.925 ms** at 60KiB, **2.453 ms vs. 2.530 ms / 3.540 ms** at 200KiB).

---

## 4. Reproducing the Benchmark

### 4.1 Prerequisites
1. 64-bit Linux (`linux-x64`) with GTK 3 development headers, CMake, Ninja, `libcurl` (`libcurl4-openssl-dev` with HTTP/2 support), and a C++17 compiler.
2. FVM Flutter `3.47.8`, Node.js, Python 3, and OpenSSL.

### 4.2 Steps

1. **Generate Temporary Local TLS Certificate (with loopback SAN)**:
   ```bash
   mkdir -p /tmp/turbo-linux-bench
   openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 7 \
     -keyout /tmp/turbo-linux-bench/key.pem \
     -out /tmp/turbo-linux-bench/cert.pem \
     -subj "/CN=local-benchmark" \
     -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
   ```

2. **Start Mock Server (in a separate terminal)**:
   ```bash
   HOST=127.0.0.1 PORT=8443 REQUIRE_TLS=1 BENCHMARK_LOG=1 \
     CERT_PATH=/tmp/turbo-linux-bench/cert.pem \
     KEY_PATH=/tmp/turbo-linux-bench/key.pem \
     node example/server/http2_mock_server.mjs > /tmp/turbo-linux-bench/server.jsonl
   ```

3. **Build and Run Linux Release Binary**:
   ```bash
   SHA=$(git rev-parse HEAD)
   cd example
   fvm flutter build linux --release -t lib/main.dart \
     --dart-define=BENCHMARK_BASE_URL=https://127.0.0.1:8443 \
     --dart-define=BENCHMARK_LOCAL_TLS=true \
     --dart-define=BENCHMARK_ENGINE=libcurl \
     --dart-define=BENCHMARK_SOURCE_SHA="$SHA"
   ./build/linux/x64/release/bundle/flutter_turbo_fetch_example
   ```
   Wait until the app displays `Complete: 90 measured trials, 9 warmups` (or `/tmp/turbo-benchmark.jsonl` records `"type":"session_complete"`), then close both the app and the mock server.

4. **Audit, Export, and Summarize Evidence**:
   ```bash
   DATE=$(date +%Y-%m-%d)
   python3 tool/benchmark/export_benchmark.py /tmp/turbo-benchmark.jsonl /tmp/turbo-linux-bench/server.jsonl "tool/benchmark/data/linux/$DATE"
   python3 tool/benchmark/summarize_benchmark.py "tool/benchmark/data/linux/$DATE-raw.csv"
   ```

---

## 5. Raw Evidence & Artifacts

Raw benchmark measurements and server event verification logs are tracked under [`tool/benchmark/data/linux/`](../../tool/benchmark/data/linux/):

- **[Raw Trials CSV](../../tool/benchmark/data/linux/2026-11-23-raw.csv)**: 90 measured trials + 9 warmups with timing, protocol, bytes, engine metadata, and RSS.
- **[Per-Request CSV](../../tool/benchmark/data/linux/2026-11-23-requests.csv)**: 8,700 measured requests (+ 870 warmups) with individual Network/Parse times.
- **[Server Log Evidence](../../tool/benchmark/data/linux/2026-11-23-server.csv)**: Server-side independent matching log entries.
- **[Summary Metrics CSV](../../tool/benchmark/data/linux/2026-11-23-summary.csv)**: Full statistical metrics including Mean, Median, Sample SD, Min, and Max.
