# ネイティブ層（Android / iOS）および通信ブリッジ仕様

> 通信方式（MethodChannel/Pigeon vs FFI）の比較背景については [`docs/design-rationale.md`](../../docs/design-rationale.md) を参照。

---

## 1. 概要 & ドキュメント構成

本パッケージのネイティブ層は、OS バックグラウンドでの HTTP/2 一括受信・チャンク結合、および完成した単一の生バイト配列（`Uint8List`）の一括返却を担当します。

- **共通通信ブリッジ仕様（本ドキュメント）**: Dart ⇄ ネイティブ間の Pigeon データモデルおよび共通エラーコード体系
- **Android ネイティブ層仕様**: [`android-native-layer.md`](./android-native-layer.md)（OkHttp3、3層クラス構成、リソース管理）
- **iOS ネイティブ層仕様**: [`ios-native-layer.md`](./ios-native-layer.md)（URLSession、CFNetwork completionHandler 一括受信）

---

## 2. Dart ⇄ ネイティブ間の通信ブリッジ仕様（Pigeon）

通信ブリッジには **Pigeon（`StandardMessageCodec` ベースの型安全コード生成、`@HostApi` + `@async`）** を採用し、完成したレスポンスボディ `Uint8List`（Android `byte[]` / iOS `FlutterStandardTypedData`）を 1 リクエストにつき 1 往復でやり取りする。

### 通信データモデル

- **リクエスト情報（`TurboHttpRequest`）**:
  - `requestId`: リクエスト識別子（キャンセル紐付け用）
  - `url`: リクエスト URL
  - `method`: HTTP メソッド（GET / POST / PUT / DELETE / PATCH / HEAD 等）
  - `headers`: リクエストヘッダ（`Map<String?, String?>`）
  - `body`: リクエストボディ（nullable `Uint8List`）
  - `connectTimeoutMs` / `receiveTimeoutMs` / `sendTimeoutMs`: タイムアウト設定（ミリ秒）
  - `followRedirects`: リダイレクト追従設定（真偽値。`OkHttp` / `URLSession` ともにリダイレクト上限回数の標準設定プロパティを持たないため、`maxRedirects` による追加の回数制御は行わない）
  - `maxRequestsPerHost`: 1 ホストあたりの最大同時リクエスト数（デフォルト `32`）
  - `allowBadCertificates`: 自己署名証明書等の許可設定（ローカル検証用、デフォルト `false`）
  - `maxResponseBytes`: レスポンスボディの最大許容バイト数（nullable、未指定時は無制限。超過時はデータを切り詰めて正常返却することはせず、上限超過を検知した時点で処理を中断してエラーコード `maxResponseBytesExceeded` を返す）
- **レスポンス情報（`TurboHttpResponse`）**:
  - `statusCode`: HTTP ステータスコード
  - `statusMessage`: ステータスメッセージ（任意）
  - `headers`: レスポンスヘッダ（マルチバリュー対応。Pigeon スキーマ上は `Map<String?, Object?>?` だが、実体はキーを小文字（lowercase）に正規化した `Map<String, List<String>>` として格納する）
  - `body`: 一括結合済みレスポンスボディ（`Uint8List`）
  - `errorCode` / `errorMessage`: エラー発生時の共通エラーコードと詳細メッセージ
- **キャンセル操作**:
  - `cancelRequest(requestId)`: 実行中のネイティブリクエストを即座に中断する
  - `cancelAllRequests()`: 実行中の全ネイティブリクエストを一括中断する（エンジン破棄・ホットリスタート対応）

---

## 3. プラットフォーム共通エラーコード体系

ネイティブ層で発生した例外・エラーは、Dart 層（`DioExceptionType`）と 1:1 に整合する共通エラーコードへ正規化して返却する。

| 共通エラーコード | 対応する `DioExceptionType` | 発生契機 |
| :--- | :--- | :--- |
| `cancel` | `DioExceptionType.cancel` | `cancelRequest` 呼び出し、または通信タスク中断 |
| `connectionTimeout` | `DioExceptionType.connectionTimeout` | TCP ハンドシェイク / TLS ネゴシエーション時のタイムアウト |
| `sendTimeout` | `DioExceptionType.sendTimeout` | リクエストボディの書き込み・送信中タイムアウト |
| `receiveTimeout` | `DioExceptionType.receiveTimeout` | サーバーからのレスポンス待機・受信中タイムアウト |
| `badCertificate` | `DioExceptionType.badCertificate` | SSL/TLS ハンドシェイク失敗、自己署名証明書等 |
| `maxResponseBytesExceeded` | `DioExceptionType.unknown` | 受信データ量が `maxResponseBytes` 上限を超過（エラーメッセージに上限値を明記） |
| `connectionError` | `DioExceptionType.connectionError` | 名前解決失敗（DNS）、接続拒否、ホスト到達不能、不正URL / ヘッダ |
| `unknown` | `DioExceptionType.unknown` | プラットフォーム内部の予期せぬ実行時例外 |
