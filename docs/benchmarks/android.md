# Android Benchmark Performance Report (`android.md`)

This document records the empirical benchmark results, statistical verification, and technical analysis for **Android**, evaluated on a **Pixel 10a physical device** in **Release** mode across both native transport engines supported by `flutter_turbo_fetch`:

- **`OkHttp3`** (repository default, `com.squareup.okhttp3:okhttp:4.12.0`)
- **`Cronet`** (optional build-time engine, `org.chromium.net:cronet-api:143.7445.0` + `cronet-embedded:143.7445.0`)

For cross-platform comparisons and common evaluation methodology, see [`docs/benchmark-results.md`](../benchmark-results.md) and [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md).

---

## 1. Measurement Environment & Controlled Protocol

### 1.1 Physical Device Hardware & OS Environment

| Item | Specification / Observed Value |
| :--- | :--- |
| **Device / SoC** | Google Pixel 10a (`arm64-v8a`, Google Tensor G4) |
| **OS** | Android 17 (API 37) |
| **Native Engines** | (1) `OkHttp3` `4.12.0` (`TurboClientManager` shared `ConnectionPool`, 16MiB HTTP/2 window, default)<br>(2) `Cronet` `143.7445.0` (`cronet-api` + `cronet-embedded`, Chromium C++ HTTP/2 + BoringSSL) |
| **Release APK Size** | `46.7 MB` (`OkHttp3`) / `63.8 MB` (`Cronet` with `cronet-embedded`, `+17.1 MB`) |
| **Flutter / Dart SDK** | Flutter 3.47.8 / Dart 3.13.7 (FVM) |
| **Mock Server** | Node.js v26.12.1 Dual-Stack HTTP/2 TLS Server (`example/server/http2_mock_server.mjs`) |
| **Network Interface** | Local Wi-Fi LAN (`https://<LAN_IP>:8444`) — identical network class to the iOS physical-device benchmark |
| **Execution Mode** | Android Release AOT (`fvm flutter build apk --release -t lib/main.dart`), screen kept awake throughout each session |
| **Evidence & Integrity** | Per engine: 90 measured trials, 8,700 measured requests, 0 failures (excluding 9 warmup trials / 870 requests) |

### 1.2 Split-Pipeline Rigorous Measurement Protocol

Following [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md), both Android engines were evaluated independently under the **controlled split-pipeline protocol**:

1. **Warmup & Sample Size (N=10 Trials per Engine)**:
   - For each scenario (A, B, C), each adapter executes **1 warmup trial** (90–100 requests) followed by **10 measured trials** (90 measured trials / 8,700 measured requests per engine; 180 measured trials / 17,400 measured requests combined). Warmup data is excluded from statistical summaries.
2. **Round-Robin Rotation**:
   - Execution order rotates cyclically across trials (`IO -> dio_http2 -> Turbo`, `dio_http2 -> Turbo -> IO`, `Turbo -> IO -> dio_http2`) to eliminate thermal, radio, and scheduler bias.
3. **Common Reception Boundary (`ResponseType.bytes`)**:
   - Every adapter receives the full response body as bytes (`ResponseType.bytes`). `Network Time` ends strictly upon **complete full-body reception**.
   - Those bytes are then passed to `BackgroundTransformer` (`IO` / `dio_http2`) or `TurboTransformer` (`TurboFetch`) to isolate `Parse Time`.
4. **TLS Verification Parity**:
   - `OkHttp3` uses `allowBadCertificates: true` (`BENCHMARK_LOCAL_TLS=true`) against a local SAN-enabled self-signed certificate.
   - `Cronet` does not support `allowBadCertificates: true`; it verifies the same SAN-enabled local certificate via Android Network Security Config (`BENCHMARK_LOCAL_TLS=false` for `TurboFetch`, while `IO` / `dio_http2` keep `allowBadCertificates: true`).
5. **1-to-1 Server Audit**:
   - Every request's HTTP status (`200`), exact byte length, HTTP protocol version (`1.1` vs `2.0`), and ALPN (`h2`) are audited 1-to-1 against independent server logs (`server.csv`).

---

## 2. Engine Direct Comparison: `OkHttp3` vs. `Cronet`

