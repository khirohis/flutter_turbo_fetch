# TurboTransformer リファレンス実装・設計解説

本ドキュメントは、[`docs/architectures/dart-layer.md`](../architectures/dart-layer.md) で定義された `TurboTransformer` の設計意図とリファレンス実装を記録した補足資料である（通常時は参照不要、該当機能の実装・改修時のみ参照する）。

---

## 1. 設計のポイント

1. **3 種類のレスポンス形式（バイト列・テキスト・JSON）をすべてゼロコピー対応**
   - **`ResponseType.bytes` (`Uint8List`)**: ネイティブ層（Android `OkHttp` / iOS `URLSession`）が結合済みの `Uint8List` を 1 チャンクだけ流すため、`BytesBuilder(copy: false)` を用いることで再アロケーション・メモリコピーなしでバッファを取り出してそのまま返却する。
   - **`ResponseType.plain` (`String`)**: Dio 標準の `BackgroundTransformer` は巨大テキストでも Main Isolate で同期 `utf8.decode` を実行してしまうが、`TurboTransformer` では JSON と同様に閾値以上なら Isolate へオフロードする。
   - **`ResponseType.json` (`dynamic`)**: `utf8.decoder.fuse(json.decoder)` により中間 `String` を作らずバイト列から直接オブジェクトツリーを生成する。
2. **閾値分岐（デフォルト 24KB）による Isolate 起動オーバーヘッド回避**
   - 小さいデータ（〜24KB 未満）では `Isolate.run` の起動コスト（約 0.5〜1.5ms）の方が高くつくため、Main Isolate 上で直接デコードする（0.5ms 以下）。
3. **往復 O(1) ゼロコピー (`TransferableTypedData` + `Isolate.run`)**
   - **行き**: `TransferableTypedData.fromList([bytes])` により、Main Isolate から Worker Isolate へ O(1) でメモリ所有権を移譲する。
   - **帰り**: `Isolate.run`（内部で `Isolate.exit` を使用）により、Worker 終了時に共有ヒープ上のポインタ所有権を Main Isolate へ移すだけで返却が完了する（常駐 Isolate の `SendPort.send` で発生する Transitive Copy を回避）。
4. **セマフォによる同時起動 Isolate 数の制限**
   - 10〜20 並列リクエスト時に無制限に Isolate がスポーンされて OS スレッドが飽和するのを防ぐため、セマフォで同時実行数を制限（デフォルト 3）する。

---

## 2. リファレンス実装コード

```dart
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
```
