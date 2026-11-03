## 0.0.1

* Initial release of `flutter_turbo_fetch`.
* **Platform-Native Batch Reception**: High-performance HTTP/2 networking adapter for `package:dio` powered by OkHttp 3 (Android) and URLSession (iOS), eliminating per-chunk cross-thread jitter.
* **Zero-Copy Worker Isolate Offloading**: `TurboTransformer` with `TransferableTypedData` and fused UTF-8 / JSON decoding for non-blocking UI thread operation.
* **Full Dio Compatibility**: Drop-in support for all standard `ResponseType`s (`json`, `plain`, `bytes`, `stream`).
* **Typed Model Mapping & Safety**: In-isolate domain model mapping via `TurboOptions(mapper: ...)` and payload size bounding via `TurboOptions(maxResponseBytes: ...)`.