Baseline drift between the two independent sessions remained minimal (`IO` medians within 0.8%–6.8% and `dio_http2` medians within 0.8%–3.9%), providing a fair apples-to-apples comparison on identical hardware and network conditions.

| Scenario | OkHttp3 Median (RPS) | Cronet Median (RPS) | Cronet vs. OkHttp3 | OkHttp3 Net P50 / P95 | Cronet Net P50 / P95 |
| :--- | :---: | :---: | :--- | :---: | :---: |
| **Scenario A**<br>*(5–45KiB, 3w)* | 494.07 ms (182.27) | **472.47 ms (190.50)** | **-4.4% wall / +4.5% RPS** | 13.67 / 19.76 ms | **13.05 / 17.79 ms** |
| **Scenario B**<br>*(10–100KiB, 5w)* | 359.68 ms (278.05) | **297.24 ms (336.49)** | **-17.4% wall / +21.0% RPS** | 13.72 / 22.17 ms | **10.61 / 16.70 ms** |
| **Scenario C**<br>*(20–200KiB, 10w)* | 393.95 ms (254.41) | **353.78 ms (282.67)** | **-10.2% wall / +11.1% RPS** | 32.89 / 55.46 ms | **28.51 / 42.45 ms** |

---

## 3. Benchmark Scenario Results: `OkHttp3` (Default Engine)

### 3.1 Total Wall-Clock Time and Throughput (10 Trials per Preset)

*All times in milliseconds. RPS is the median requests per second (successful requests / trial wall time).*

| Scenario | Adapter | Median Total | Mean Total | Sample SD | Min | Max | Median req/s |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Scenario A**<br>*(5–45KiB, 3 workers)* | (1) IO (HTTP/1.1) | 492.22 ms | 490.07 ms | 38.33 ms | 443.40 ms | 556.78 ms | 182.91 req/s |
| | (2) dio_http2 | 2,220.29 ms | 2,202.26 ms | 124.66 ms | 1,955.80 ms | 2,346.99 ms | 40.54 req/s |
| | **(3) TurboFetch (OkHttp3)** | **494.07 ms** | **503.47 ms** | **47.60 ms** | **440.68 ms** | **584.37 ms** | **182.27 req/s** |
| **Scenario B**<br>*(10–100KiB, 5 workers)* | (1) IO (HTTP/1.1) | 490.16 ms | 494.41 ms | 65.45 ms | 390.69 ms | 601.19 ms | 204.30 req/s |
| | (2) dio_http2 | 1,277.24 ms | 1,290.48 ms | 113.33 ms | 1,144.78 ms | 1,553.19 ms | 78.31 req/s |
| | **(3) TurboFetch (OkHttp3)** | **359.68 ms** | **367.28 ms** | **45.55 ms** | **323.06 ms** | **472.01 ms** | **278.05 req/s** |
| **Scenario C**<br>*(20–200KiB, 10 workers)* | (1) IO (HTTP/1.1) | 655.55 ms | 641.49 ms | 68.15 ms | 547.99 ms | 735.42 ms | 152.54 req/s |
| | (2) dio_http2 | 2,320.85 ms | 2,340.34 ms | 63.99 ms | 2,281.60 ms | 2,460.42 ms | 43.09 req/s |
| | **(3) TurboFetch (OkHttp3)** | **393.95 ms** | **430.25 ms** | **124.29 ms** | **325.19 ms** | **755.36 ms** | **254.41 req/s** |

> [!NOTE]
> In Scenario C (`OkHttp3`), 9 of 10 measured trials finished tightly between `325.19 ms` and `482.37 ms` (IQR `356.3–450.3 ms`); a single Wi-Fi jitter outlier in trial 4 (`755.36 ms`) inflated Sample SD without affecting the median (`393.95 ms`).

### 3.2 Phase Latency: Network Time vs. Parse Time (`OkHttp3`)

*Values in milliseconds; each cell shows the median across 10 trials of that trial's per-request **Mean / P50 / P95**.*

