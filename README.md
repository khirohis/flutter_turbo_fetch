# flutter_turbo_fetch

[![pub package](https://img.shields.io/pub/v/flutter_turbo_fetch.svg)](https://pub.dev/packages/flutter_turbo_fetch)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)

A high-performance, non-blocking networking plugin for Flutter (`package:dio`).

`flutter_turbo_fetch` replaces traditional Dart-based HTTP streaming bottlenecks with **platform-native batch reception** (Android `OkHttp3` / iOS `URLSession` / Windows `WinHTTP`) and **zero-copy Isolate offloading** (`TransferableTypedData` + semaphore-governed `Isolate.run`). It guarantees a silky-smooth 60/120 FPS UI thread even under heavy parallel HTTP/2 JSON, text, or binary workloads.

---

## ✨ Features

- **Native HTTP/2 Batch Reception**: Zero per-chunk FFI overhead via OS-level buffering (OkHttp3, URLSession, & WinHTTP).
- **Non-Blocking UI Thread**: Eliminates jank on large payloads (>= 24KB) through zero-copy `TransferableTypedData` offloading.
- **In-Worker Model Mapping**: Directly parses and instantiates domain models inside worker Isolates.
- **Full Dio Compatibility**: Drop-in replacement supporting all 4 standard `ResponseType`s.

---

## 📋 Requirements & Compatibility

| Platform / Dependency | Supported Version | Status |
| :--- | :--- | :--- |
| **Flutter** | `>= 3.24.0` | Supported |
| **Dart** | `>= 3.5.0` | Supported |
| **Dio** | `^5.4.0` | Supported |
| **Android** | `minSdk = 24` (Android 7.0+) • `okhttp:4.12.0` | Tested & Benchmarked |
| **iOS** | `iOS 13.0+` • SPM & CocoaPods | Implemented (Benchmarking Pending) |
| **Windows** | `Windows 10 (1809+)` (64-bit) • `WinHTTP` (`winhttp.dll`) | Tested & Benchmarked |
| **Web** | Not Supported | Out of Scope |
| **macOS / Linux** | Not Supported | Future Consideration |

> [!NOTE]
>
> - **iOS Status**: Native `URLSession` integration is fully implemented, but formal end-to-end performance benchmarking and stress testing have not yet been conducted.
> - **Web & Other Desktop Platforms**: Web is not supported due to native OS transport requirements. Support for macOS and Linux is under consideration for future phases.

---

## 📦 Installation

Add `flutter_turbo_fetch` and `dio` to your Flutter project:

```bash
flutter pub add flutter_turbo_fetch dio
```

Or add them directly to your `pubspec.yaml`:

```yaml
dependencies:
  dio: ^5.4.0
  flutter_turbo_fetch: ^0.0.1
```

---

## ⚙️ Platform Setup

### Android

Ensure internet permission is declared in your app's `android/app/src/main/AndroidManifest.xml`:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <uses-permission android:name="android.permission.INTERNET" />
</manifest>
```

> **R8 / ProGuard**: Rules for `OkHttp3` are automatically bundled. No manual configuration is required unless using strict R8 full mode.

### iOS

No special permissions are needed for HTTPS endpoints. If testing locally with cleartext HTTP or self-signed certificates, configure `NSAppTransportSecurity` in `ios/Runner/Info.plist`:

```xml
<key>NSAppTransportSecurity</key>
<dict>
    <key>NSAllowsLocalNetworking</key>
    <true/>
</dict>
```

### Windows

No additional setup or external libraries are required. The plugin links directly against the Windows OS system component `winhttp.dll`.

---

## 💡 Quick Start

Configure a single `Dio` instance once at application startup.

`TurboOptions` inherits Dio's standard `responseType = ResponseType.json` default. By passing `mapper`, JSON decoding and domain model instantiation (`.fromJson`) execute **entirely inside a Worker Isolate** for payloads >= 24KB, returning typed objects to the UI thread in $O(1)$:

```dart
import 'package:dio/dio.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';

final dio = Dio()
  ..httpClientAdapter = TurboHttpAdapter()
  ..transformer = TurboTransformer();

// 1. Define your domain model
class UserModel {
  UserModel.fromJson(Map<String, dynamic> json)
      : id = json['id'] as int,
        name = json['name'] as String;

  final int id;
  final String name;
}

// 2. Fetch directly as a typed model (parsed in a background Isolate)
final response = await dio.get<UserModel>(
  'https://api.example.com/users/1',
  options: TurboOptions.json<UserModel>(
    mapper: (json) => UserModel.fromJson(json as Map<String, dynamic>),
  ),
);

final UserModel user = response.data!;
print('User: ${user.name}');
```

> [!TIP]
>
> - **In-Isolate Mapper**: To run inside worker Isolates, `mapper` should be a top-level function, `static` method, or pure closure without capturing external mutable state.
> - **Typed Factories**: Use `TurboOptions.json`, `TurboOptions.bytes`, or `TurboOptions.plain` to automatically bind `responseType` and type `mapper` parameters (e.g. `Uint8List` for Protobuf/FlatBuffers).
> - **Defensive Payload Guard**: Terminate oversized responses early with `TurboOptions(maxResponseBytes: 10 * 1024 * 1024)`.
> - **Standard Dio Features**: Fully supports Dio's `CancelToken`, `sendTimeout`, and `receiveTimeout`.

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
| `maxConcurrentIsolates` | `int?` | `max(2, cores - 2)` | Maximum concurrent worker Isolates permitted by the internal semaphore, preventing CPU thread starvation during 10-20x burst requests. |

---

## 📊 Performance & Benchmarks

Compared to traditional HTTP/1.1 and streaming Dart adapters, `flutter_turbo_fetch` delivers:

- **Up to 3x faster end-to-end response times** under high concurrency.
- **Zero UI frame drops (60/120 FPS preserved)** during multi-megabyte JSON decoding and model mapping.
- **Minimal memory footprints** via native Okio/CFNetwork buffering and zero-copy Isolate transfers.

To run benchmarks locally or view full empirical metrics on physical/emulator hardware:

- **Microbenchmarks**: Run `flutter test test/turbo_benchmark_test.dart`
- **End-to-End Suite**: Uses a local Node.js HTTP/2 mock server with self-signed TLS (`allowBadCertificates: true`). See [`example/`](https://github.com/khirohis/flutter_turbo_fetch/tree/main/example) for server setup and [Benchmark Results](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmark-results.md) for detailed metrics.

---

## 📚 Technical Documentation

For in-depth architecture and design rationale, see the [docs/](https://github.com/khirohis/flutter_turbo_fetch/tree/main/docs) directory in the GitHub repository:

- [Design Rationale & Bottleneck Analysis](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/design-rationale.md): Technical justifications behind avoiding stream FFI, native JSON parsing, and persistent worker pools.
- [Benchmark Results & Performance Analysis](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmark-results.md): Cross-platform empirical measurements and frame-timing evaluations on Android and Windows.
- [TurboTransformer Implementation Reference](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/turbo-transformer.md): In-depth guide to zero-copy Isolate transformations and semaphore governance.

For internal development specifications and agent directives (maintained in Japanese), refer to [specs/](https://github.com/khirohis/flutter_turbo_fetch/tree/main/specs).

---

## 📄 License

This project is licensed under the MIT License.
