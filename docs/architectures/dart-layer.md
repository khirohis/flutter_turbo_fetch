# Dart 層設計（`TurboHttpAdapter`、`TurboTransformer` & `TurboOptions`）

本ドキュメントでは、Dart 層で提供する公開 API（Dio 用 `TurboHttpAdapter`、`TurboTransformer`、および `TurboOptions`）の責務および変換仕様を定義する。

---

## 1. 利用側アプリからの導入インターフェース

```dart
final dio = Dio();
dio.httpClientAdapter = TurboHttpAdapter(
  maxRequestsPerHost: 32,
  allowBadCertificates: false,
);
dio.transformer = TurboTransformer(
  // maxConcurrentIsolates 未指定時は max(4, Platform.numberOfProcessors - 2)
);

// カスタムオブジェクトへのマッピングやリクエスト単位の上限指定を行う場合
final response = await dio.get<User>(
  '/users/1',
  options: TurboOptions(
    mapper: (data) => User.fromJson(data as Map<String, dynamic>),
    responseType: ResponseType.json, // デフォルトは ResponseType.json
    maxResponseBytes: 16 * 1024 * 1024, // 16MiB
  ),
);
final User user = response.data!;
```

---

## 2. `TurboHttpAdapter`（Dio `HttpClientAdapter`）の責務とスコープ

### 2.1 サポート対象（全 `ResponseType` をネイティブ一括受信で統一処理）

- **対応プラットフォーム**: Android / iOS（それ以外の OS で実行された場合は `UnsupportedError` をスローする。ただし `hostApi` が注入された場合は検証をスキップ）
- **HTTP メソッド**: GET / POST / PUT / DELETE / PATCH / HEAD 等の基本メソッド
- **リクエスト**:
  - リクエストヘッダ: ヘッダ値が `Iterable`（配列等）の場合はカンマ区切り（`', '`）文字列に自動結合してネイティブへ渡す。
  - リクエストボディ: `Stream<Uint8List>` をサイズ制限なしで `BytesBuilder(copy: false)` により `Uint8List` として集約。
- **レスポンス**: ステータスコード、レスポンスヘッダ（キーは小文字に統一）、一括受信ボディ（`ResponseBody.fromBytes(bytes, statusCode, headers: ...)` により単一チャンクの `Stream.value(bytes)` を内包した `ResponseBody` として返却。`ResponseType.stream` 指定時も別アダプタへフォールバックしない）
- **制御機能・オプション**:
  - `maxRequestsPerHost`: 1 ホストあたりの同時リクエスト上限（デフォルト `32`）
  - `allowBadCertificates`: 自己署名証明書の許可（ローカル検証用、デフォルト `false`）
  - `maxResponseBytes`: 最大許容レスポンスバイト数（`TurboOptions` でリクエスト単位に指定可能。未指定時は無制限）
  - `hostApi`: Pigeon 生成 API の依存性注入用パラメータ（テスト・モック用）
  - タイムアウト制御（`connectTimeout`, `receiveTimeout`, `sendTimeout`）
  - キャンセル連動（`cancelFuture` 発火時にネイティブの `cancelRequest(requestId)` を呼び出す）
  - `requestId` 生成仕様: ネイティブ層（キャンセル照合等）で使用する一意識別子。フォーマットは `'turbo_${Isolate.current.hashCode}_${++_requestCounter}'`（`_requestCounter` はクラス共通の静的連番）。
  - リダイレクト追従設定（`followRedirects` の真偽値制御。ネイティブ層がリダイレクト上限回数の標準設定プロパティを持たないため、`maxRedirects` による追加の回数制御は行わない）
- **ライフサイクル制御**:
  - `close({bool force = false})` メソッドをサポートし、クローズ後は新規の `fetch()` 呼び出しに対して `StateError` をスローする。

### 2.2 エラーハンドリング（`DioException` マッピング）

- ネイティブ層から返却された共通エラーコードを、Dio 標準の `DioExceptionType`（`connectionTimeout`, `sendTimeout`, `receiveTimeout`, `badCertificate`, `cancel`, `connectionError`, `unknown`）へ変換する。
  - `maxResponseBytesExceeded`（最大許容サイズ超過）は、エラーメッセージに上限バイト数を明記した上で `DioExceptionType.unknown` にマッピングする。
