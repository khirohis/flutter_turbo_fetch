# TurboTransformer & TurboOptions: Architecture & Reference Guide (`turbo-transformer.md`)

This document provides architectural details, design rationale, and the reference implementation for `TurboTransformer` and `TurboOptions`, as specified in [`specs/architectures/dart-layer.md`](../specs/architectures/dart-layer.md).

---

## 1. Architectural Highlights

### 1. Full Support for All 4 Dio ResponseTypes + Typed Model Mapping

- **`ResponseType.stream` (`ResponseBody`)**:
  `TurboHttpAdapter` receives and concatenates response bytes in the OS background, wrapping the result in a single-chunk `ResponseBody.fromBytes` (backed by `Stream.value(bytes)`). `TurboTransformer` returns this `ResponseBody` directly in $O(1)$ without falling back to a separate adapter or draining the stream, maintaining 100% type compatibility with Dio.
- **`ResponseType.bytes` (`Uint8List`)**:
  Because the native layer yields a pre-assembled byte array in a single chunk, `TurboTransformer` extracts the buffer via `BytesBuilder(copy: false)` with zero re-allocation. If `TurboOptions(mapper: ...)` is provided (e.g., custom binary decoders/protobuf), mapping is automatically offloaded to an Isolate for payloads exceeding the size threshold.
- **`ResponseType.plain` (`String`)**:
  While Dio's standard `BackgroundTransformer` synchronously executes `utf8.decode` on the Main Isolate even for large text responses, `TurboTransformer` offloads text decoding to an Isolate whenever the payload meets or exceeds the threshold.
- **`ResponseType.json` (`dynamic`)**:
  Fuses UTF-8 decoding and JSON decoding (`utf8.decoder.fuse(json.decoder)`), converting raw bytes directly into the target object graph without intermediate multi-megabyte `String` allocations.
- **Object Mapping via `TurboOptions(mapper: ...)`**:
  Accepts decoded data (`ResponseBody`, `Uint8List`, `String`, or parsed JSON) and transforms it into domain models. For payloads >= 24KiB in `bytes`, `plain`, and `json`, the mapper executes entirely within the Worker Isolate, returning the final instantiated model graph to the Main Isolate via `Isolate.exit` in $O(1)$ zero-copy.

### 2. Threshold-Based Offloading (Default 24KiB)

Spawning an Isolate via `Isolate.run` incurs a baseline overhead of approximately 0.5ms to 1.5ms. For small payloads (< 24KiB), direct decoding on the Main Isolate is faster (< 0.5ms) and avoids unnecessary thread context switches. Payloads >= 24KiB are offloaded to keep the Main UI thread free of frame drops.

### 3. Bidirectional $O(1)$ Zero-Copy (`TransferableTypedData` + `Isolate.run`)

- **Inbound (Main -> Worker)**: Uses `TransferableTypedData.fromList([bytes])` to transfer memory ownership to the Worker Isolate in $O(1)$ without heap copies.
- **Outbound (Worker -> Main)**: `Isolate.run` internally invokes `Isolate.exit`, transferring ownership of the parsed Dart object tree back to the Main Isolate in $O(1)$ upon isolate termination. This avoids the transitive deep copies (which typically consume 25% to 40% of parsing time) inherent to persistent worker pools using `SendPort.send`.

### 4. Semaphore-Governed Concurrency (CPU Core Awareness)

Spawning unconstrained Isolates during bursts of 10 to 20 concurrent network requests saturates OS threads and triggers CPU thrashing. `TurboTransformer` restricts concurrent Isolate execution using a lightweight async `_Semaphore`. The default limit is calculated via `dart:io` as `math.max(TurboTransformer.defaultMinConcurrentIsolates, Platform.numberOfProcessors - 2)` with a baseline of 2 (`defaultMinConcurrentIsolates = 2`), sustaining parser throughput on dual-core hardware (e.g., iPhone 6s) while strictly preserving 2 CPU cores for Flutter's UI and raster threads on devices with 4 or more cores.

---

## 2. Reference Implementation

### `lib/src/turbo_options.dart`

```dart
import 'package:dio/dio.dart';

typedef TurboResponseMapper<T> = T Function(dynamic data);

/// Key used to store and retrieve the custom response mapper from RequestOptions.extra.
const String turboMapperExtraKey = '_turbo_response_mapper';

/// Key used to store and retrieve the maximum response byte limit from RequestOptions.extra.
const String turboMaxResponseBytesExtraKey = '_turbo_max_response_bytes';

/// Extended Dio [Options] supporting custom isolate model mapping and response safety limits.
class TurboOptions<T> extends Options {
  TurboOptions({
    TurboResponseMapper<T>? mapper,
    int? maxResponseBytes,
    super.method,
    super.sendTimeout,
    super.receiveTimeout,
    super.extra,
    super.headers,
    super.preserveHeaderCase,
    super.responseType = ResponseType.json,
    super.contentType,
    super.validateStatus,
    super.receiveDataWhenStatusError,
    super.followRedirects,
    super.maxRedirects, // Note: Ignored as native OkHttp/URLSession handle redirects internally
    super.persistentConnection,
    super.requestEncoder,
    super.responseDecoder,
    super.listFormat,
  }) : super() {
    if (mapper != null || maxResponseBytes != null) {
      extra = <String, Object?>{
        ...?extra,
        if (mapper != null) turboMapperExtraKey: mapper,
        if (maxResponseBytes != null)
          turboMaxResponseBytesExtraKey: maxResponseBytes,
      };
    }
  }
}
```

### `lib/src/turbo_transformer.dart`

