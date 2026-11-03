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

class TurboTransformer extends BackgroundTransformer {
  /// Default byte threshold (24KiB) above which parsing is offloaded to a worker Isolate.
  static const int defaultIsolateThresholdBytes = 24 * 1024;

  /// Default minimum number of concurrent worker Isolates.
  static const int defaultMinConcurrentIsolates = 4;

  /// Semaphore to constrain concurrent Isolates and prevent OS thread saturation.
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
    return await _semaphore.withResource(() async {
      final transferable = TransferableTypedData.fromList([bytes]);
      return await Isolate.run(() {
        final rawBytes = transferable.materialize().asUint8List();
        return mapper(rawBytes);
      });
    });
  }

  Future<dynamic> _transformPlain(
    RequestOptions options,
    ResponseBody responseBody,
  ) async {
    final mapper = _extractMapper(options);
    final bytes = await _readBytes(responseBody.stream);
    if (bytes.isEmpty) {
      return mapper != null ? mapper('') : '';
    }
    if (bytes.lengthInBytes < isolateThresholdBytes) {
      final decoded = utf8.decode(bytes, allowMalformed: true);
      return mapper != null ? mapper(decoded) : decoded;
    }
    return await _semaphore.withResource(() async {
      final transferable = TransferableTypedData.fromList([bytes]);
      return await Isolate.run(() {
        final rawBytes = transferable.materialize().asUint8List();
        final decoded = utf8.decode(rawBytes, allowMalformed: true);
        return mapper != null ? mapper(decoded) : decoded;
      });
    });
  }

  Future<dynamic> _transformJson(
    RequestOptions options,
    ResponseBody responseBody,
  ) async {
    final mapper = _extractMapper(options);
    final bytes = await _readBytes(responseBody.stream);
    if (bytes.isEmpty) {
      return mapper != null ? mapper(null) : null;
    }
    if (bytes.lengthInBytes < isolateThresholdBytes) {
      final decoded = _jsonUtf8Decoder.convert(bytes);
      return mapper != null ? mapper(decoded) : decoded;
    }
    return await _semaphore.withResource(() async {
      // Inbound: O(1) zero-copy transfer of raw byte buffer
      final transferable = TransferableTypedData.fromList([bytes]);

      // Outbound: Isolate.run uses Isolate.exit to transfer parsed object tree in O(1)
      return await Isolate.run(() {
        final rawBytes = transferable.materialize().asUint8List();
        final decoded = _jsonUtf8Decoder.convert(rawBytes);
        return mapper != null ? mapper(decoded) : decoded;
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
