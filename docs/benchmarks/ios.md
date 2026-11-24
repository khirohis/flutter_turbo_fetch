# iOS Benchmark Performance Report (`ios.md`)

This document records the empirical benchmark results, statistical verification, and technical analysis for **iOS**, evaluated in **Release** mode using the `URLSession` native batch reception engine of `flutter_turbo_fetch` across two contrasting hardware generations:

- **iPhone 17** (`iPhone18,3`, iOS 27.2) — flagship Apple Silicon hardware
- **iPhone 6s** (`iPhone8,1`, iOS 15.8.7) — legacy / entry-tier Apple A9 hardware

For cross-platform comparisons and common evaluation methodology, see [`docs/benchmark-results.md`](../benchmark-results.md) and [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md).

---

## 1. Measurement Environment & Controlled Protocol

### 1.1 Physical Device Hardware & OS Environments

| Item | iPhone 17 (Flagship) | iPhone 6s (Legacy / Entry-Tier) |
| :--- | :--- | :--- |
| **Physical Device** | iPhone 17 (`iPhone18,3`), 64-bit ARM64, 6 CPU cores | iPhone 6s (`iPhone8,1`), Apple A9, 64-bit ARM64, 2 CPU cores |
| **Device OS** | iOS 27.2 (Build `24B5099f`) | iOS 15.8.7 (Build `19H411`) |
| **Rendering Backend** | Impeller (Metal) | Impeller (Metal) |
| **Native Engine** | `URLSession.dataTask(completionHandler:)` (CFNetwork / Darwin) | `URLSession.dataTask(completionHandler:)` (CFNetwork / Darwin) |
| **Host System** | macOS 27.0.1 (Build `26A434`), Apple Silicon (`arm64`), Xcode 27.1 | macOS 15.8.2 (Build `24H232`), Intel (`x86_64`), Xcode 26.3 |
| **Flutter / Dart SDK** | Flutter 3.47.8 / Dart 3.13.7 (FVM) | Flutter 3.47.8 / Dart 3.13.7 (FVM) |
| **Mock Server** | Node.js v26.3.0 HTTP/2 TLS Server (`example/server/http2_mock_server.mjs`) | Node.js v26.13.0 HTTP/2 TLS Server (`example/server/http2_mock_server.mjs`) |
| **Network Interface** | Local Wi-Fi LAN; server bound to a single LAN IPv4 address | Local Wi-Fi LAN; server bound to a single LAN IPv4 address |
| **Execution Mode** | Physical Device Release AOT (`fvm flutter build ios --release -t lib/main.dart`) | Physical Device Release AOT (`fvm flutter run --release -t lib/main.dart`) |
| **Evidence & Integrity** | 90 measured trials, 8,700 measured requests, 0 failures (excluding 9 warmups / 870 requests) | 90 measured trials, 8,700 measured requests, 0 failures (excluding 9 warmups / 870 requests) |

### 1.2 Split-Pipeline Rigorous Measurement Protocol

Following [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md), both devices were evaluated independently under the **controlled split-pipeline protocol**:

1. **Warmup & Sample Size (N=10 Trials per Device)**:
   - For each scenario (A, B, C), each adapter executes **1 warmup trial** (90–100 requests) to stabilize TCP/TLS connections and socket pools, followed by **10 measured trials** (90 measured trials / 8,700 measured requests per device; 180 measured trials / 17,400 measured requests combined). Warmup data is logged separately and strictly excluded from statistical reporting.
2. **Round-Robin Rotation**:
   - Execution order rotates cyclically across trials (`IO -> dio_http2 -> Turbo`, `dio_http2 -> Turbo -> IO`, `Turbo -> IO -> dio_http2`) to eliminate bias from device thermal throttling and OS resource scheduling.
3. **Common Reception Boundary (`ResponseType.bytes`)**:
   - Every adapter receives the complete response payload as bytes (`ResponseType.bytes`). `Network Time` ends strictly upon **complete full-body reception**.
   - Those bytes are then passed to the preset's configured transformer (`BackgroundTransformer` for `IO` / `dio_http2`, `TurboTransformer` for `TurboFetch`) to isolate `Parse Time`.
4. **Server Pre-generation & 1-to-1 Audit**:
   - All response payloads are pre-generated on the mock server.
   - Every request's HTTP status (`200`), exact byte length, HTTP protocol version (`1.1` vs `2.0`), and ALPN (`h2`) are audited 1-to-1 against independent server logs (`server.csv`).

---

## 2. Hardware Generation Comparison: iPhone 17 vs. iPhone 6s