```dart
import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:isolate';
import 'dart:math' as math;
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'turbo_options.dart';

/// Fused UTF-8 and JSON decoder that bypasses intermediate String allocations.
final _jsonUtf8Decoder = utf8.decoder.fuse(json.decoder);

/// High-performance Dio Transformer with threshold-based zero-copy Isolate offloading
/// and semaphore concurrency governance.
class TurboTransformer extends BackgroundTransformer {
  /// Default byte threshold (24KiB) above which parsing is offloaded to a worker Isolate.
  static const int defaultIsolateThresholdBytes = 24 * 1024;

  /// Default minimum number of concurrent worker Isolates.
  static const int defaultMinConcurrentIsolates = 2;

  final _Semaphore _semaphore;

  /// Byte threshold above which parsing is offloaded to a worker Isolate (default: [defaultIsolateThresholdBytes]).
  final int isolateThresholdBytes;

  /// Maximum number of concurrently running worker Isolates.
  final int maxConcurrentIsolates;

  TurboTransformer({
    int isolateThresholdBytes = defaultIsolateThresholdBytes,
    int? maxConcurrentIsolates,
  }) : this._(
         isolateThresholdBytes: isolateThresholdBytes,
         maxConcurrentIsolates:
             maxConcurrentIsolates ??
             math.max(
               defaultMinConcurrentIsolates,
               Platform.numberOfProcessors - 2,
             ),
       );

  TurboTransformer._({
    required this.isolateThresholdBytes,
    required this.maxConcurrentIsolates,
  }) : _semaphore = _Semaphore(maxConcurrentIsolates);

  @override
  Future transformResponse(
    RequestOptions options,
    ResponseBody responseBody,
  ) {
    switch (options.responseType) {
      case ResponseType.stream:
        return _transformStream(options, responseBody);
      case ResponseType.bytes:
        return _transformBytes(options, responseBody);
      case ResponseType.plain:
        return _transformPlain(options, responseBody);
      case ResponseType.json:
        return _transformJson(options, responseBody);
    }
  }

  dynamic Function(dynamic)? _extractMapper(RequestOptions options) {
    return options.extra[turboMapperExtraKey] as dynamic Function(dynamic)?;
  }

  Future<dynamic> _transformStream(
    RequestOptions options,
    ResponseBody responseBody,
  ) async {
    final mapper = _extractMapper(options);
    return mapper != null ? mapper(responseBody) : responseBody;
  }

  Future<dynamic> _transformBytes(
    RequestOptions options,
    ResponseBody responseBody,
  ) async {
    final mapper = _extractMapper(options);
    final bytes = await _readBytes(responseBody.stream);
    if (mapper == null) {
      return bytes;
    }
    if (bytes.lengthInBytes < isolateThresholdBytes) {
      return mapper(bytes);
    }
    return await _offload(bytes, mapper);
  }

  Future<dynamic> _transformPlain(
    RequestOptions options,
    ResponseBody responseBody,
  ) {
    return _transformPayload(
      options,
      responseBody,
      emptyValue: '',
      decode: (bytes) => utf8.decode(bytes, allowMalformed: true),
    );
  }

  Future<dynamic> _transformJson(
    RequestOptions options,
    ResponseBody responseBody,
  ) {
    return _transformPayload(
      options,
      responseBody,
      emptyValue: null,
      decode: _jsonUtf8Decoder.convert,
    );
  }

  /// Common pipeline for decodable payloads (plain text and JSON).
  Future<dynamic> _transformPayload(
    RequestOptions options,
    ResponseBody responseBody, {
    required dynamic emptyValue,
    required dynamic Function(Uint8List bytes) decode,
  }) async {
    final mapper = _extractMapper(options);
    final bytes = await _readBytes(responseBody.stream);

    if (bytes.isEmpty) {
      return mapper != null ? mapper(emptyValue) : emptyValue;
    }

    dynamic process(Uint8List rawBytes) {
      final decoded = decode(rawBytes);
      return mapper != null ? mapper(decoded) : decoded;
    }

    if (bytes.lengthInBytes < isolateThresholdBytes) {
      return process(bytes);
    }
    return await _offload(bytes, process);
  }

  /// Offloads [action] to a worker Isolate using bidirectional O(1) zero-copy transfer.
  Future<R> _offload<R>(
    Uint8List bytes,
    R Function(Uint8List rawBytes) action,
  ) async {
    return await _semaphore.withResource(() async {
      // Inbound: O(1) zero-copy transfer of raw byte buffer
      final transferable = TransferableTypedData.fromList([bytes]);

      // Outbound: Isolate.run uses Isolate.exit to transfer parsed object tree in O(1)
      return await Isolate.run(() {
        final rawBytes = transferable.materialize().asUint8List();
        return action(rawBytes);
      });
    });
  }

  Future<Uint8List> _readBytes(Stream<Uint8List> stream) async {
    final bytesBuilder = BytesBuilder(copy: false);
    await for (final chunk in stream) {
      bytesBuilder.add(chunk);
    }
    return bytesBuilder.takeBytes();
  }
}

/// Lightweight asynchronous semaphore for concurrency throttling.
class _Semaphore {
  final int maxConcurrent;
  int _permits;
  final _queue = <Completer<void>>[];

  _Semaphore(this.maxConcurrent) : _permits = maxConcurrent;

  Future<T> withResource<T>(Future<T> Function() action) async {
    if (_permits > 0) {
      _permits--;
    } else {
      final completer = Completer<void>();
      _queue.add(completer);
      await completer.future;
    }

    try {
      return await action();
    } finally {
      if (_queue.isNotEmpty) {
        _queue.removeAt(0).complete();
      } else {
        _permits++;
      }
    }
  }
}
```
