# Windows Benchmark Performance Report (`windows.md`)

This document records the empirical benchmark results, statistical verification, and technical analysis for **Windows Desktop**, evaluated in **Release** mode using the `WinHTTP` C++17 native batch reception engine of `flutter_turbo_fetch`.

For cross-platform comparisons and common evaluation methodology, see [`docs/benchmark-results.md`](../benchmark-results.md) and [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md).

---

## 1. Measurement Environment & Controlled Protocol

### 1.1 Desktop Hardware & OS Environment

| Item | Specification / Observed Value |
| :--- | :--- |
| **OS** | Microsoft Windows 11 Pro 26H2 (Version `10.0.26300.10075`, 64-bit `windows-x64`) |
| **CPU** | AMD Ryzen 7 8845HS w/ Radeon 780M Graphics (8 physical cores / 16 logical processors) |
| **Native Engine** | Windows OS Standard `WinHTTP` (`winhttp.dll`, `WINHTTP_PROTOCOL_FLAG_HTTP2`) + Windows Thread Pool (`TP_POOL` / `TP_CLEANUP_GROUP`) via `TurboSessionManager` and `TurboHttpExecutor` |
| **Flutter / Dart SDK** | Flutter 3.47.8 / Dart 3.13.7 (FVM) |
| **Mock Server** | Node.js v26.12.1 Dual-Stack HTTP/2 TLS Server (`example/server/http2_mock_server.mjs`) |
| **Network Interface** | Localhost loopback (`https://127.0.0.1:8443`) |
| **Execution Mode** | Windows Desktop Release AOT (`fvm flutter build windows --release -t lib/main.dart`) |
| **Evidence & Integrity** | 90 measured trials, 8,700 measured requests, 0 failures (excluding 9 warmup trials / 870 requests) |

### 1.2 Split-Pipeline Rigorous Measurement Protocol

Following the common standard established in the iOS evaluation, the Windows Desktop benchmark employs a **controlled split-pipeline protocol** to eliminate timing asymmetries between streaming and batch adapters:

1. **Warmup & Sample Size (N=10 Trials)**:
   - For each scenario (A, B, C), each adapter executes **1 warmup trial** (90–100 requests) to stabilize TCP/TLS sessions and connection pools, followed by **10 measured trials** (total 90 measured trials / 8,700 measured requests). Warmup data is logged separately and strictly excluded from statistical reporting.
2. **Round-Robin Rotation**:
   - Execution order rotates cyclically across trials (`IO -> dio_http2 -> Turbo`, `dio_http2 -> Turbo -> IO`, `Turbo -> IO -> dio_http2`) to eliminate bias from OS thread scheduling, thermal state, and CPU boost behavior.
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
| **Scenario A**<br>*(5–45KiB, 3 workers)* | (1) IO (HTTP/1.1) | 77.87 ms | 77.43 ms | 3.86 ms | 71.91 ms | 81.83 ms | 1,156.30 req/s |
| | (2) dio_http2 | 108.64 ms | 111.71 ms | 8.33 ms | 99.72 ms | 125.32 ms | 828.51 req/s |
| | **(3) TurboFetch** | **38.90 ms** | **39.25 ms** | **2.22 ms** | **36.12 ms** | **43.97 ms** | **2,313.48 req/s** |
| **Scenario B**<br>*(10–100KiB, 5 workers)* | (1) IO (HTTP/1.1) | 111.66 ms | 111.98 ms | 6.84 ms | 101.76 ms | 122.55 ms | 895.74 req/s |
| | (2) dio_http2 | 234.08 ms | 235.10 ms | 14.15 ms | 213.90 ms | 267.01 ms | 427.28 req/s |
| | **(3) TurboFetch** | **40.79 ms** | **40.48 ms** | **2.07 ms** | **37.37 ms** | **43.56 ms** | **2,451.50 req/s** |
| **Scenario C**<br>*(20–200KiB, 10 workers)* | (1) IO (HTTP/1.1) | 169.94 ms | 171.35 ms | 5.91 ms | 163.69 ms | 182.46 ms | 588.48 req/s |
| | (2) dio_http2 | 390.91 ms | 387.55 ms | 10.12 ms | 369.25 ms | 398.00 ms | 255.83 req/s |
| | **(3) TurboFetch** | **51.21 ms** | **53.62 ms** | **7.72 ms** | **45.33 ms** | **67.71 ms** | **1,954.57 req/s** |

---

### 2.2 Phase Latency: Network Time vs. Parse Time

*Values in milliseconds; each cell shows the median across 10 trials of that trial's per-request **Mean / P50 / P95**.*