| Scenario | Adapter | Network Time (Mean / P50 / P95) | Parse Time (Mean / P50 / P95) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 15.739 ms / 15.572 ms / 23.192 ms | 0.358 ms / 0.307 ms / 0.716 ms |
| | (2) dio_http2 | 72.077 ms / 76.456 ms / 92.494 ms | 0.870 ms / 0.709 ms / 2.327 ms |
| | **(3) TurboFetch (OkHttp3)** | **14.103 ms / 13.670 ms / 19.756 ms** | **2.165 ms / 2.196 ms / 5.093 ms** |
| **Scenario B** | (1) IO (HTTP/1.1) | 21.256 ms / 20.575 ms / 33.667 ms | 3.012 ms / 2.139 ms / 7.360 ms |
| | (2) dio_http2 | 58.548 ms / 58.777 ms / 86.207 ms | 3.657 ms / 2.943 ms / 8.387 ms |
| | **(3) TurboFetch (OkHttp3)** | **14.544 ms / 13.720 ms / 22.167 ms** | **3.148 ms / 3.177 ms / 6.428 ms** |
| **Scenario C** | (1) IO (HTTP/1.1) | 58.857 ms / 50.746 ms / 121.128 ms | 4.724 ms / 3.425 ms / 11.399 ms |
| | (2) dio_http2 | 214.525 ms / 215.262 ms / 270.362 ms | 5.189 ms / 3.076 ms / 12.508 ms |
| | **(3) TurboFetch (OkHttp3)** | **32.967 ms / 32.886 ms / 55.456 ms** | **4.411 ms / 3.192 ms / 10.434 ms** |

### 3.3 Memory Footprint Observations (`OkHttp3`)

*RSS values are medians of ten per-trial observations in MiB (`ProcessInfo.currentRss`, 25 ms sampler). Jank Frames and Max Frame Duration are N/A in this Release protocol.*

| Scenario | Adapter | Sampled Peak RSS | RSS Delta (Net Allocation) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 215.26 MiB | +0.23 MiB |
| | (2) dio_http2 | 217.38 MiB | +0.00 MiB |
| | **(3) TurboFetch (OkHttp3)** | **229.66 MiB** | **+15.08 MiB** |
| **Scenario B** | (1) IO (HTTP/1.1) | 248.75 MiB | +0.10 MiB |
| | (2) dio_http2 | 240.99 MiB | +0.27 MiB |
| | **(3) TurboFetch (OkHttp3)** | **249.28 MiB** | **+32.50 MiB** |
| **Scenario C** | (1) IO (HTTP/1.1) | 281.94 MiB | +0.93 MiB |
| | (2) dio_http2 | 227.08 MiB | +0.00 MiB |
| | **(3) TurboFetch (OkHttp3)** | **281.12 MiB** | **+60.39 MiB** |

---

## 4. Benchmark Scenario Results: `Cronet` (Optional Engine)

### 4.1 Total Wall-Clock Time and Throughput (10 Trials per Preset)

| Scenario | Adapter | Median Total | Mean Total | Sample SD | Min | Max | Median req/s |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Scenario A**<br>*(5–45KiB, 3 workers)* | (1) IO (HTTP/1.1) | 488.31 ms | 497.03 ms | 27.38 ms | 462.32 ms | 546.23 ms | 184.33 req/s |
| | (2) dio_http2 | 2,256.83 ms | 2,235.68 ms | 79.63 ms | 2,031.14 ms | 2,306.84 ms | 39.88 req/s |
| | **(3) TurboFetch (Cronet)** | **472.47 ms** | **477.74 ms** | **41.18 ms** | **420.42 ms** | **561.15 ms** | **190.50 req/s** |
| **Scenario B**<br>*(10–100KiB, 5 workers)* | (1) IO (HTTP/1.1) | 523.38 ms | 506.24 ms | 56.37 ms | 427.75 ms | 585.22 ms | 191.07 req/s |
| | (2) dio_http2 | 1,287.62 ms | 1,268.42 ms | 73.48 ms | 1,160.99 ms | 1,359.59 ms | 77.67 req/s |
| | **(3) TurboFetch (Cronet)** | **297.24 ms** | **301.48 ms** | **26.97 ms** | **278.80 ms** | **366.76 ms** | **336.49 req/s** |
| **Scenario C**<br>*(20–200KiB, 10 workers)* | (1) IO (HTTP/1.1) | 633.83 ms | 626.61 ms | 132.29 ms | 428.93 ms | 814.08 ms | 157.77 req/s |
| | (2) dio_http2 | 2,411.11 ms | 2,377.36 ms | 80.20 ms | 2,257.12 ms | 2,471.66 ms | 41.47 req/s |
| | **(3) TurboFetch (Cronet)** | **353.78 ms** | **352.04 ms** | **22.87 ms** | **319.73 ms** | **395.18 ms** | **282.67 req/s** |

