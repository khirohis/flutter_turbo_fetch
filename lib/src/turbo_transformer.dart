import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:isolate';
import 'dart:math' as math;
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'turbo_options.dart';

/// UTF-8デコードとJSONデコードを結合した高速デコーダ（中間Stringを生成しない）
final _jsonUtf8Decoder = utf8.decoder.fuse(json.decoder);

class TurboTransformer extends BackgroundTransformer {
  /// 同時起動するIsolate数を制限するセマフォ（並列リクエスト時のスレッド飽和を防止）
  final _Semaphore _semaphore;

  /// Isolateへオフロードする閾値（並列通信時は 20KB〜32KB 推奨、デフォルト 24KB）
  final int isolateThresholdBytes;

  /// 同時起動する最大Isolate数
  final int maxConcurrentIsolates;

  TurboTransformer({
    int isolateThresholdBytes = 24 * 1024,
    int? maxConcurrentIsolates,
  }) : this._(
         isolateThresholdBytes: isolateThresholdBytes,
         maxConcurrentIsolates:
             maxConcurrentIsolates ??
             math.max(4, Platform.numberOfProcessors - 2),
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
    final mapper =
        options.extra[turboMapperExtraKey] as dynamic Function(dynamic)?;

    switch (options.responseType) {
      case ResponseType.stream:
        return _transformStream(responseBody, mapper);
      case ResponseType.bytes:
        return _transformBytes(responseBody, mapper);
      case ResponseType.plain:
        return _transformPlain(responseBody, mapper);
      case ResponseType.json:
        return _transformJson(responseBody, mapper);
    }
  }

  Future<dynamic> _transformStream(
    ResponseBody responseBody,
    dynamic Function(dynamic)? mapper,
  ) async {
    return mapper != null ? mapper(responseBody) : responseBody;
  }

  Future<dynamic> _transformBytes(
    ResponseBody responseBody,
    dynamic Function(dynamic)? mapper,
  ) async {
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
    ResponseBody responseBody,
    dynamic Function(dynamic)? mapper,
  ) async {
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
    ResponseBody responseBody,
    dynamic Function(dynamic)? mapper,
  ) async {
    final bytes = await _readBytes(responseBody.stream);
    if (bytes.isEmpty) {
      return mapper != null ? mapper(null) : null;
    }
    if (bytes.lengthInBytes < isolateThresholdBytes) {
      final decoded = _jsonUtf8Decoder.convert(bytes);
      return mapper != null ? mapper(decoded) : decoded;
    }
    return await _semaphore.withResource(() async {
      // 【行き】TransferableTypedData で O(1) ゼロコピー転送
      final transferable = TransferableTypedData.fromList([bytes]);

      // 【帰り】Isolate.run は内部で Isolate.exit を呼ぶため、生成されたカスタムオブジェクトも O(1) ゼロコピー返却
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
