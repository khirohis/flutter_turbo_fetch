# flutter_turbo_fetch

A high-performance, non-blocking networking plugin for Flutter (`package:dio`).

`flutter_turbo_fetch` replaces traditional Dart-based HTTP streaming bottlenecks with **platform-native batch reception** (Android `OkHttp3` / iOS `URLSession`) and **zero-copy Isolate offloading** (`TransferableTypedData` + semaphore-governed `Isolate.run`). It guarantees a silky-smooth 60/120 FPS UI thread even under heavy parallel HTTP/2 JSON, text, or binary workloads.

---

## 🚀 Key Architectural Advantages

1. **Native HTTP/2 Batch Reception (No Per-Chunk FFI Jitter)**
   - **Android (`OkHttp3`)**: Receives all HTTP/2 data frames and concatenates them in native background memory using Okio buffer pools, returning a single completed `Uint8List` directly to Dart.
   - **iOS (`URLSession`)**: Uses the C/C++ CFNetwork completion-handler pipeline (`dataTask(with:completionHandler:)`) without delegate ping-pong overhead.
2. **True O(1) Zero-Copy Worker Isolate Offloading**
   - **Inbound**: Transfers large byte buffers (>= 24KB) to worker Isolates using `TransferableTypedData.fromList()` in $O(1)$ without deep-copying.
   - **Outbound**: Uses `Isolate.run` (`Isolate.exit`) to transfer decoded Dart object trees back to the Main Isolate with zero transitive copy overhead.
   - **Direct JSON Decoding**: Fuses UTF-8 and JSON decoding (`utf8.decoder.fuse(json.decoder)`) to eliminate multi-megabyte intermediate `String` allocations.
3. **Full `Dio` Compatibility + Typed Model Mapping**
   - Drop-in replacement with `TurboHttpAdapter` and `TurboTransformer`.
   - Supports all 4 standard `ResponseType`s (`json`, `plain`, `bytes`, `stream`).
   - `TurboOptions(mapper: ...)` runs user-defined domain model mapping (e.g. `User.fromJson`) directly inside the Worker Isolate, returning fully instantiated models to Main Isolate in $O(1)$.
   - Per-request payload safety limit (`TurboOptions(maxResponseBytes: ...)`).

---

## 📋 Requirements & Compatibility

| Platform / Dependency | Supported Version |
| :--- | :--- |
| **Flutter** | `>= 3.24.0` |
| **Dart** | `>= 3.5.0` |
| **Dio** | `^5.4.0` |
| **Android** | `minSdk = 24` (Android 7.0+) • `okhttp:4.12.0` |
| **iOS** | `iOS 13.0+` • Swift Package Manager & CocoaPods |

---

## 📦 Installation

Add `flutter_turbo_fetch` to your `pubspec.yaml` via `git` or local `path`:

```yaml
dependencies:
  dio: ^5.4.0
  flutter_turbo_fetch:
    git:
      url: https://github.com/khirohis/flutter_turbo_fetch.git
      ref: main
    # or local development:
    # path: ../flutter_turbo_fetch
```

Then fetch packages:

```bash
fvm flutter pub get
```

---

## ⚙️ Platform Setup

### Android

Ensure internet permission is declared in your app's `android/app/src/main/AndroidManifest.xml`:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <uses-permission android:name="android.permission.INTERNET" />
    ...
</manifest>
```

#### R8 / ProGuard
`OkHttp3` rules are automatically handled by the Android Gradle Plugin. If you use strict R8 full mode, ensure standard OkHttp rules are retained:

```proguard
-dontwarn okhttp3.**
-dontwarn okio.**
-keepattributes Signature
-keepattributes *Annotation*
```

### iOS

No special permissions are needed for HTTPS endpoints. If testing locally with cleartext HTTP or self-signed certificates, configure `NSAppTransportSecurity` in `ios/Runner/Info.plist`:

```xml
<key>NSAppTransportSecurity</key>
<dict>
    <key>NSAllowsLocalNetworking</key>
    <true/>
</dict>
```

---

## 💡 Quick Start

Configure a single `Dio` instance once at application startup. All downstream requests benefit from native HTTP/2 batching and zero-copy transformations:

```dart
import 'package:dio/dio.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';

