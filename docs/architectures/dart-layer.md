# Dart 層設計（`TurboHttpAdapter` & `TurboTransformer`）

本ドキュメントでは、Dart 層で提供する公開 API（Dio 用 `TurboHttpAdapter` と `TurboTransformer`）の責務およびフォールバック仕様を定義する。

---

## 1. 利用側アプリからの導入インターフェース

利用側アプリが最小の変更（2 行の設定）で既存の `Dio` インスタンスに導入できる公開 API とする。

```dart
final dio = Dio();
dio.httpClientAdapter = TurboHttpAdapter(
  maxRequestsPerHost: 32,
  allowBadCertificates: false,
);
dio.transformer = TurboTransformer();
```

---

## 2. `TurboHttpAdapter`（Dio `HttpClientAdapter`）の責務とスコープ

### 2.1 サポート対象（ネイティブ一括受信経路）

- **対応プラットフォーム**: Android / iOS（それ以外の OS で実行された場合は `UnsupportedError` をスローする）
- **HTTP メソッド**: GET / POST / PUT / DELETE / PATCH / HEAD 等の基本メソッド
- **リクエスト**: リクエストヘッダ、リクエストボディ（`Stream<Uint8List>` をサイズ制限なしで `Uint8List` として集約）
- **レスポンス**: ステータスコード、レスポンスヘッダ、一括受信ボディ（単一チャンクの `ResponseBody.fromBytes(bytes, statusCode, headers: ...)` として返却）
- **制御機能・オプション**:
  - 1 ホストあたりの同時リクエスト上限設定（`maxRequestsPerHost`、デフォルト `32`）
  - 自己署名証明書の許可設定（`allowBadCertificates`、ローカル検証環境向け、デフォルト `false`）
  - タイムアウト制御（`connectTimeout`, `receiveTimeout`, `sendTimeout`）
  - キャンセル連動（`cancelFuture` 発火時にネイティブの `cancelRequest(requestId)` を呼び出す）
  - リダイレクト追従設定（`followRedirects`, `maxRedirects`）

### 2.2 除外・自動フォールバック対象

- `options.responseType == ResponseType.stream`（SSE / ストリーミング受信）が指定された場合のみ、内部で保持する標準 `IOHttpClientAdapter` へ**自動フォールバック**する。

### 2.3 エラーハンドリング（`DioException` マッピング）

- ネイティブ層（Android `OkHttp` / iOS `URLSession`）から返却された共通エラーコードを、Dio 標準の `DioExceptionType`（`connectionTimeout`, `sendTimeout`, `receiveTimeout`, `badCertificate`, `cancel`, `connectionError`, `unknown`）へ一貫してマッピングする。
- HTTP ステータスコード（4xx / 5xx）自体はネイティブ層で例外とせず、通常のレスポンス（ステータスコード＋レスポンスボディ＋ヘッダ）として Dart へ返し、Dio 本体の `validateStatus` ロジックに委ねる。

---

## 3. `TurboTransformer` の設計仕様

`TurboHttpAdapter` から渡された単一チャンクの `Uint8List` を、Main Isolate をブロックせず最小のコピーコストで **バイト列・テキスト・JSON パースオブジェクトの 3 種類** に変換する。

- **単一チャンクのゼロコピー受領**: `BytesBuilder(copy: false)` でストリームから `Uint8List` を取り出す。
- **取得データ種別ごとの変換処理**:
  1. **バイト列 (`ResponseType.bytes` → `Uint8List`)**: 取り出した単一チャンクの `Uint8List` をそのまま O(1) ゼロコピーで返却する。
  2. **テキスト (`ResponseType.plain` → `String`)**: 閾値未満は Main Isolate で `utf8.decode`、閾値以上は Isolate オフロードでデコードして返却する。
  3. **JSON パースオブジェクト (`ResponseType.json` → `dynamic`)**: `utf8.decoder.fuse(json.decoder)` を使用し、中間 `String` を生成せずにバイト列から直接デコードする（閾値未満は Main Isolate、閾値以上は Isolate オフロード）。
- **Isolate オフロード制御（閾値デフォルト 24KB / セマフォデフォルト最大同時 3）**:
  - **閾値未満**: Isolate 起動コスト（約 0.5〜1.5ms）を回避するため、Main Isolate で同期変換する。
  - **閾値以上**: セマフォ制御のもと、行きは `TransferableTypedData`、帰りは `Isolate.run`（`Isolate.exit`）により往復 O(1) ゼロコピーでオフロードする。
- **リファレンス実装**: 詳細な設計解説とコードは [`docs/references/turbo-transformer.md`](../references/turbo-transformer.md) を参照。
