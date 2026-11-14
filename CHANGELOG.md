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
