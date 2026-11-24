import 'dart:async';
import 'dart:collection' show Queue;
import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:isolate';
import 'dart:math' as math;
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'turbo_options.dart';

/// Fused UTF-8 and JSON decoder that bypasses intermediate String allocations.
final _jsonUtf8Decoder = utf8.decoder.fuse(json.decoder);

typedef _ByteDecoder = dynamic Function(Uint8List bytes);

String _decodePlain(Uint8List bytes) =>
    utf8.decode(bytes, allowMalformed: true);

dynamic _decodeJson(Uint8List bytes) => _jsonUtf8Decoder.convert(bytes);

class TurboTransformer extends BackgroundTransformer {
  /// Default byte threshold (24KiB) above which parsing is offloaded to a worker Isolate.
  static const int defaultIsolateThresholdBytes = 24 * 1024;

  /// Default minimum number of concurrent worker Isolates.
  static const int defaultMinConcurrentIsolates = 2;

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
  }) : _semaphore = _Semaphore(maxConcurrentIsolates) {
    if (maxConcurrentIsolates < 1) {
      throw RangeError.value(
        maxConcurrentIsolates,
        'maxConcurrentIsolates',
        'Must be at least 1',
      );
    }
  }

  @override
  Future<dynamic> transformResponse(
    RequestOptions options,
    ResponseBody responseBody,
  ) async {
    final responseType = options.responseType;
    final mapper =
        options.extra[turboMapperExtraKey] as TurboResponseMapper<dynamic>?;

    if (responseType == ResponseType.stream) {
      return mapper != null ? mapper(responseBody) : responseBody;
    }

    final bytes = await _readBytes(responseBody.stream);

    if (responseType == ResponseType.bytes && mapper == null) {
      return bytes;
    }

    final (_ByteDecoder? decode, Object? emptyValue) = switch (responseType) {
      ResponseType.bytes => (null, bytes),
      ResponseType.plain => (_decodePlain, ''),
      ResponseType.json => (_decodeJson, null),
      ResponseType.stream => throw StateError('Unreachable'),
    };

    if (bytes.isEmpty) {
      return mapper != null ? mapper(emptyValue) : emptyValue;
    }

    if (bytes.lengthInBytes < isolateThresholdBytes) {
      return _applyPipeline(bytes, decode, mapper);
    }

    return _semaphore.withResource(() {
      // TransferableTypedData avoids heap deep copies on worker materialize().
      final transferable = TransferableTypedData.fromList([bytes]);
      return _runInIsolate(transferable, decode, mapper);
    });
  }

  Future<Uint8List> _readBytes(Stream<Uint8List> stream) async {
    final bytesBuilder = BytesBuilder(copy: false);
    await for (final chunk in stream) {
      bytesBuilder.add(chunk);
    }
    return bytesBuilder.takeBytes();
  }

  static dynamic _applyPipeline(
    Uint8List rawBytes,
    _ByteDecoder? decode,
    TurboResponseMapper<dynamic>? mapper,
  ) {
    final decoded = decode != null ? decode(rawBytes) : rawBytes;
    return mapper != null ? mapper(decoded) : decoded;
  }

  /// Runs primary decoding and optional mapping inside a worker Isolate.
  ///
  /// Kept `static` so the closure passed to [Isolate.run] captures only
  /// [transferable], [decode], and [mapper]—never `this` or surrounding locals.
  static Future<dynamic> _runInIsolate(
    TransferableTypedData transferable,
    _ByteDecoder? decode,
    TurboResponseMapper<dynamic>? mapper,
  ) {
    // Isolate.run uses Isolate.exit to return the parsed object graph in O(1).
    return Isolate.run(() {
      final rawBytes = transferable.materialize().asUint8List();
      return _applyPipeline(rawBytes, decode, mapper);
    });
  }
}

class _Semaphore {
  int _permits;
  final _queue = Queue<Completer<void>>();

  _Semaphore(this._permits);

  Future<T> withResource<T>(Future<T> Function() action) async {
    if (_permits > 0) {
      _permits--;
    } else {
      final completer = Completer<void>();
      _queue.addLast(completer);
      await completer.future;
    }

    try {
      return await action();
    } finally {
      if (_queue.isNotEmpty) {
        _queue.removeFirst().complete();
      } else {
        _permits++;
      }
    }
  }
}