- HTTP ステータスコード（4xx / 5xx）はネイティブ層で例外とせず通常のレスポンスとして返し、Dio 本体の `validateStatus`（失敗時は `DioExceptionType.badResponse`）に委ねる。
- Pigeon ブリッジ層の予期せぬ例外発生時は `DioExceptionType.unknown`（キャンセル済みの場合は `DioExceptionType.cancel`）としてラップしてスローする。

---

## 3. `TurboTransformer` & `TurboOptions` の設計仕様

### 3.1 `TurboOptions<T>`（カスタムオブジェクトマッパー・リクエスト単位オプション指定）

- `Dio` 標準の `Options` を継承し、コンストラクタで `mapper: TurboResponseMapper<T>?`（`typedef TurboResponseMapper<T> = T Function(dynamic data)`）、`maxResponseBytes: int?`、および通常の `Options` パラメータ（`responseType` デフォルト `ResponseType.json` 等）を受け取る。
- 内部的には `Options.extra` の予約キーを通じて `TurboHttpAdapter`（`maxResponseBytes`）および `TurboTransformer`（`mapper`）へ設定を引き渡す。
- **Isolate 転送要件**: 閾値以上のレスポンス（`bytes` [mapper 指定時] / `plain` / `json`）では `mapper` も `Isolate.run`（Worker Isolate）内で実行するため、トップレベル関数、`static` メソッド、または外部のミュータブルな状態をキャプチャしていないクロージャを渡す仕様とする。

### 3.2 `TurboTransformer` の変換・オフロード処理

- **取得データ種別ごとの変換処理（全 4 種の `ResponseType` 対応）**:
  1. **ストリーム (`ResponseType.stream` → `ResponseBody` または `mapper(ResponseBody)`)**:
     - `TurboHttpAdapter` が生成した単一チャンクの `ResponseBody` をストリーム消費せずそのまま O(1) で返却する（`mapper` 指定時は Main Isolate 上で `mapper(responseBody)` を実行）。
  2. **バイト列 (`ResponseType.bytes` → `Uint8List` または `mapper(Uint8List)`)**:
     - `BytesBuilder(copy: false)` で単一チャンクの `Uint8List` をゼロコピー取得する。
     - `mapper` 未指定時: データサイズに関わらず Main Isolate 上でそのまま `Uint8List` を O(1) 返却する（Isolate 転送オーバーヘッドを回避）。
     - `mapper` 指定時: 閾値未満なら Main Isolate で `mapper(bytes)` を同期実行し、閾値以上なら Worker Isolate へオフロードして `mapper(rawBytes)` を実行・返却する。
  3. **テキスト (`ResponseType.plain` → `String` または `mapper(String)`)**:
     - 空データ（0 バイト）の場合: `mapper` 未指定なら `''`、指定時は `mapper('')` を返却する。
     - 閾値未満なら Main Isolate 上で `utf8.decode(bytes, allowMalformed: true)`（および指定時は `mapper`）を実行する。
     - 閾値以上なら Worker Isolate へオフロードして実行・返却する。
  4. **JSON (`ResponseType.json` → `dynamic` または `mapper(dynamic)`)**:
     - 空データ（0 バイト）の場合: `mapper` 未指定なら `null`、指定時は `mapper(null)` を返却する。
     - 閾値未満なら Main Isolate 上で `utf8.decoder.fuse(json.decoder)` で中間 `String` を作らずデコード（および指定時は `mapper`）を実行する。
     - 閾値以上なら Worker Isolate へオフロードして中間 `String` なしでデコード・オブジェクト生成まで完結させて返却する。
- **Isolate オフロード制御（`bytes` [mapper 指定時] / `plain` / `json` 共通）**:
  - **閾値（`isolateThresholdBytes`、デフォルト 24KiB）**: 閾値未満は Main Isolate で同期実行し、閾値以上は行き `TransferableTypedData`・帰り `Isolate.run`（`Isolate.exit`）により生成オブジェクトまで往復 O(1) ゼロコピーでオフロードする。
  - **同時起動 Isolate 数（`maxConcurrentIsolates`）**: セマフォで同時起動数を制限する。デフォルトは `math.max(4, Platform.numberOfProcessors - 2)` とし、コンストラクタ引数でカスタマイズ可能とする。
- **リファレンス実装**: [`docs/references/turbo-transformer.md`](../references/turbo-transformer.md) を参照。