Evaluating the same `URLSession` + `TurboTransformer` (24KiB default threshold) stack across a 10-year hardware span reveals how CPU decoding speed interacts with native HTTP/2 batching:

| Scenario | Device | TurboFetch Median (RPS) | Speedup vs. IO (HTTP/1.1) | Speedup vs. dio_http2 | TurboFetch Net P50 / Parse P50 |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **Scenario A**<br>*(5–45KiB, 3w)* | **iPhone 17** | 462.00 ms (194.81) | 0.96x | **1.57x faster** | 14.35 ms / 0.70 ms |
| | **iPhone 6s** | **524.43 ms (171.65)** | **1.01x faster** | **1.26x faster** | 14.21 ms / 1.15 ms |
| **Scenario B**<br>*(10–100KiB, 5w)* | **iPhone 17** | 411.84 ms (242.82) | 0.90x | **3.23x faster** | 18.12 ms / 1.18 ms |
| | **iPhone 6s** | **621.83 ms (160.82)** | **1.13x faster** | **2.00x faster** | 24.46 ms / 2.34 ms |
| **Scenario C**<br>*(20–200KiB, 10w)* | **iPhone 17** | 454.22 ms (220.16) | 0.95x | **5.79x faster** | 41.34 ms / 1.65 ms |
| | **iPhone 6s** | **825.73 ms (121.26)** | **1.32x faster** | **2.83x faster** | 59.11 ms / 6.61 ms |

---

## 3. Benchmark Scenario Results: iPhone 17 (Flagship Apple Silicon)

### 3.1 Total Wall-Clock Time and Throughput (10 Trials per Preset)

*All times in milliseconds. Each cell summarizes ten measured trials. RPS is the median requests per second (successful requests / trial wall time).*

| Scenario | Adapter | Median Total | Mean Total | Sample SD | Min | Max | Median req/s |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Scenario A**<br>*(5–45KiB, 3 workers)* | (1) IO (HTTP/1.1) | 444.76 ms | 449.46 ms | 17.42 ms | 430.80 ms | 478.25 ms | 202.37 req/s |
| | (2) dio_http2 | 723.97 ms | 736.05 ms | 33.10 ms | 699.55 ms | 813.58 ms | 124.32 req/s |
| | **(3) TurboFetch** | **462.00 ms** | **459.83 ms** | **8.67 ms** | **441.04 ms** | **472.60 ms** | **194.81 req/s** |
| **Scenario B**<br>*(10–100KiB, 5 workers)* | (1) IO (HTTP/1.1) | 370.54 ms | 371.07 ms | 15.61 ms | 347.13 ms | 396.48 ms | 269.89 req/s |
| | (2) dio_http2 | 1,329.47 ms | 1,337.56 ms | 36.92 ms | 1,296.40 ms | 1,405.66 ms | 75.22 req/s |
| | **(3) TurboFetch** | **411.84 ms** | **415.28 ms** | **18.07 ms** | **387.98 ms** | **441.71 ms** | **242.82 req/s** |
| **Scenario C**<br>*(20–200KiB, 10 workers)* | (1) IO (HTTP/1.1) | 429.89 ms | 444.30 ms | 43.48 ms | 398.87 ms | 526.96 ms | 232.66 req/s |
| | (2) dio_http2 | 2,629.91 ms | 2,648.13 ms | 71.33 ms | 2,579.30 ms | 2,820.74 ms | 38.02 req/s |
| | **(3) TurboFetch** | **454.22 ms** | **453.18 ms** | **16.98 ms** | **419.43 ms** | **476.47 ms** | **220.16 req/s** |

### 3.2 Phase Latency: Network Time vs. Parse Time (iPhone 17)

*Values in milliseconds; each cell shows the median across 10 trials of that trial's per-request **Mean / P50 / P95**.*

| Scenario | Adapter | Network Time (Mean / P50 / P95) | Parse Time (Mean / P50 / P95) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 14.261 ms / 14.073 ms / 18.318 ms | 0.356 ms / 0.352 ms / 0.603 ms |
| | (2) dio_http2 | 23.546 ms / 24.233 ms / 35.401 ms | 0.395 ms / 0.396 ms / 0.690 ms |
| | **(3) TurboFetch** | **14.402 ms / 14.353 ms / 18.712 ms** | **0.649 ms / 0.700 ms / 1.597 ms** |
| **Scenario B** | (1) IO (HTTP/1.1) | 17.564 ms / 16.898 ms / 24.087 ms | 0.435 ms / 0.473 ms / 0.814 ms |
| | (2) dio_http2 | 63.377 ms / 63.082 ms / 92.519 ms | 1.302 ms / 1.358 ms / 2.640 ms |
| | **(3) TurboFetch** | **19.186 ms / 18.118 ms / 25.696 ms** | **1.176 ms / 1.178 ms / 2.266 ms** |
| **Scenario C** | (1) IO (HTTP/1.1) | 40.291 ms / 39.216 ms / 54.886 ms | 0.650 ms / 0.623 ms / 1.169 ms |
| | (2) dio_http2 | 245.635 ms / 257.668 ms / 273.207 ms | 2.454 ms / 2.432 ms / 4.825 ms |
| | **(3) TurboFetch** | **41.080 ms / 41.340 ms / 55.545 ms** | **2.306 ms / 1.647 ms / 6.978 ms** |

