# Dart 層設計（Dio Adapter & ZeroCopyJsonTransformer）

本ドキュメントでは、Dart 層で提供する公開 API（Dio 用 `HttpClientAdapter` と `ZeroCopyJsonTransformer`）の責務およびフォールバック仕様を定義する。

---

## 1. 利用側アプリからの導入インターフェース

利用側アプリが最小の変更（2 行の設定）で既存の `Dio` インスタンスに導入できる公開 API とする。

```dart
final dio = Dio();
dio.httpClientAdapter = NativeBatchHttpAdapter(); // 仮称（Step 0-2 / 2-4 で正式名称確定）
dio.transformer = ZeroCopyJsonTransformer();
```

---

## 2. Dio `HttpClientAdapter` の責務とスコープ（Step 0-2）

### 2.1 サポート対象（ネイティブ一括受信経路）

- **HTTP メソッド**: GET / POST / PUT / DELETE / PATCH / HEAD 等の基本メソッド
- **リクエスト**: リクエストヘッダ、リクエストボディ（`Stream<Uint8List>` を `Uint8List` として集約）
- **レスポンス**: ステータスコード、レスポンスヘッダ、一括受信ボディ（単一チャンクの `ResponseBody.fromBytes(bytes, statusCode, headers: ...)` として返却）
- **制御機能**:
  - タイムアウト制御（`connectTimeout`, `receiveTimeout`, `sendTimeout`）
  - キャンセル連動（`cancelFuture` 発火時にネイティブの `cancelRequest(requestId)` を呼び出す）
  - リダイレクト追従設定（`followRedirects`, `maxRedirects`）

### 2.2 除外・自動フォールバック対象

- `options.responseType == ResponseType.stream` 等による SSE / 無期限ストリーミング受信、または巨大ファイルのストリームアップロードなど、一括受信に適さないリクエストは、内部で保持する標準 `IOHttpClientAdapter` へ**自動フォールバック**する設計とする。

### 2.3 エラーハンドリング（`DioException` マッピング）

- ネイティブ層（Android `OkHttp` / iOS `URLSession`）から返却された共通エラーコードを、Dio 標準の `DioExceptionType`（`connectionTimeout`, `sendTimeout`, `receiveTimeout`, `badCertificate`, `cancel`, `connectionError`, `unknown`）へ一貫してマッピングする。
- HTTP ステータスコード（4xx / 5xx）自体はネイティブ層で例外とせず、通常のレスポンス（ステータスコード＋レスポンスボディ＋ヘッダ）として Dart へ返し、Dio 本体の `validateStatus` ロジックに委ねる。

---

## 3. `ZeroCopyJsonTransformer` の設計仕様

ネイティブアダプタから渡された単一チャンクの `Uint8List` を、Main Isolate をブロックせず最小のコピーコストで JSON パースする。

- **単一チャンクのゼロコピー受領**: `BytesBuilder(copy: false)` でストリームから `Uint8List` を取り出す。
- **直接 UTF-8 + JSON 変換**: `utf8.decoder.fuse(json.decoder)` を使用し、中間 `String` を生成せずにバイト列から直接デコードする。
- **サイズ閾値による分岐（デフォルト 24KB）**:
  - **閾値未満**: Isolate 起動コスト（約 0.5〜1.5ms）を回避するため、Main Isolate で同期変換する。
  - **閾値以上**: セマフォ（デフォルト最大同時 3）制御のもと、行きは `TransferableTypedData`、帰りは `Isolate.run`（`Isolate.exit`）により往復 O(1) ゼロコピーでオフロードする。
- **リファレンス実装**: 詳細な設計解説とコードは [`docs/references/zero-copy-json-transformer.md`](../references/zero-copy-json-transformer.md) を参照。
