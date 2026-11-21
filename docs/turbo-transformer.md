# TurboTransformer & TurboOptions: Architecture & Usage Guide (`turbo-transformer.md`)

This document provides architectural highlights, JSON usage examples, and conceptual sketches for `TurboTransformer` and `TurboOptions`, as specified in [`specs/architectures/dart-layer.md`](../specs/architectures/dart-layer.md).

---

## 1. Architectural Highlights

### 1. Full Support for All 4 Dio ResponseTypes + Typed Model Mapping

- **`ResponseType.stream` (`ResponseBody`)**:
  `TurboHttpAdapter` receives and concatenates response bytes in the OS background, wrapping the result in a single-chunk `ResponseBody.fromBytes` (backed by `Stream.value(bytes)`). `TurboTransformer` returns this `ResponseBody` directly in $O(1)$ without falling back to a separate adapter or draining the stream, maintaining 100% type compatibility with Dio.
- **`ResponseType.bytes` (`Uint8List`)**:
  Because the native layer yields a pre-assembled byte array in a single chunk, `TurboTransformer` extracts the buffer via `BytesBuilder(copy: false)` with zero re-allocation. If `TurboOptions(mapper: ...)` is provided (e.g., custom binary decoders/protobuf), mapping is automatically offloaded to a Worker Isolate for payloads meeting or exceeding the size threshold.
- **`ResponseType.plain` (`String`)**:
  While Dio's standard `BackgroundTransformer` synchronously executes `utf8.decode` on the Main Isolate even for large text responses, `TurboTransformer` offloads text decoding to a Worker Isolate whenever the payload meets or exceeds the threshold.
- **`ResponseType.json` (`dynamic`)**:
  Fuses UTF-8 decoding and JSON decoding (`utf8.decoder.fuse(json.decoder)`), converting raw bytes directly into the target object graph without intermediate multi-megabyte `String` allocations.
- **Post-Processing & Object Mapping via `TurboOptions(mapper: ...)`**:
  In `flutter_turbo_fetch`, `ResponseType` dictates the primary decoding baseline (yielding `ResponseBody`, `Uint8List`, `String`, or parsed JSON). The `mapper` callback serves as a post-processing hook that executes directly on this decoded result. It can be utilized for domain model instantiation (e.g., `.fromJson`), binary deserialization (Protobuf/MessagePack), or background processing such as cache persistence. For payloads >= 24KiB in `bytes`, `plain`, and `json`, both the primary decoding and the `mapper` hook execute entirely inside the Worker Isolate, returning the final processed result to the Main Isolate via `Isolate.exit` in $O(1)$ zero-copy.

### 2. Threshold-Based Offloading (Default 24KiB)

Spawning an Isolate via `Isolate.run` incurs a baseline overhead of approximately 0.5 ms to 1.5 ms. For small payloads (< 24KiB), direct decoding on the Main Isolate is faster (< 0.5 ms) and avoids unnecessary thread context switches. Payloads >= 24KiB are offloaded to keep the Main UI thread free of frame drops.

### 3. Bidirectional $O(1)$ Zero-Copy (`TransferableTypedData` + `Isolate.run`)

- **Inbound (Main -> Worker)**: Uses `TransferableTypedData.fromList([bytes])` to transfer memory ownership to the Worker Isolate in $O(1)$ without heap copies.
- **Outbound (Worker -> Main)**: `Isolate.run` internally invokes `Isolate.exit`, transferring ownership of the parsed Dart object tree back to the Main Isolate in $O(1)$ upon isolate termination. This avoids the transitive deep copies (which typically consume 25% to 40% of parsing time) inherent to persistent worker pools using `SendPort.send`.

### 4. Semaphore-Governed Concurrency (CPU Core Awareness)

Spawning unconstrained Isolates during bursts of 10 to 20 concurrent network requests saturates OS threads and triggers CPU thrashing. `TurboTransformer` restricts concurrent Isolate execution using a lightweight async `_Semaphore`. The default limit is calculated via `dart:io` as `math.max(TurboTransformer.defaultMinConcurrentIsolates, Platform.numberOfProcessors - 2)` with a baseline of 2 (`defaultMinConcurrentIsolates = 2`), sustaining parser throughput on dual-core hardware (e.g., iPhone 6s) while strictly preserving 2 CPU cores for Flutter's UI and raster threads on devices with 4 or more cores.

---

## 2. Usage Examples (JSON)

`TurboOptions` inherits Dio's default `ResponseType.json`. Payloads >= 24KiB are automatically offloaded to a Worker Isolate with zero-copy transfer.

### 2.1 Without a Mapper (Raw Decoded JSON)

Use standard Dio requests (or `TurboOptions` without `mapper`) when you only need decoded JSON (`Map<String, dynamic>` / `List<dynamic>`). Fused UTF-8 and JSON decoding still executes inside a Worker Isolate above the threshold:

```dart
final dio = Dio()
  ..httpClientAdapter = TurboHttpAdapter()
  ..transformer = TurboTransformer();

// ResponseType.json is the default; payloads >= 24KiB decode in a Worker Isolate.
final response = await dio.get<Map<String, dynamic>>(
  'https://api.example.com/users/1',
  options: TurboOptions(maxResponseBytes: 10 * 1024 * 1024),
);

final Map<String, dynamic> rawJson = response.data!;
```