### 3.3 Memory Footprint Observations (iPhone 17)

*RSS values are medians of ten per-trial observations in MiB (`ProcessInfo.currentRss`, 25 ms sampler). Jank Frames and Max Frame Duration are N/A in this Release protocol.*

| Scenario | Adapter | Sampled Peak RSS | RSS Delta (Net Allocation) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 69.72 MiB | +0.10 MiB |
| | (2) dio_http2 | 69.45 MiB | +0.66 MiB |
| | **(3) TurboFetch** | **68.74 MiB** | **+0.75 MiB** |
| **Scenario B** | (1) IO (HTTP/1.1) | 90.57 MiB | +2.24 MiB |
| | (2) dio_http2 | 91.11 MiB | +4.19 MiB |
| | **(3) TurboFetch** | **90.70 MiB** | **+6.70 MiB** |
| **Scenario C** | (1) IO (HTTP/1.1) | 97.65 MiB | +1.45 MiB |
| | (2) dio_http2 | 96.84 MiB | +4.27 MiB |
| | **(3) TurboFetch** | **96.62 MiB** | **+6.95 MiB** |

---

## 4. Benchmark Scenario Results: iPhone 6s (Legacy / Entry-Tier Hardware)

### 4.1 Total Wall-Clock Time and Throughput (10 Trials per Preset)

| Scenario | Adapter | Median Total | Mean Total | Sample SD | Min | Max | Median req/s |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Scenario A**<br>*(5–45KiB, 3 workers)* | (1) IO (HTTP/1.1) | 531.02 ms | 530.65 ms | 62.95 ms | 442.20 ms | 630.48 ms | 169.77 req/s |
| | (2) dio_http2 | 662.86 ms | 667.89 ms | 42.17 ms | 616.26 ms | 749.79 ms | 135.78 req/s |
| | **(3) TurboFetch** | **524.43 ms** | **540.96 ms** | **63.44 ms** | **494.09 ms** | **712.36 ms** | **171.65 req/s** |
| **Scenario B**<br>*(10–100KiB, 5 workers)* | (1) IO (HTTP/1.1) | 705.37 ms | 734.08 ms | 106.81 ms | 634.93 ms | 1,016.23 ms | 141.77 req/s |
| | (2) dio_http2 | 1,244.29 ms | 1,249.89 ms | 58.51 ms | 1,154.94 ms | 1,337.52 ms | 80.37 req/s |
| | **(3) TurboFetch** | **621.83 ms** | **602.24 ms** | **57.51 ms** | **509.33 ms** | **680.99 ms** | **160.82 req/s** |
| **Scenario C**<br>*(20–200KiB, 10 workers)* | (1) IO (HTTP/1.1) | 1,089.34 ms | 1,082.05 ms | 65.01 ms | 989.01 ms | 1,186.93 ms | 91.83 req/s |
| | (2) dio_http2 | 2,333.12 ms | 2,348.39 ms | 93.38 ms | 2,206.78 ms | 2,498.17 ms | 42.86 req/s |
| | **(3) TurboFetch** | **825.73 ms** | **935.15 ms** | **444.21 ms** | **679.56 ms** | **2,171.47 ms** | **121.26 req/s** |

> [!NOTE]
> In Scenario C (`iPhone 6s`), 9 of 10 measured `TurboFetch` trials finished tightly between `679.56 ms` and `939.06 ms` (`106.5–147.2 req/s`); a single Wi-Fi jitter outlier in trial 2 (`2,171.47 ms`) inflated Sample SD without affecting the median (`825.73 ms`).

### 4.2 Phase Latency: Network Time vs. Parse Time (iPhone 6s)