### 4.2 Phase Latency: Network Time vs. Parse Time (`Cronet`)

| Scenario | Adapter | Network Time (Mean / P50 / P95) | Parse Time (Mean / P50 / P95) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 15.708 ms / 15.968 ms / 22.840 ms | 0.342 ms / 0.317 ms / 0.677 ms |
| | (2) dio_http2 | 73.537 ms / 74.832 ms / 92.039 ms | 0.837 ms / 0.630 ms / 2.213 ms |
| | **(3) TurboFetch (Cronet)** | **13.044 ms / 13.047 ms / 17.788 ms** | **2.227 ms / 2.091 ms / 5.312 ms** |
| **Scenario B** | (1) IO (HTTP/1.1) | 22.197 ms / 22.129 ms / 36.409 ms | 3.091 ms / 2.562 ms / 7.793 ms |
| | (2) dio_http2 | 59.098 ms / 57.924 ms / 90.898 ms | 3.615 ms / 4.117 ms / 8.474 ms |
| | **(3) TurboFetch (Cronet)** | **11.223 ms / 10.607 ms / 16.701 ms** | **2.985 ms / 3.169 ms / 6.479 ms** |
| **Scenario C** | (1) IO (HTTP/1.1) | 56.475 ms / 49.919 ms / 111.940 ms | 4.447 ms / 3.200 ms / 11.502 ms |
| | (2) dio_http2 | 222.649 ms / 221.094 ms / 273.650 ms | 5.411 ms / 4.590 ms / 12.488 ms |
| | **(3) TurboFetch (Cronet)** | **29.253 ms / 28.510 ms / 42.445 ms** | **4.322 ms / 3.233 ms / 9.660 ms** |

### 4.3 Memory Footprint Observations (`Cronet`)

| Scenario | Adapter | Sampled Peak RSS | RSS Delta (Net Allocation) |
| :--- | :--- | ---: | ---: |
| **Scenario A** | (1) IO (HTTP/1.1) | 217.00 MiB | +0.04 MiB |
| | (2) dio_http2 | 215.28 MiB | +0.04 MiB |
| | **(3) TurboFetch (Cronet)** | **224.63 MiB** | **+19.30 MiB** |
| **Scenario B** | (1) IO (HTTP/1.1) | 254.58 MiB | +0.00 MiB |
| | (2) dio_http2 | 228.92 MiB | +0.27 MiB |
| | **(3) TurboFetch (Cronet)** | **255.66 MiB** | **+41.88 MiB** |
| **Scenario C** | (1) IO (HTTP/1.1) | 294.44 MiB | +1.56 MiB |
| | (2) dio_http2 | 247.72 MiB | +0.00 MiB |
| | **(3) TurboFetch (Cronet)** | **294.24 MiB** | **+78.82 MiB** |

---

## 5. Technical Analysis (Android / `OkHttp3` & `Cronet`)

### 5.1 Concurrency Scaling Over Real Wi-Fi LAN

Both Android native engines demonstrate clear HTTP/2 multiplexing advantages as worker concurrency and payload size scale up:

- **`OkHttp3`**:
  - **Scenario A (3w, 5–45KiB)**: **494.07 ms** — **4.49x faster** than `dio_http2`, tied with `IO` (`492.22 ms`).
  - **Scenario B (5w, 10–100KiB)**: **359.68 ms** (**278.05 req/s**) — **1.36x faster** than `IO` and **3.55x faster** than `dio_http2`.
  - **Scenario C (10w, 20–200KiB)**: **393.95 ms** (**254.41 req/s**) — **1.66x faster** than `IO` and **5.89x faster** than `dio_http2`.