| Scenario | Adapter | Network Time (Mean / P50 / P95) | Parse Time (Mean / P50 / P95) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 2.354 ms / 2.279 ms / 4.005 ms | 0.216 ms / 0.188 ms / 0.455 ms |
| | (2) dio_http2 | 3.377 ms / 3.095 ms / 5.702 ms | 0.208 ms / 0.182 ms / 0.441 ms |
| | **(3) TurboFetch** | **0.972 ms / 0.807 ms / 1.969 ms** | **0.299 ms / 0.273 ms / 0.794 ms** |
| **Scenario B** | (1) IO (HTTP/1.1) | 4.672 ms / 4.710 ms / 8.026 ms | 0.787 ms / 0.805 ms / 1.755 ms |
| | (2) dio_http2 | 10.591 ms / 10.510 ms / 15.396 ms | 0.772 ms / 0.838 ms / 1.584 ms |
| | **(3) TurboFetch** | **1.415 ms / 1.236 ms / 2.645 ms** | **0.590 ms / 0.500 ms / 1.358 ms** |
| **Scenario C** | (1) IO (HTTP/1.1) | 14.832 ms / 14.752 ms / 26.794 ms | 1.642 ms / 1.429 ms / 3.726 ms |
| | (2) dio_http2 | 35.404 ms / 36.445 ms / 41.139 ms | 1.399 ms / 1.333 ms / 2.747 ms |
| | **(3) TurboFetch** | **3.536 ms / 3.184 ms / 8.370 ms** | **1.199 ms / 0.908 ms / 3.591 ms** |

---

### 2.3 Memory Footprint Observations

*RSS values are medians of ten per-trial observations in MiB, sampled via `ProcessInfo.currentRss` every 25 ms. Jank Frames and Max Frame Duration are recorded as N/A because this Release protocol does not instrument frame timing.*

| Scenario | Adapter | Sampled Peak RSS | RSS Delta (Net Allocation) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 118.57 MiB | +1.07 MiB |
| | (2) dio_http2 | 118.09 MiB | +0.16 MiB |
| | **(3) TurboFetch** | **119.29 MiB** | **+3.35 MiB** |
| **Scenario B** | (1) IO (HTTP/1.1) | 130.16 MiB | +0.00 MiB |
| | (2) dio_http2 | 128.23 MiB | +1.22 MiB |
| | **(3) TurboFetch** | **131.69 MiB** | **+12.48 MiB** |
| **Scenario C** | (1) IO (HTTP/1.1) | 137.22 MiB | +2.13 MiB |
| | (2) dio_http2 | 135.49 MiB | +0.26 MiB |
| | **(3) TurboFetch** | **140.88 MiB** | **+11.01 MiB** |

---

## 3. Technical Analysis (Windows / WinHTTP + OS Thread Pool)

### 3.1 Consistent 2.0x–7.6x End-to-End Speedup Across All Workloads

Under strict split-pipeline isolation (`N=10` median total wall time), `TurboFetch` decisively outperforms both standard `IO (HTTP/1.1)` and pure-Dart `dio_http2`:

- **Scenario A (5–45KiB, 3 workers)**: **38.90 ms** — **2.0x faster** than `IO` (77.87 ms, -50.0%) and **2.8x faster** than `dio_http2` (108.64 ms, -64.2%), reaching **2,313 req/s**.
- **Scenario B (10–100KiB, 5 workers)**: **40.79 ms** — **2.7x faster** than `IO` (111.66 ms, -63.5%) and **5.7x faster** than `dio_http2` (234.08 ms, -82.6%), peaking at **2,452 req/s**.
- **Scenario C (20–200KiB, 10 workers)**: **51.21 ms** — **3.3x faster** than `IO` (169.94 ms, -69.9%) and **7.6x faster** than `dio_http2` (390.91 ms, -86.9%), sustaining **1,955 req/s** while transferring ~11 MiB of JSON per 51 ms trial.

### 3.2 Flat Native Network Latency via WinHTTP + `TP_POOL`

The split-pipeline `Network Time` isolation exposes why `WinHTTP` scales so well under desktop concurrency:

- In **Scenario C**, per-request median `Network Time` for `(1) IO` scaled linearly with payload size from **4.36 ms (20KiB)** to **25.61 ms (200KiB)**, and `(2) dio_http2` remained stalled at **35.2–37.5 ms** across all sizes due to single-isolate Dart HTTP/2 framing and its 64KiB initial receive window.
- In contrast, `(3) TurboFetch` stayed nearly flat from **2.99 ms (20KiB)** to **3.47 ms (200KiB)** — **7.4x faster than `IO`** and **10.6x faster than `dio_http2`** at 200KiB.
- This is driven by two architectural properties of the Windows native layer:
  1. **Kernel-Assisted SChannel TLS & Shared `HINTERNET` Session**: `TurboSessionManager` reuses a single HTTP/2-enabled `HINTERNET` session across concurrent requests, avoiding repeated TLS handshakes and multiplexing streams natively.
  2. **OS Thread Pool (`TP_POOL`) Batch Reception**: `TurboHttpExecutor` reads 64KiB blocks across Windows thread pool workers into pre-allocated contiguous `std::vector<uint8_t>` buffers, delivering one complete `Uint8List` over Pigeon with zero per-chunk Dart event-loop wakeups.