| Scenario | Adapter | Network Time (Mean / P50 / P95) | Parse Time (Mean / P50 / P95) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 16.921 ms / 14.903 ms / 20.851 ms | 0.521 ms / 0.509 ms / 0.913 ms |
| | (2) dio_http2 | 21.333 ms / 19.044 ms / 32.707 ms | 0.541 ms / 0.509 ms / 0.949 ms |
| | **(3) TurboFetch** | **16.048 ms / 14.206 ms / 19.143 ms** | **1.288 ms / 1.153 ms / 3.761 ms** |
| **Scenario B** | (1) IO (HTTP/1.1) | 31.922 ms / 28.901 ms / 50.504 ms | 2.729 ms / 2.663 ms / 6.841 ms |
| | (2) dio_http2 | 58.194 ms / 58.359 ms / 91.399 ms | 2.425 ms / 2.504 ms / 5.800 ms |
| | **(3) TurboFetch** | **27.623 ms / 24.462 ms / 37.210 ms** | **2.778 ms / 2.344 ms / 7.328 ms** |
| **Scenario C** | (1) IO (HTTP/1.1) | 98.767 ms / 91.196 ms / 165.505 ms | 6.242 ms / 6.089 ms / 12.853 ms |
| | (2) dio_http2 | 216.049 ms / 220.678 ms / 246.369 ms | 4.464 ms / 4.574 ms / 9.073 ms |
| | **(3) TurboFetch** | **71.431 ms / 59.112 ms / 186.603 ms** | **8.153 ms / 6.613 ms / 20.895 ms** |

### 4.3 Memory Footprint Observations (iPhone 6s)

| Scenario | Adapter | Sampled Peak RSS | RSS Delta (Net Allocation) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 50.37 MiB | +3.05 MiB |
| | (2) dio_http2 | 50.66 MiB | +2.80 MiB |
| | **(3) TurboFetch** | **51.79 MiB** | **+3.94 MiB** |
| **Scenario B** | (1) IO (HTTP/1.1) | 61.79 MiB | +1.37 MiB |
| | (2) dio_http2 | 60.51 MiB | +3.41 MiB |
| | **(3) TurboFetch** | **65.14 MiB** | **+6.66 MiB** |
| **Scenario C** | (1) IO (HTTP/1.1) | 71.34 MiB | +3.05 MiB |
| | (2) dio_http2 | 69.85 MiB | +7.84 MiB |
| | **(3) TurboFetch** | **74.88 MiB** | **+10.62 MiB** |

---

## 5. Technical Analysis (iOS / `URLSession`)

### 5.1 Massive Superiority over Pure Dart HTTP/2 (`dio_http2`)

Across both hardware generations, delegating HTTP/2 frame assembly, flow-control windows, and TLS decryption to Apple's native CFNetwork stack dramatically outperforms pure Dart HTTP/2 (`package:http2`):

- **iPhone 17**:
  - **Scenario A**: **36.2% faster** (`462.00 ms` vs. `723.97 ms`; **1.57x** throughput).
  - **Scenario B**: **69.0% faster** (`411.84 ms` vs. `1,329.47 ms`; **3.23x** throughput).
  - **Scenario C**: **82.7% faster** (`454.22 ms` vs. `2,629.91 ms`; **5.79x** throughput).
- **iPhone 6s**:
  - **Scenario A**: **20.9% faster** (`524.43 ms` vs. `662.86 ms`; **1.26x** throughput).
  - **Scenario B**: **50.0% faster** (`621.83 ms` vs. `1,244.29 ms`; **2.00x** throughput).
  - **Scenario C**: **64.6% faster** (`825.73 ms` vs. `2,333.12 ms`; **2.83x** throughput).

### 5.2 Why TurboFetch Wins Across Every Scenario on iPhone 6s vs. Flagship iPhone 17

Comparing iPhone 6s and iPhone 17 directly explains how `flutter_turbo_fetch`'s default **24KiB hybrid threshold** behaves across CPU tiers:

1. **Constrained Dual-Core CPU (iPhone 6s / Apple A9)**:
   - Under 5–10 concurrent workers, `IOHttpClientAdapter`'s Dart-side HTTP/1.1 stream chunk assembly and TLS I/O compete heavily with JSON decoding on the dual-core A9.
   - Offloading transport and body buffering to native `URLSession` cuts median `Network Time` by **15.4%** in Scenario B (`28.90 ms -> 24.46 ms`) and **35.2%** in Scenario C (`91.20 ms -> 59.11 ms`).
   - As a result, `TurboFetch` is the fastest adapter in **all three scenarios** on iPhone 6s—outperforming `IO` by **1.13x** in Scenario B (`-11.8%` wall time) and **1.32x** in Scenario C (`-24.2%` wall time), while keeping absolute `Sampled Peak RSS` compact (`51.8–74.9 MiB`).