- **`Cronet`**:
  - **Scenario A (3w, 5–45KiB)**: **472.47 ms** — **1.03x faster** than `IO` and **4.78x faster** than `dio_http2`.
  - **Scenario B (5w, 10–100KiB)**: **297.24 ms** (**336.49 req/s**) — **1.76x faster** than `IO` and **4.33x faster** than `dio_http2`.
  - **Scenario C (10w, 20–200KiB)**: **353.78 ms** (**282.67 req/s**) — **1.79x faster** than `IO` and **6.82x faster** than `dio_http2`, with remarkably tight variance (CV **6.5%**, `319.7–395.2 ms`).

### 5.2 Why `Cronet` Outperforms `OkHttp3` — and the Practical Trade-offs

Comparing the two engines on the exact same Pixel 10a hardware highlights a clear performance-versus-footprint trade-off:

1. **Network Layer Throughput & Tail Latency**:
   - `Cronet` runs Chromium's native C++ HTTP/2 stack and BoringSSL directly beneath JNI, avoiding JVM/Okio framing overhead. This cuts per-request median `Network Time` by **22.7%** in Scenario B (`13.72 ms -> 10.61 ms`) and **13.3%** in Scenario C (`32.89 ms -> 28.51 ms`), while tightening Scenario C `Network P95` by **23.5%** (`55.46 ms -> 42.45 ms`).
2. **APK Footprint & Runtime RSS**:
   - Bundling `cronet-embedded` increases the Release APK by **+17.1 MB** (`46.7 MB -> 63.8 MB`), and Scenario C `Sampled Peak RSS` rises by **+13.1 MiB** (`281.12 MiB -> 294.24 MiB`) due to Chromium's native library and direct `ByteBuffer` pool. *(Apps targeting Google Play devices can eliminate the APK increase by using `play-services-cronet` instead of `cronet-embedded`.)*
3. **Developer Ergonomics**:
   - `OkHttp3` remains the default engine because it requires zero extra dependencies, keeps APK size minimal, and supports `allowBadCertificates: true` for local development without Android Network Security Config XML.

### 5.3 `TurboTransformer` 24KiB Threshold Behavior on Tensor G4

Inspecting Scenario A per-payload `Parse Time` medians (identical across both engine runs) shows the exact effect of the 24KiB hybrid threshold:

- **5–20KiB (Main Isolate synchronous)**: `0.141 ms` (5KiB) to `0.372 ms` (20KiB) — matching `IO` (`0.111–0.295 ms`).
- **25–45KiB (Worker Isolate offload)**: `3.141 ms` (25KiB) to `4.149 ms` (45KiB) — adding ~2.8–3.5 ms of Worker Isolate dispatch/semaphore overhead vs. `0.308–0.513 ms` Main Isolate decoding in `BackgroundTransformer`.
- Consequently, in Scenario A, `OkHttp3`'s **12% faster Network Time** (`13.67 ms` vs `15.57 ms`) is offset by 25–45KiB Isolate dispatch, while `Cronet`'s **18% faster Network Time** (`13.05 ms` vs `15.97 ms`) still pulls ahead of `IO`.
- Above 50KiB (Scenarios B & C), `BackgroundTransformer` crosses its 50KiB `compute` threshold and `TurboTransformer` matches or beats both baselines (`Parse P95`: **9.66–10.43 ms** vs. **11.40–11.50 ms** for `IO` in Scenario C).

### 5.4 Memory Stability & GC Recovery

Although `TurboFetch` reports higher per-trial `RSS Delta` (`+60.4 MiB` on `OkHttp3` / `+78.8 MiB` on `Cronet` in Scenario C), trial-by-trial inspection confirms **zero memory leaks**:

- In `OkHttp3` Scenario C, RSS rises from `~220 MiB` to `~281 MiB` during the 10-worker 200KiB burst and returns cleanly to `~220 MiB` before subsequent trials after ART GC reclamation.
- **Absolute `Sampled Peak RSS` matches `IO` closely** (`OkHttp3`: `281.12 MiB` vs. `IO` `281.94 MiB`; `Cronet`: `294.24 MiB` vs. `IO` `294.44 MiB`).

---

## 6. Reproducing the Benchmark

