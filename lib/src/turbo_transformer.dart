import 'dart:async';
import 'dart:convert';
import 'dart:isolate';
import 'dart:typed_data';
import 'package:dio/dio.dart';

/// UTF-8デコードとJSONデコードを結合した高速デコーダ（中間Stringを生成しない）
final _jsonUtf8Decoder = utf8.decoder.fuse(json.decoder);

class TurboTransformer extends BackgroundTransformer {
  /// 同時起動するIsolate数を制限するセマフォ（並列リクエスト時のスレッド飽和を防止）
  final _Semaphore _semaphore;

  /// Isolateへオフロードする閾値（並列通信時は 20KB〜32KB 推奨、デフォルト 24KB）
  final int isolateThresholdBytes;

  TurboTransformer({
    this.isolateThresholdBytes = 24 * 1024,
    int maxConcurrentIsolates = 3,
  }) : _semaphore = _Semaphore(maxConcurrentIsolates);

  @override
  Future transformResponse(
    RequestOptions options,
    ResponseBody responseBody,
  ) async {
    if (options.responseType == ResponseType.stream) {
      return responseBody;
    }

    // 1. ストリームからUint8Listを取得（copy: false により単一チャンクならゼロコピーで取り出し可能）
    final bytesBuilder = BytesBuilder(copy: false);
    await for (final chunk in responseBody.stream) {
      bytesBuilder.add(chunk);
    }
    final Uint8List bytes = bytesBuilder.takeBytes();

    // ① バイト列 (ResponseType.bytes) はそのまま O(1) ゼロコピー返却
    if (options.responseType == ResponseType.bytes) {
      return bytes;
    }

    // ② テキスト (ResponseType.plain)
    if (options.responseType == ResponseType.plain) {
      if (bytes.isEmpty) return '';
      if (bytes.lengthInBytes < isolateThresholdBytes) {
        return utf8.decode(bytes, allowMalformed: true);
      }
      return await _semaphore.withResource(() async {
        final transferable = TransferableTypedData.fromList([bytes]);
        return await Isolate.run(() {
          final rawBytes = transferable.materialize().asUint8List();
          return utf8.decode(rawBytes, allowMalformed: true);
        });
      });
    }

    // ③ JSONパースオブジェクト (ResponseType.json)
    if (bytes.isEmpty) return null;
    if (bytes.lengthInBytes < isolateThresholdBytes) {
      return _jsonUtf8Decoder.convert(bytes);
    }

    return await _semaphore.withResource(() async {
      // 【行き】TransferableTypedData で O(1) ゼロコピー転送
      final transferable = TransferableTypedData.fromList([bytes]);

      // 【帰り】Isolate.run は内部で Isolate.exit を呼ぶため、生成されたオブジェクトも O(1) ゼロコピー返却
      return await Isolate.run(() {
        final rawBytes = transferable.materialize().asUint8List();
        return _jsonUtf8Decoder.convert(rawBytes);
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
