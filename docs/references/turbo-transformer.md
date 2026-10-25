# TurboTransformer & TurboOptions リファレンス実装・設計解説

本ドキュメントは、[`docs/architectures/dart-layer.md`](../architectures/dart-layer.md) で定義された `TurboTransformer` および `TurboOptions` の設計意図とリファレンス実装を記録した補足資料である（通常時は参照不要、該当機能の実装・改修時のみ参照する）。

---

## 1. 設計のポイント

1. **Dio 標準の 4 種類のレスポンス形式（ストリーム・バイト列・テキスト・JSON）＋ カスタムオブジェクトマッピングをすべてサポート**
   - **`ResponseType.stream` (`ResponseBody`)**: `TurboHttpAdapter` が一括受信した `Uint8List` を `ResponseBody.fromBytes`（内部で `Stream.value(bytes)` をラップ）として渡すため、`TurboTransformer` がその `ResponseBody` をそのまま返却することで、Dio 標準の `ResponseType.stream` と 100% 互換の型・挙動をフォールバックなしで実現する。
   - **`ResponseType.bytes` (`Uint8List`)**: ネイティブ層（Android `OkHttp` / iOS `URLSession`）が結合済みの `Uint8List` を 1 チャンクだけ流すため、`BytesBuilder(copy: false)` を用いることで再アロケーション・メモリコピーなしでバッファを取り出してそのまま返却する（`TurboOptions(mapper: ...)` 指定時はバイナリパーサー等のマッパー処理も閾値に応じて Isolate オフロード可能）。
   - **`ResponseType.plain` (`String`)**: Dio 標準の `BackgroundTransformer` は巨大テキストでも Main Isolate で同期 `utf8.decode` を実行してしまうが、`TurboTransformer` では JSON と同様に閾値以上なら Isolate へオフロードする。
   - **`ResponseType.json` (`dynamic`)**: `utf8.decoder.fuse(json.decoder)` により中間 `String` を作らずバイト列から直接オブジェクトツリーを生成する。
   - **`TurboOptions(mapper: ...)` によるオブジェクトマッピング**: デコード結果（`ResponseBody` / `Uint8List` / `String` / JSON オブジェクト）を引数として受け取り、任意のドメインモデルへ変換する。24KB 以上の `bytes` / `plain` / `json` では Worker Isolate 内でマッパー関数まで一気通貫で実行し、完成したモデルインスタンスを `Isolate.exit` で O(1) ゼロコピー返却する。
2. **閾値分岐（デフォルト 24KB）による Isolate 起動オーバーヘッド回避**
   - 小さいデータ（〜24KB 未満）では `Isolate.run` の起動コスト（約 0.5〜1.5ms）の方が高くつくため、Main Isolate 上で直接デコード（＋マッピング）する（0.5ms 以下）。
3. **往復 O(1) ゼロコピー (`TransferableTypedData` + `Isolate.run`)**
   - **行き**: `TransferableTypedData.fromList([bytes])` により、Main Isolate から Worker Isolate へ O(1) でメモリ所有権を移譲する。
   - **帰り**: `Isolate.run`（内部で `Isolate.exit` を使用）により、Worker 終了時に共有ヒープ上のポインタ所有権を Main Isolate へ移すだけで返却が完了する（常駐 Isolate の `SendPort.send` で発生する Transitive Copy を回避）。
4. **セマフォによる同時起動 Isolate 数の制限（論理コア数連動＋カスタマイズ可能）**
   - 10〜20 並列リクエスト時に無制限に Isolate がスポーンされて OS スレッドが飽和するのを防ぐため、セマフォで同時実行数を制限する。
   - デフォルト値は `dart:io` の `Platform.numberOfProcessors` を用いた `math.max(4, Platform.numberOfProcessors - 2)`（UI / Raster スレッド用の 2 コアを保護しつつ、8 コア機なら 6 並列、6 コア機なら 4 並列）とし、`TurboTransformer(maxConcurrentIsolates: ...)` で利用側からカスタマイズ可能とする。

---

## 2. リファレンス実装コード

### `lib/src/turbo_options.dart`

```dart
import 'package:dio/dio.dart';

typedef TurboResponseMapper<T> = T Function(dynamic data);

/// TurboTransformer 内部で extra からマッパーを取り出すためのキー
const String turboMapperExtraKey = '_turbo_response_mapper';

/// TurboHttpAdapter 内部で extra から最大レスポンスバイト数を取り出すためのキー
const String turboMaxResponseBytesExtraKey = '_turbo_max_response_bytes';

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
    super.maxRedirects,
    super.persistentConnection,
    super.requestEncoder,
    super.responseDecoder,
    super.listFormat,
  }) : super() {
    if (mapper != null || maxResponseBytes != null) {
      this.extra = <String, Object?>{
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
    this.isolateThresholdBytes = 24 * 1024,
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
```