### 2.2 With a Mapper (In-Isolate Model Instantiation)

Pass a `mapper` via `TurboOptions.json<T>` to run JSON decoding **and** domain model instantiation inside the same Worker Isolate, returning only the final typed object to the Main Isolate in $O(1)$:

```dart
final response = await dio.get<UserModel>(
  'https://api.example.com/users/1',
  options: TurboOptions.json<UserModel>(
    mapper: (json) => UserModel.fromJson(json as Map<String, dynamic>),
    maxResponseBytes: 10 * 1024 * 1024,
  ),
);

final UserModel user = response.data!;
```

> [!TIP]
> Because `mapper` executes inside `Isolate.run` for payloads >= 24KiB, it must be a top-level function, `static` method, or pure closure that does not capture mutable external state.

---

## 3. Core Mechanics (Conceptual Sketches)

> [!NOTE]
> The code snippets below are simplified conceptual sketches intended to illustrate the core pipeline mechanics—they are **not** runnable or verbatim copies of the production code, and do not need to track minor implementation edits. For the authoritative implementation, refer directly to [`lib/src/turbo_options.dart`](../lib/src/turbo_options.dart) and [`lib/src/turbo_transformer.dart`](../lib/src/turbo_transformer.dart).

### 3.1 Options Propagation (`TurboOptions` -> `RequestOptions.extra`)

`TurboOptions` attaches `mapper` and `maxResponseBytes` to reserved keys in Dio's `Options.extra`. Typed factory constructors (`.json`, `.bytes`, `.plain`) bind `responseType` and narrow the `mapper` parameter type at compile time:

```dart
class TurboOptions<T> extends Options {
  TurboOptions({
    T Function(dynamic data)? mapper,
    int? maxResponseBytes,
    super.responseType = ResponseType.json,
    // ... standard Dio Options parameters forwarded via super
  }) {
    extra = {
      ...?extra,
      if (mapper != null) turboMapperExtraKey: mapper,
      if (maxResponseBytes != null) turboMaxResponseBytesExtraKey: maxResponseBytes,
    };
  }

  factory TurboOptions.json({required T Function(dynamic json) mapper, /* ... */}) =>
      TurboOptions<T>(mapper: mapper, responseType: ResponseType.json, /* ... */);

  // Similarly: TurboOptions.bytes (Uint8List -> T) and TurboOptions.plain (String -> T)
}
```

### 3.2 `ResponseType` Dispatch & 24KiB Hybrid Offloading

`TurboTransformer` extracts the single-chunk native buffer with `BytesBuilder(copy: false)` and dispatches by `ResponseType` and payload size:

```dart
final _jsonUtf8Decoder = utf8.decoder.fuse(json.decoder); // No intermediate String

class TurboTransformer extends BackgroundTransformer {
  Future<dynamic> transformResponse(RequestOptions options, ResponseBody body) {
    return switch (options.responseType) {
      ResponseType.stream => _returnStreamInO1(options, body),
      ResponseType.bytes  => _transformBytes(options, body), // Offload only if mapper != null && >= threshold
      ResponseType.plain  => _transformPayload(options, body, empty: '', decode: (b) => utf8.decode(b, allowMalformed: true)),
      ResponseType.json   => _transformPayload(options, body, empty: null, decode: _jsonUtf8Decoder.convert),
    };
  }

  Future<dynamic> _transformPayload(/* ... */) async {
    final bytes = await _readZeroCopy(body.stream); // BytesBuilder(copy: false)
    if (bytes.isEmpty) return mapper != null ? mapper(empty) : empty;

    dynamic process(Uint8List raw) {
      final decoded = decode(raw);
      return mapper != null ? mapper(decoded) : decoded;
    }

    return bytes.lengthInBytes < isolateThresholdBytes
        ? process(bytes)            // Fast synchronous path on Main Isolate
        : _offload(bytes, process); // Worker Isolate offload
  }
}
```

### 3.3 Bidirectional $O(1)$ Zero-Copy Offload

Raw bytes enter the Worker Isolate via `TransferableTypedData`, and the decoded/mapped result exits via `Isolate.run` (`Isolate.exit`), avoiding heap deep copies in both directions:

```dart
Future<R> _offload<R>(Uint8List bytes, R Function(Uint8List) action) {
  return _semaphore.withResource(() async {
    final transferable = TransferableTypedData.fromList([bytes]); // Inbound O(1)
    return Isolate.run(() {                                       // Outbound O(1) via Isolate.exit
      final rawBytes = transferable.materialize().asUint8List();
      return action(rawBytes);
    });
  });
}
```

### 3.4 Semaphore Concurrency Governance

A lightweight async semaphore bounds concurrent `Isolate.run` executions (defaulting to `max(2, Platform.numberOfProcessors - 2)`) to prevent thread saturation during request bursts:

```dart
class _Semaphore {
  int _permits;
  final _queue = <Completer<void>>[];

  Future<T> withResource<T>(Future<T> Function() action) async {
    if (_permits > 0) {
      _permits--;
    } else {
      final gate = Completer<void>();
      _queue.add(gate);
      await gate.future;
    }
    try {
      return await action();
    } finally {
      _queue.isNotEmpty ? _queue.removeAt(0).complete() : _permits++;
    }
  }
}
```
