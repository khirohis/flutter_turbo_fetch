# Dart 層仕様（`TurboHttpAdapter` / `TurboTransformer` / `TurboOptions`）

> リファレンス実装および利用コード例については [`docs/turbo-transformer.md`](../../docs/turbo-transformer.md) を参照。

---

## 1. `TurboHttpAdapter`（Dio `HttpClientAdapter`）

### 1.1 責務と入出力契約

- **対応プラットフォーム**: Android / iOS / macOS / Windows。非対応環境ではコンストラクタで `UnsupportedError` をスローする（テスト用に `hostApi` が注入された場合を除く）。
- **リクエスト変換**:
  - ヘッダ値が `Iterable` の場合は `', '` 区切り文字列に結合する。
  - リクエストボディ（`Stream<Uint8List>?`）は `BytesBuilder(copy: false)` で単一の `Uint8List` に集約してネイティブへ渡す。
  - リダイレクト設定は `followRedirects`（真偽値）のみを反映する（ネイティブ層の仕様に合わせ `maxRedirects` による回数制限は行わない）。
- **レスポンス変換**:
  - すべての `ResponseType`（`stream` を含む）をネイティブ一括受信で処理し、別アダプタへのフォールバックは行わない。
  - 受信した `Uint8List` を `ResponseBody.fromBytes(bytes, statusCode, headers: ...)` で単一チャンクストリームとしてラップして返却する。レスポンスヘッダのキーは小文字に正規化する。
- **並行制御・キャンセル・ライフサイクル**:
  - `maxRequestsPerHost`: 1 ホストあたりの最大同時リクエスト数（デフォルト `32`）。
  - `allowBadCertificates`: 自己署名証明書の許可（ローカル検証用、デフォルト `false`）。
  - リクエストごとに一意な `requestId` を採番し、`cancelFuture` 発火時にネイティブの `cancelRequest(requestId)` を呼び出す。
  - `close({bool force = false})`: クローズ後は新規 `fetch()` に対して `StateError` をスローする。`force: true` の場合は管理中の実行中リクエストを即座にキャンセルする。

### 1.2 エラーマッピング（`DioException`）

- ネイティブ層の共通 `errorCode`（[`native-layer.md`](./native-layer.md) 参照）を対応する `DioExceptionType` へマッピングする。
- HTTP 4xx / 5xx ステータスはアダプタ層では例外とせず通常レスポンスとして返却し、Dio 本体の `validateStatus`（`DioExceptionType.badResponse`）に委ねる。
- Pigeon ブリッジ通信自体の例外は `DioExceptionType.unknown`（キャンセル済みの場合は `DioExceptionType.cancel`）としてラップする。

---

## 2. `TurboOptions<T>`（リクエスト単位オプション & 後処理フック）

- **2段階パイプライン（一次デコード ＋ 後処理フック）**:
  - `responseType` で一次デコード形式を決定し、その結果を `mapper`（`T Function(dynamic data)`）へ渡して任意の型 `T` へ変換する。
  - `maxResponseBytes`: リクエスト単位の最大許容レスポンスバイト数（未指定時は無制限）。
  - 設定値は内部的に `Options.extra` の予約キーを介して `TurboHttpAdapter` と `TurboTransformer` へ伝達する。
- **コンストラクタ体系**:
  - `TurboOptions({...})`: 汎用コンストラクタ（`responseType` 省略時は Dio 標準の `ResponseType.json` を継承）。
  - `TurboOptions.json({required T Function(dynamic json) mapper, ...})`: `ResponseType.json` 固定。
  - `TurboOptions.bytes({required T Function(Uint8List bytes) mapper, ...})`: `ResponseType.bytes` 固定。
  - `TurboOptions.plain({required T Function(String text) mapper, ...})`: `ResponseType.plain` 固定。
- **Isolate 転送制約**:
  - 閾値以上のレスポンスでは `mapper` も `Isolate.run` 内で実行されるため、トップレベル関数、`static` メソッド、または Isolate 間転送可能なクロージャである必要がある。

---

## 3. `TurboTransformer`（Dio `BackgroundTransformer` 拡張）

### 3.1 `ResponseType` 別の変換・オフロード仕様

`TurboHttpAdapter` から渡された単一チャンクの `ResponseBody` を `BytesBuilder(copy: false)` でゼロコピー抽出し、データ種別とサイズ（閾値 `isolateThresholdBytes`、デフォルト 24KiB）に応じて以下のように処理する。

| `ResponseType` | 一次デコード方式 | 空ボディ（0 byte）時 | 閾値未満（Main Isolate） | 閾値以上（Worker Isolate オフロード） |
| :--- | :--- | :--- | :--- | :--- |
| **`stream`** | ストリーム消費なし（`ResponseBody` 維持） | そのまま返却（または `mapper`） | Main Isolate で O(1) 返却（指定時は `mapper(responseBody)`） | オフロードしない（常に Main Isolate で O(1) 返却） |
| **`bytes`** | `BytesBuilder(copy: false)` | `Uint8List(0)`（または `mapper`） | Main Isolate で O(1) 返却（指定時は `mapper(bytes)`） | `mapper` 未指定時は Main Isolate で O(1) 返却。<br>`mapper` 指定時のみ Worker Isolate で `mapper(bytes)` を実行 |
| **`plain`** | `utf8.decode(bytes, allowMalformed: true)` | `''`（または `mapper('')`） | Main Isolate でデコード（＋指定時は `mapper`） | Worker Isolate でデコード（＋指定時は `mapper`）を一貫実行 |
| **`json`** | `utf8.decoder.fuse(json.decoder)`<br>（中間 `String` 生成なし） | `null`（または `mapper(null)`） | Main Isolate でデコード（＋指定時は `mapper`） | Worker Isolate でデコード（＋指定時は `mapper`）を一貫実行 |

### 3.2 Isolate オフロードとセマフォ制御

- **ゼロコピー往復転送**:
  - 閾値以上のオフロード時は、入力バイト列を `TransferableTypedData` で Worker Isolate へ $O(1)$ 転送し、一次デコードと `mapper` を同一 Worker 内で連続実行した上で、最終成果物のみを `Isolate.run`（`Isolate.exit`）で $O(1)$ 返却する。
- **同時起動 Isolate 数の制限（`maxConcurrentIsolates`）**:
  - セマフォにより同時起動数を制限する。デフォルト値は `math.max(2, Platform.numberOfProcessors - 2)`（最小保証定数 `defaultMinConcurrentIsolates = 2`）とし、UI / Raster スレッド用の CPU コアを保護する。