final dio = Dio()
  ..httpClientAdapter = TurboHttpAdapter()
  ..transformer = TurboTransformer();
```

### 1. Zero-Copy Typed Domain Model Mapping (`TurboOptions`)

By passing `mapper` to `TurboOptions<T>`, JSON decoding and `.fromJson` model instantiation execute **entirely inside a Worker Isolate** for payloads $\ge$ 24KB. The instantiated object tree is returned to the UI thread in $O(1)$:

```dart
class UserModel {
  const UserModel({required this.id, required this.name});
  final int id;
  final String name;

  factory UserModel.fromJson(Map<String, dynamic> json) => UserModel(
        id: json['id'] as int,
        name: json['name'] as String,
      );
}

// Request typed model directly
final response = await dio.get<UserModel>(
  '/api/users/1',
  options: TurboOptions<UserModel>(
    responseType: ResponseType.json,
    mapper: (data) => UserModel.fromJson(data as Map<String, dynamic>),
  ),
);

final UserModel user = response.data!;
print('User: ${user.name}');
```

> **Note on `mapper` closures**: When offloaded to a worker Isolate, `mapper` must be a top-level function, `static` method, or closure that does not capture mutable state outside the Isolate boundary.

### 2. Supported Response Types

`TurboHttpAdapter` and `TurboTransformer` support all 4 standard Dio response types simultaneously on a single `Dio` instance:

```dart
// 1. JSON (dynamic Map/List or custom mapper)
final jsonRes = await dio.get<dynamic>('/api/data');

// 2. Plain Text (String)
final textRes = await dio.get<String>(
  '/static/terms.txt',
  options: TurboOptions<String>(responseType: ResponseType.plain),
);

// 3. Raw Bytes (Uint8List)
final bytesRes = await dio.get<Uint8List>(
  '/assets/banner.png',
  options: TurboOptions<Uint8List>(responseType: ResponseType.bytes),
);

// 4. Stream (Single-chunk ResponseBody without fallback)
final streamRes = await dio.get<ResponseBody>(
  '/stream/data',
  options: TurboOptions<ResponseBody>(responseType: ResponseType.stream),
);
```

### 3. Response Size Guard (`maxResponseBytes`)

Protect device memory from unexpectedly large payloads on a per-request basis. If a payload exceeds `maxResponseBytes`, the native layer immediately terminates the connection without consuming further network bandwidth:

```dart
try {
  await dio.get<dynamic>(
    '/api/download',
    options: TurboOptions(
      maxResponseBytes: 10 * 1024 * 1024, // 10 MB limit
    ),
  );
} on DioException catch (e) {
  // Throws DioExceptionType.unknown with exceeded limit in error message
  print('Exceeded limit: ${e.message}');
}
```

### 4. Cancellation & Timeouts

Seamlessly connects with Dio's `CancelToken` and timeout configurations:

```dart
final cancelToken = CancelToken();

// Cancel request mid-flight
Future.delayed(const Duration(milliseconds: 50), () {
  cancelToken.cancel('User navigated away');
});

