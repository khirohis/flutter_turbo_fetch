## 0.1.2

* **Dart Layer Hardening & Refactoring**: Fixed unhandled asynchronous exceptions during request cancellation, prevented dispatching requests cancelled while reading `requestStream`, added validation requiring `maxConcurrentIsolates >= 1` in `TurboTransformer`, and simplified internal adapter/transformer/options pipelines.
* **Dart Hot-Path Optimizations**: Optimized `TurboHttpAdapter` hot paths, semaphore queueing, and immediate main-isolate handling for empty response payloads.
* **iOS Legacy Device Benchmarks**: Added physical iPhone 6s Release benchmark results under the split-pipeline protocol (`N=10` trials with 1:1 server audit) to `README.md` and the benchmark documentation.

## 0.1.1

* **Android Cronet Engine Support (Opt-in)**: Added build-time Cronet engine selection (`flutter_turbo_fetch.androidEngine=cronet` in `android/gradle.properties`) alongside the default `OkHttp3` engine, excluding `OkHttp3` when Cronet is selected and providing full unit test coverage.
* **Linux HTTP/2 Multiplexing Fix**: Replaced the `CURLSH` + thread-pool design in the Linux native layer with a single-threaded `curl_multi` event loop for reliable HTTP/2 multiplexing and concurrency.
* **Rigorous Multi-Platform Benchmarks**: Refreshed benchmark reports under the split-pipeline protocol (`N=10` trials with 1:1 server audit) across Android (Pixel 10a — `OkHttp3` & `Cronet`), Windows, and Linux.

## 0.1.0

* **Breaking Change — Simplified Public API**: Removed `maxRequestsPerHost` from `TurboHttpAdapter` and all native session managers (`Android`, `iOS`/`macOS`, `Windows`, `Linux`), relying on each platform's native HTTP/2 connection and multiplexing defaults. When upgrading from `0.0.x`, simply remove the `maxRequestsPerHost` argument from `TurboHttpAdapter(...)`.
* **Unified Darwin Tests & CI**: Consolidated iOS and macOS `XCTest` suites under shared `example/darwin/RunnerTests/`, added deterministic cancellation, delegate, and detach coverage, and unified Darwin CI verification across iOS and macOS.
* **Documentation & Guide Refresh**: Restructured `docs/turbo-transformer.md` into JSON usage examples and conceptual sketches, aligned all platform design/benchmark docs with the current implementation, and updated `README.md` links for `pub.dev`.

## 0.0.5

* **Documentation & Tuning Guide**: Added performance tuning guidelines for high-end Apple Silicon devices (tested on iPhone, noting that macOS may also benefit), detailing the sweet spot for `isolateThresholdBytes` (128KiB–256KiB) and concurrency settings to safeguard 120 FPS UI frame budgets.
* **Verified iOS Benchmark Results**: Published physical iPhone release benchmark reports and isolated comparative data against Dio baseline and `dio_http2`.
* **Package Optimization**: Refined `.pubignore` to exclude internal developer tools, raw benchmark datasets, and platform build artifacts, minimizing the published package archive size.

## 0.0.4

* **Linux Platform Support**: Added native Linux desktop support (`Ubuntu 20.04+` / `Debian 11+`, 64-bit x64 / ARM64) powered by C++17 `libcurl` (`libcurl4-openssl-dev`) with HTTP/2 connection sharing (`CURLSH*`) and worker thread pool execution.
* **Pigeon Desktop Code Generation**: Differentiated Pigeon code generation for desktop platforms, adopting standard GObject C API (`--gobject_out`) for Linux adhering to the Flutter GTK architecture and C++ (`--cpp_out`) for Windows.
* **Linux Native 3-Layer Architecture**: Implemented 3-layer separation of concerns (`FlutterTurboFetchPlugin`, `TurboHttpExecutor`, and `TurboSessionManager`) with dedicated C++ unit test suite runnable on WSL2 and Linux environments.

## 0.0.3

* **macOS Platform Support**: Added native macOS desktop support (`macOS 10.15+`) via a unified Darwin (`sharedDarwinSource: true`) implementation powered by `URLSession`, supporting both Swift Package Manager (SPM) and CocoaPods.
* **Darwin 3-Layer Native Architecture**: Refactored the iOS/macOS native layer into a clean 3-layer separation of concerns (`FlutterTurboFetchPlugin`, `TurboHttpExecutor`, and `TurboSessionManager`) with dedicated `XCTest` unit test suites.

## 0.0.2

* **Windows Platform Support**: Added native Windows desktop support (x64 / ARM64) powered by WinHTTP with HTTP/2 multiplexing and OS thread pool execution.
* **Typed `TurboOptions` Factories**: Added `TurboOptions.json`, `TurboOptions.bytes`, and `TurboOptions.plain` factory constructors for compile-time type-safe `mapper` inputs without manual casts.
* **Documentation & Benchmarks**: Added multi-platform performance benchmark reports (Android & Windows) and refined API documentation.

## 0.0.1

* Initial release of `flutter_turbo_fetch`.
* **Platform-Native Batch Reception**: High-performance HTTP/2 networking adapter for `package:dio` powered by OkHttp 3 (Android) and URLSession (iOS), eliminating per-chunk cross-thread jitter.
* **Zero-Copy Worker Isolate Offloading**: `TurboTransformer` with `TransferableTypedData` and fused UTF-8 / JSON decoding for non-blocking UI thread operation.
* **Full Dio Compatibility**: Drop-in support for all standard `ResponseType`s (`json`, `plain`, `bytes`, `stream`).
* **Typed Model Mapping & Safety**: In-isolate domain model mapping via `TurboOptions(mapper: ...)` and payload size bounding via `TurboOptions(maxResponseBytes: ...)`.