### 3.3 Zero-Copy `TurboTransformer` Advantage Above 50KiB

Comparing `Parse Time` in isolation highlights the exact behavior of `TurboTransformer`'s **24KiB hybrid threshold** vs. `BackgroundTransformer`'s **50KiB `compute` threshold**:

- **Small payloads (Scenario A, 5–45KiB)**: `TurboFetch` is faster below 24KiB (e.g., **0.049 ms vs. 0.088 ms** at 5KiB due to the fused `utf8.decoder.fuse(json.decoder)`), while incurring modest Isolate dispatch overhead between 25KiB and 45KiB (~0.36–0.40 ms vs. ~0.20–0.25 ms synchronous Main Isolate decode).
- **Medium & large payloads (Scenarios B & C, 50KiB–200KiB)**: Once `BackgroundTransformer` crosses its 50KiB `compute` threshold, `TurboTransformer` becomes consistently faster across every payload step — achieving **0.500 ms vs. 0.805 ms (IO) / 0.838 ms (dio_http2)** in Scenario B (P50) and **0.908 ms vs. 1.429 ms (IO) / 1.333 ms (dio_http2)** in Scenario C (P50; **1.456 ms vs. 2.436 ms** at 200KiB) thanks to zero-copy `TransferableTypedData` transfer and semaphore-governed concurrency (`maxConcurrentIsolates`).

---

## 4. Reproducing the Benchmark

### 4.1 Prerequisites
1. Windows 10 (1809+) or Windows 11 64-bit (`windows-x64`) with Visual Studio C++ desktop build tools.
2. FVM Flutter `3.47.8`, Node.js, Python 3, and OpenSSL (e.g. Git for Windows `openssl.exe`).

### 4.2 Steps

1. **Generate Temporary Local TLS Certificate (with loopback SAN)**:
   ```powershell
   New-Item -ItemType Directory -Force -Path "$env:TEMP\turbo-win-bench" | Out-Null
   & "C:\Program Files\Git\usr\bin\openssl.exe" req -x509 -newkey rsa:2048 -sha256 -nodes -days 7 `
     -keyout "$env:TEMP\turbo-win-bench\key.pem" -out "$env:TEMP\turbo-win-bench\cert.pem" `
     -subj "/CN=local-benchmark" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
   ```

2. **Start Mock Server (in a separate terminal)**:
   ```powershell
   $env:HOST = "127.0.0.1"
   $env:PORT = "8443"
   $env:REQUIRE_TLS = "1"
   $env:BENCHMARK_LOG = "1"
   $env:CERT_PATH = "$env:TEMP\turbo-win-bench\cert.pem"
   $env:KEY_PATH = "$env:TEMP\turbo-win-bench\key.pem"
   node example/server/http2_mock_server.mjs > "$env:TEMP\turbo-win-bench\server.jsonl"
   ```

3. **Build and Run Windows Release Binary**:
   ```powershell
   $sha = git rev-parse HEAD
   cd example
   fvm flutter build windows --release -t lib/main.dart `
     --dart-define=BENCHMARK_BASE_URL=https://127.0.0.1:8443 `
     --dart-define=BENCHMARK_LOCAL_TLS=true `
     --dart-define=BENCHMARK_ENGINE=winhttp `
     --dart-define=BENCHMARK_SOURCE_SHA=$sha
   .\build\windows\x64\runner\Release\flutter_turbo_fetch_example.exe
   ```
   Wait until the app displays `Complete: 90 measured trials, 9 warmups`, then close both the app and the mock server.

4. **Audit, Export, and Summarize Evidence**:
   ```powershell
   $date = Get-Date -Format "yyyy-MM-dd"
   python tool/benchmark/export_benchmark.py "$env:TEMP\turbo-benchmark.jsonl" "$env:TEMP\turbo-win-bench\server.jsonl" "tool/benchmark/data/windows/$date"
   python tool/benchmark/summarize_benchmark.py "tool/benchmark/data/windows/$date-raw.csv"
   ```

---

## 5. Raw Evidence & Artifacts

Raw benchmark measurements and server event verification logs are tracked under [`tool/benchmark/data/windows/`](../../tool/benchmark/data/windows/):

- **[Raw Trials CSV](../../tool/benchmark/data/windows/2026-11-23-raw.csv)**: 90 measured trials + 9 warmups with timing, protocol, bytes, engine metadata, and RSS.
- **[Per-Request CSV](../../tool/benchmark/data/windows/2026-11-23-requests.csv)**: 8,700 measured requests (+ 870 warmups) with individual Network/Parse times.
- **[Server Log Evidence](../../tool/benchmark/data/windows/2026-11-23-server.csv)**: Server-side independent matching log entries.
- **[Summary Metrics CSV](../../tool/benchmark/data/windows/2026-11-23-summary.csv)**: Full statistical metrics including Mean, Median, Sample SD, Min, and Max.
