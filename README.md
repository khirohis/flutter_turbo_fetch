# flutter_turbo_fetch

[![pub package](https://img.shields.io/pub/v/flutter_turbo_fetch.svg)](https://pub.dev/packages/flutter_turbo_fetch)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)

A high-performance, non-blocking networking plugin for Flutter (`package:dio`).

`flutter_turbo_fetch` replaces traditional Dart-based HTTP streaming bottlenecks with **platform-native batch reception** (Android `OkHttp3` or optional `Cronet` / iOS & macOS `URLSession` / Windows `WinHTTP` / Linux `libcurl`) and **zero-copy Isolate offloading** (`TransferableTypedData` + semaphore-governed `Isolate.run`). It guarantees a silky-smooth 60/120 FPS UI thread even under heavy parallel HTTP/2 JSON, text, or binary workloads.

---

## ✨ Features

- **Native HTTP/2 Batch Reception**: Zero per-chunk FFI overhead via OS-level buffering (OkHttp3 / Cronet, URLSession, WinHTTP, & libcurl).
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
| **Android** | `minSdk = 24` (Android 7.0+) • `OkHttp3` (default) or `Cronet` | Tested & Benchmarked (Pixel 10a, OkHttp3 & Cronet) |
| **iOS** | `iOS 13.0+` • `URLSession` (SPM & CocoaPods) | Tested & Benchmarked (iPhone 17 & iPhone 6s, Release) |
| **macOS** | `macOS 10.15+` • `URLSession` (SPM & CocoaPods) | Tested (Benchmarking Pending) |
| **Windows** | `Windows 10 (1809+)` (64-bit) • `WinHTTP` (`winhttp.dll`) | Tested & Benchmarked |
| **Linux** | `Ubuntu 20.04+ / Debian 11+` (64-bit) • `libcurl` | Tested & Benchmarked |
| **Web** | Not Supported | Out of Scope |

> [!NOTE]
>
> - **Mobile & Desktop Benchmarks**: Split-pipeline rigorous Release benchmarks (`N=10` trials + 1:1 server audit) are published for [Android (`OkHttp3` & `Cronet`)](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmarks/android.md), [iOS](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmarks/ios.md), [Windows](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmarks/windows.md), and [Linux](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmarks/linux.md).
> - **macOS Status**: Native implementation is complete and unit/integration-tested; formal end-to-end performance benchmarking report is pending.
> - **Web**: Web is not supported due to native OS transport requirements.

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
  flutter_turbo_fetch: ^0.1.1
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

#### Optional: Using Cronet on Android

By default, Android uses `OkHttp3`. To switch to `Cronet` at build time—while **completely excluding `OkHttp3`** from your APK/AAB—add the following property to your app's `android/gradle.properties`:

```properties
flutter_turbo_fetch.androidEngine=cronet
```

1. **Add a Cronet Provider Dependency**: Because `flutter_turbo_fetch` depends only on the Cronet API artifact, add your preferred provider implementation in `android/app/build.gradle(.kts)`:
   - **Google Play Services (minimal APK size)**: `implementation("com.google.android.gms:play-services-cronet:<version>")` (recommended; also call `CronetProviderInstaller.installProvider(context)` on app startup before making requests).
   - **Standalone Embedded (works without GMS, larger APK size)**: `implementation("org.chromium.net:cronet-embedded:<version>")` (or `cronet-bundled` for newer `500.x` artifacts).
2. **Cronet-Specific Limitations**:
   - `allowBadCertificates: true` is **not supported** by Cronet's standard API and fails immediately with `DioExceptionType.unknown`; use Android's Network Security Config XML for local self-signed certificates.
   - Non-empty request bodies **must** include a `Content-Type` header; requests with a non-empty body and no `Content-Type` fail with `DioExceptionType.connectionError`.
   - When `followRedirects: false` is set, 3xx responses return their status code and headers (including `Location`) with an empty response body (`Uint8List(0)`).

### iOS

No special permissions are needed for HTTPS endpoints. If testing locally with cleartext HTTP or self-signed certificates, configure `NSAppTransportSecurity` in `ios/Runner/Info.plist`:

```xml
<key>NSAppTransportSecurity</key>
<dict>
    <key>NSAllowsLocalNetworking</key>
    <true/>
</dict>
```

### macOS

For macOS apps running with App Sandbox enabled (the default in Flutter), ensure outgoing network connections are permitted in both `macos/Runner/DebugProfile.entitlements` and `macos/Runner/Release.entitlements`:

```xml
<key>com.apple.security.network.client</key>
<true/>
```

### Windows

No additional setup or external libraries are required. The plugin links directly against the Windows OS system component `winhttp.dll`.

### Linux

Ensure `libcurl` and OpenSSL development headers are installed on your build machine:

```bash
sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev
```

---

## 💡 Quick Start

Configure a single `Dio` instance once at application startup.

`TurboOptions` inherits Dio's standard `responseType = ResponseType.json` default. By passing `mapper`, JSON decoding and domain model instantiation (`.fromJson`) execute **entirely inside a Worker Isolate** for payloads >= 24KB, returning typed objects to the UI thread in `O(1)`:

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

Configured once on the `Dio` instance to manage transport security:

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `allowBadCertificates` | `bool` | `false` | Allows self-signed certificates for local development and mock servers. (Not supported with Android Cronet; use Android Network Security Config instead.) |

### `TurboTransformer`

Controls Isolate dispatching and thread concurrency:

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `isolateThresholdBytes` | `int` | `24 * 1024` (24KB) | Payloads below this threshold are decoded synchronously on the Main Isolate (< 0.5ms). Payloads at or above this threshold are dispatched to Worker Isolates via zero-copy `TransferableTypedData`. |
| `maxConcurrentIsolates` | `int?` | `max(2, cores - 2)` | Maximum concurrent worker Isolates permitted by the internal semaphore, preventing CPU thread starvation during 10-20x burst requests. |

> [!TIP]
> **Performance Tuning for High-End Devices (Apple Silicon / Flagship Android)**:
>
> The default `24KB` threshold and semaphore limit prioritize 60/120 FPS safety on entry-to-mid-range hardware. On modern flagship CPUs, raising `isolateThresholdBytes` to **`128KB–256KB`** (and `maxConcurrentIsolates` to **`4–6`** for 10+ concurrent bursts) avoids Worker Isolate dispatch overhead on mid-sized payloads while preserving UI frame safety. See [Benchmark Results & Tuning Analysis](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmark-results.md#33-hardware-characteristics--tuning-on-high-performance-mobile-chips) for empirical break-even data.
>
> ```dart
> dio.transformer = TurboTransformer(
>   isolateThresholdBytes: 128 * 1024, // 128KB–256KB sweet spot on flagship devices
> );
> ```

---

## 📊 Performance & Benchmarks

Under the controlled split-pipeline Release protocol (`N=10` trials + 1:1 server audit), `flutter_turbo_fetch` delivers:

- **2.0x to 56.8x faster throughput than pure Dart HTTP/2 (`dio_http2`)** under concurrent workloads (Scenarios B & C), and **up to 3.8x faster than standard `IOHttpClientAdapter` (HTTP/1.1)** on desktop, Android, and entry-tier iOS devices (up to **1.32x** on iPhone 6s).
- **Zero UI frame drops (60/120 FPS preserved)** during large JSON decoding and model mapping via threshold-based Worker Isolate offloading.
- **Minimal memory footprints** via platform-native buffering (`Okio` / `Cronet` / `CFNetwork` / `WinHTTP` / `libcurl`) and zero-copy Isolate transfers.

To view full empirical metrics or run benchmarks locally:

- **Cross-Platform Summary**: See [Benchmark Results & Performance Analysis](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmark-results.md) for side-by-side results across [Android (`OkHttp3` & `Cronet`)](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmarks/android.md), [iOS](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmarks/ios.md), [Windows](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmarks/windows.md), and [Linux](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmarks/linux.md).
- **Microbenchmarks**: Run `flutter test test/turbo_benchmark_test.dart`
- **End-to-End Suite**: Uses a local Node.js HTTP/2 TLS mock server (`allowBadCertificates: true`, or Android Network Security Config for Cronet). See [`example/`](https://github.com/khirohis/flutter_turbo_fetch/tree/main/example) for server and app setup.

---

## 📚 Technical Documentation

For in-depth architecture and design rationale, see the [docs/](https://github.com/khirohis/flutter_turbo_fetch/tree/main/docs) directory in the GitHub repository:

- [Design Rationale & Bottleneck Analysis](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/design-rationale.md): Technical justifications behind avoiding stream FFI, native JSON parsing, and persistent worker pools.
- [Benchmark Results & Performance Analysis](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/benchmark-results.md): Cross-platform empirical measurements, split-pipeline analysis, and hardware evaluations on Android, iOS, Windows, and Linux.
- [TurboTransformer & TurboOptions Guide](https://github.com/khirohis/flutter_turbo_fetch/blob/main/docs/turbo-transformer.md): Architecture overview, JSON usage examples, and conceptual sketches of zero-copy Isolate transformations and semaphore governance.

For internal development specifications and agent directives (maintained in Japanese), refer to [specs/](https://github.com/khirohis/flutter_turbo_fetch/tree/main/specs).

---

## 📄 License

This project is licensed under the MIT License.
