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
  ) async {
    final mapper =
        options.extra[turboMapperExtraKey] as dynamic Function(dynamic)?;

    // ① ストリーム (ResponseType.stream): 単一チャンク Uint8List をラップ済みの ResponseBody を返却
    if (options.responseType == ResponseType.stream) {
      return mapper != null ? mapper(responseBody) : responseBody;
    }

    // ストリームからUint8Listを取得（copy: false により単一チャンクならゼロコピーで取り出し可能）
    final bytesBuilder = BytesBuilder(copy: false);
    await for (final chunk in responseBody.stream) {
      bytesBuilder.add(chunk);
    }
    final Uint8List bytes = bytesBuilder.takeBytes();

    // ② バイト列 (ResponseType.bytes)
    if (options.responseType == ResponseType.bytes) {
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

    // ③ テキスト (ResponseType.plain)
    if (options.responseType == ResponseType.plain) {
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

    // ④ JSONパースオブジェクト (ResponseType.json)
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
}

class _Semaphore {
  final int maxConcurrent;
  int _current = 0;
  final _queue = <Completer<void>>[];

  _Semaphore(this.maxConcurrent);

  Future<T> withResource<T>(Future<T> Function() action) async {
    if (_current >= maxConcurrent) {
      final completer = Completer<void>();
      _queue.add(completer);
      await completer.future;
    }
    _current++;
    try {
      return await action();
    } finally {
      _current--;
      if (_queue.isNotEmpty) {
        _queue.removeAt(0).complete();
      }
    }
  }
}
