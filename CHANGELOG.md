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