try {
  await dio.get<dynamic>(
    '/api/heavy-search',
    cancelToken: cancelToken,
    options: TurboOptions(
      sendTimeout: const Duration(seconds: 5),
      receiveTimeout: const Duration(seconds: 10),
    ),
  );
} on DioException catch (e) {
  if (e.type == DioExceptionType.cancel) {
    print('Request cancelled cleanly');
  }
}
```

---

## 🔧 Configuration & Tuning Parameters

### `TurboHttpAdapter`

Configured once on the `Dio` instance to manage connection pools and transport security:

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `maxRequestsPerHost` | `int` | `32` | Maximum concurrent HTTP/2 requests per host. On Android, OkHttp's dispatcher maxRequests is automatically scaled if greater than 64. |
| `allowBadCertificates` | `bool` | `false` | Allows self-signed certificates for local development and mock servers. |

### `TurboTransformer`

Controls Isolate dispatching and thread concurrency:

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `isolateThresholdBytes` | `int` | `24 * 1024` (24KB) | Payloads below this threshold are decoded synchronously on the Main Isolate (< 0.5ms). Payloads at or above this threshold are dispatched to Worker Isolates via zero-copy `TransferableTypedData`. |
| `maxConcurrentIsolates` | `int?` | `max(4, cores - 2)` | Maximum concurrent worker Isolates permitted by the internal semaphore, preventing CPU thread starvation during 10-20x burst requests. |

---

## 📊 Benchmarking & Performance Verification

`flutter_turbo_fetch` includes comprehensive benchmarking suites at both the Dart unit level and the native device level.

### 1. Dart-Layer Microbenchmark (No Emulator Needed)

Measure JSON decoding and model instantiation speed, event-loop lag, and memory delta against Dio's standard `BackgroundTransformer`:

```bash
fvm flutter test test/turbo_benchmark_test.dart
```

**Representative Output:**
```text
=== Dart-Layer Transform & Mapper Performance Benchmark ===
| Workload | Strategy | Total (ms) | Avg / Max Req (ms) | Max Main-Isolate Lag (ms) | RSS Peak +MB |
| :--- | :--- | ---: | ---: | ---: | ---: |
| 10KB x 20 (Small / <24KB) | 1. Dio BackgroundTransformer + Main Mapper | 20 | 0.1 / 1 | 0.0 | +0.6 |
| 10KB x 20 (Small / <24KB) | 2. TurboTransformer (Zero-Copy JSON) | 9 | 0.0 / 0 | 0.0 | +2.8 |
| 10KB x 20 (Small / <24KB) | 3. TurboTransformer + TurboOptions(mapper) | 6 | 0.0 / 0 | 0.0 | +1.3 |
| 100KB x 15 (Medium / >=24KB) | 1. Dio BackgroundTransformer + Main Mapper | 11 | 6.3 / 9 | 7.9 | +6.0 |
| 100KB x 15 (Medium / >=24KB) | 2. TurboTransformer (Zero-Copy JSON) | 33 | 18.8 / 31 | 29.8 | +9.8 |
| 100KB x 15 (Medium / >=24KB) | 3. TurboTransformer + TurboOptions(mapper) | 5 | 2.8 / 4 | 3.2 | +5.6 |
| 500KB x 10 (Large / >=24KB) | 1. Dio BackgroundTransformer + Main Mapper | 19 | 12.6 / 14 | 11.6 | +19.5 |
| 500KB x 10 (Large / >=24KB) | 2. TurboTransformer (Zero-Copy JSON) | 19 | 13.2 / 15 | 15.5 | +23.3 |
| 500KB x 10 (Large / >=24KB) | 3. TurboTransformer + TurboOptions(mapper) | 10 | 5.2 / 8 | 6.8 | +17.7 |
```

### 2. Device / Emulator End-to-End Benchmark

Compare 4 network client presets (`IOHttpClientAdapter`, `dio_http2_adapter`, `native_dio_adapter`, and `TurboHttpAdapter + TurboTransformer`) on Android or iOS:

1. **Start the local HTTP/2 mock server**:
   ```bash
   node example/server/http2_mock_server.mjs
   ```
2. **Run integration tests**:
   ```bash
   cd example
   fvm flutter test integration_test/plugin_integration_test.dart
   ```
3. Or launch the example app on an Android Emulator or iOS Simulator:
   ```bash
   cd example
   fvm flutter run
   ```

---

## 📚 Technical Documentation

For detailed design rationale and architectural deep dives, see the [`docs/`](docs/) directory:

- [Architecture Overview](docs/architectures/architecture.md): End-to-end pipeline and layer boundaries.
- [Native Layer Specifications](docs/architectures/native-layer.md): Android `OkHttp3` & iOS `URLSession` completion handler design.
- [Dart Layer Specifications](docs/architectures/dart-layer.md): `TurboHttpAdapter`, `TurboTransformer`, and `TurboOptions` contracts.
- [Design Rationale & Bottleneck Analysis](docs/references/design-rationale.md): Technical justifications behind avoiding stream FFI, native JSON parsing, and persistent worker pools.
- [Development Roadmap](docs/roadmap.md): Milestone progress and completion checklists.

---

## 📄 License

This project is licensed under the MIT License.