### 6.1 Prerequisites
1. Physical Android device (connected via USB for `adb` + same Wi-Fi LAN as the host PC, screen timeout extended) or Android Emulator.
2. FVM Flutter `3.47.8`, Node.js, Python 3, OpenSSL, and `adb`.

### 6.2 Common Steps (`OkHttp3` Default)

1. **Generate Temporary SAN-Enabled Local TLS Certificate** (outside the repository, e.g., `%TEMP%` or `/tmp`):
   ```bash
   openssl req -x509 -newkey rsa:2048 -nodes -days 7 \
     -keyout /tmp/bench-key.pem -out /tmp/bench-cert.pem \
     -subj "/CN=<LAN_IP>" \
     -addext "basicConstraints=critical,CA:TRUE" \
     -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:<LAN_IP>"
   ```

2. **Start Mock Server (LAN mode with line-flushed logging)**:
   ```bash
   HOST=<LAN_IP> PORT=8444 REQUIRE_TLS=1 BENCHMARK_LOG=1 \
     CERT_PATH=/tmp/bench-cert.pem KEY_PATH=/tmp/bench-key.pem \
     node example/server/http2_mock_server.mjs > /tmp/okhttp-server.jsonl
   ```

3. **Build Release APK & Run on Device**:
   ```bash
   SHA=$(git rev-parse HEAD)
   cd example
   fvm flutter build apk --release -t lib/main.dart \
     --dart-define=BENCHMARK_BASE_URL=https://<LAN_IP>:8444 \
     --dart-define=BENCHMARK_LOCAL_TLS=true \
     --dart-define=BENCHMARK_ENGINE=okhttp \
     --dart-define=BENCHMARK_SOURCE_SHA="$SHA"
   adb logcat -G 16M && adb logcat -c
   adb install -r build/app/outputs/flutter-apk/app-release.apk
   adb shell am force-stop io.github.khirohis.flutter_turbo_fetch_example
   # Capture `adb logcat -v raw -s flutter:I` until `"type":"session_complete"`
   adb shell am start -n io.github.khirohis.flutter_turbo_fetch_example/.MainActivity
   ```

4. **Audit, Export, and Summarize**:
   ```bash
   python tool/benchmark/export_benchmark.py /tmp/okhttp-device.log /tmp/okhttp-server.jsonl tool/benchmark/data/android/<YYYY-MM-DD>-okhttp
   python tool/benchmark/summarize_benchmark.py tool/benchmark/data/android/<YYYY-MM-DD>-okhttp-raw.csv
   ```

### 6.3 Reproducing `Cronet`
Temporarily configure `flutter_turbo_fetch.androidEngine=cronet` in `example/android/gradle.properties`, add `org.chromium.net:cronet-embedded:143.7445.0` to `example/android/app/build.gradle.kts`, register the temporary SAN certificate via Android Network Security Config (`network_security_config.xml`), pass `allowLocalCertificate = false` for the `TurboFetch` preset only, build with `--dart-define=BENCHMARK_ENGINE=cronet`, and revert all temporary changes after exporting `tool/benchmark/data/android/<YYYY-MM-DD>-cronet-*.csv`.

---

## 7. Raw Evidence & Artifacts

Raw benchmark measurements and server event verification logs are tracked under [`tool/benchmark/data/android/`](../../tool/benchmark/data/android/):

- **`OkHttp3`**:
  - **[Raw Trials CSV](../../tool/benchmark/data/android/2026-11-23-okhttp-raw.csv)** / **[Per-Request CSV](../../tool/benchmark/data/android/2026-11-23-okhttp-requests.csv)** / **[Server Log Evidence](../../tool/benchmark/data/android/2026-11-23-okhttp-server.csv)** / **[Summary Metrics CSV](../../tool/benchmark/data/android/2026-11-23-okhttp-summary.csv)**
- **`Cronet`**:
  - **[Raw Trials CSV](../../tool/benchmark/data/android/2026-11-23-cronet-raw.csv)** / **[Per-Request CSV](../../tool/benchmark/data/android/2026-11-23-cronet-requests.csv)** / **[Server Log Evidence](../../tool/benchmark/data/android/2026-11-23-cronet-server.csv)** / **[Summary Metrics CSV](../../tool/benchmark/data/android/2026-11-23-cronet-summary.csv)**