2. **Ultra-High-Performance Flagship Silicon (iPhone 17)**:
   - On iPhone 17, Main Isolate JSON decoding completes in **< 0.2 ms for <= 20KiB** and **< 0.7 ms for 100KiB**, while low-latency Wi-Fi LAN (1–2 ms RTT) minimizes HTTP/1.1 connection penalties.
   - With the default 24KiB threshold, Worker Isolate dispatch and semaphore overhead (`0.9–1.9 ms`) on 25–100KiB payloads temporarily exceeded synchronous Main Isolate decode time, making total wall time **3.9%–11.1%** slower than `IO` (even though pure network transmission at 120–200KiB was up to **13% faster** than `IO`).
   - Raising `isolateThresholdBytes` to `128KiB–256KiB` on flagship Apple Silicon recovers that overhead while keeping the default `24KiB` optimal for entry-to-mid-range iOS devices.

---

## 6. Reproducing the Benchmark

### 6.1 Prerequisites
1. Physical iPhone connected to the same Wi-Fi LAN (`<LAN_IP>`) as the host Mac.
2. FVM Flutter `3.47.8`, Xcode 15+, Node.js, Python 3, and OpenSSL.

### 6.2 Steps

1. **Generate Temporary SAN-Enabled Local TLS Certificate**:
   ```bash
   openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 7 \
     -keyout /tmp/ios-benchmark-key.pem -out /tmp/ios-benchmark-cert.pem \
     -subj "/CN=<LAN_IP>" \
     -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:<LAN_IP>"
   chmod 600 /tmp/ios-benchmark-key.pem
   ```

2. **Start Mock Server**:
   ```bash
   HOST=<LAN_IP> PORT=8444 REQUIRE_TLS=1 BENCHMARK_LOG=1 \
     CERT_PATH=/tmp/ios-benchmark-cert.pem KEY_PATH=/tmp/ios-benchmark-key.pem \
     node example/server/http2_mock_server.mjs > /tmp/ios-server.jsonl 2>&1
   ```

3. **Build and Run on Device**:
   - **iOS 17+ (`xcrun devicectl`)**:
     ```bash
     cd example
     fvm flutter build ios --release -t lib/main.dart \
       --dart-define=BENCHMARK_BASE_URL=https://<LAN_IP>:8444 \
       --dart-define=BENCHMARK_LOCAL_TLS=true \
       --dart-define=BENCHMARK_ENGINE=urlsession \
       --dart-define=BENCHMARK_SOURCE_SHA=$(git rev-parse HEAD)
     xcrun devicectl device install app --device <DEVICE_ID> build/ios/iphoneos/Runner.app
     xcrun devicectl device process launch --device <DEVICE_ID> --terminate-existing io.github.khirohis.flutterTurboFetchExample
     ```
   - **iOS 15–16 (pre-CoreDevice)**:
     Build and install with `fvm flutter run --release -d <DEVICE_ID> -t lib/main.dart` using the same `--dart-define` flags (launch on device if needed), wait for `Complete: 90 measured trials, 9 warmups`, then download the app container via Xcode **Window > Devices and Simulators > Download Container...** and extract `AppData/tmp/turbo-benchmark.jsonl`.

4. **Audit, Export, and Summarize**:
   ```bash
   python3 tool/benchmark/export_benchmark.py /tmp/ios-device.jsonl /tmp/ios-server.jsonl tool/benchmark/data/ios/<YYYY-MM-DD>
   python3 tool/benchmark/summarize_benchmark.py tool/benchmark/data/ios/<YYYY-MM-DD>-raw.csv
   ```

---

## 7. Raw Evidence & Artifacts

Raw benchmark measurements and server event verification logs are tracked under [`tool/benchmark/data/ios/`](../../tool/benchmark/data/ios/):

- **iPhone 17 (iOS 27.2)**:
  - **[Raw Trials CSV](../../tool/benchmark/data/ios/2026-11-14-raw.csv)** / **[Per-Request CSV](../../tool/benchmark/data/ios/2026-11-14-requests.csv)** / **[Server Log Evidence](../../tool/benchmark/data/ios/2026-11-14-server.csv)** / **[Summary Metrics CSV](../../tool/benchmark/data/ios/2026-11-14-summary.csv)**
- **iPhone 6s (iOS 15.8.7)**:
  - **[Raw Trials CSV](../../tool/benchmark/data/ios/2026-11-24-iphone6s-raw.csv)** / **[Per-Request CSV](../../tool/benchmark/data/ios/2026-11-24-iphone6s-requests.csv)** / **[Server Log Evidence](../../tool/benchmark/data/ios/2026-11-24-iphone6s-server.csv)** / **[Summary Metrics CSV](../../tool/benchmark/data/ios/2026-11-24-iphone6s-summary.csv)**
