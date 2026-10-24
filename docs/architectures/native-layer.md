# ネイティブ層（Android / iOS）および通信ブリッジ仕様

> 通信方式（MethodChannel/Pigeon vs FFI）の比較背景については [`docs/references/design-rationale.md`](../references/design-rationale.md) を参照。

---

## 1. Dart ⇄ ネイティブ間の通信ブリッジ仕様（Pigeon）

通信ブリッジには **Pigeon（`StandardMessageCodec` ベースの型安全コード生成、`@HostApi` + `@async`）** を採用し、完成したレスポンスボディ `Uint8List`（Android `byte[]` / iOS `FlutterStandardTypedData`）を 1 リクエストにつき 1 往復でやり取りする（詳細スキーマは Step 2-1 で確定）。

### 通信データモデル

- **リクエスト情報**:
  - `requestId`: リクエスト識別子（キャンセル紐付け用）
  - `url`: リクエスト URL
  - `method`: HTTP メソッド（GET / POST / PUT / DELETE / PATCH / HEAD 等）
  - `headers`: リクエストヘッダ
  - `body`: リクエストボディ（nullable `Uint8List`）
  - `connectTimeoutMs` / `receiveTimeoutMs` / `sendTimeoutMs`: タイムアウト設定（ミリ秒）
  - `followRedirects` / `maxRedirects`: リダイレクト追従設定
- **レスポンス情報**:
  - `statusCode`: HTTP ステータスコード
  - `statusMessage`: ステータスメッセージ（任意）
  - `headers`: レスポンスヘッダ（マルチバリュー対応）
  - `body`: 一括結合済みレスポンスボディ（`Uint8List`）
  - エラー発生時: 共通エラー種別コード＋詳細メッセージ
- **キャンセル操作**:
  - `cancelRequest(requestId)`: 実行中のネイティブリクエストを即座に中断する

---

## 2. Android ネイティブ層仕様（`OkHttp3`）

- **ターゲット・依存バージョン**:
  - `minSdk = 24`（Android 7.0+）
  - `com.squareup.okhttp3:okhttp:4.12.0`
- **クライアント構成**:
  - 共有の `OkHttpClient` インスタンス（`ConnectionPool`、HTTP/2 初期 16MB ウィンドウ）を保持し、リクエスト単位のタイムアウト・リダイレクト設定は `client.newBuilder()` で適用する。
- **一括受信処理**:
  - `Call.enqueue(Callback)` のバックグラウンドスレッド上で `response.body.bytes()` を呼び出し、チャンク結合完了後に Dart へ 1 回で返却する。
- **ライフサイクル・並行管理**:
  - `ConcurrentHashMap<String, Call>` で `requestId` と `okhttp3.Call` を管理し、`cancelRequest(requestId)` で `call.cancel()` を呼ぶ。
  - `FlutterPlugin.onDetachedFromEngine` 時に未完了リクエストのキャンセルおよびリソース解放（`dispatcher.executorService.shutdown()` 等）を行う。
- **エラー分類**:
  - `InterruptedIOException` / `call.isCanceled()` → `cancel`
  - `SocketTimeoutException` → `connectionTimeout` / `sendTimeout` / `receiveTimeout`
  - `UnknownHostException` / `ConnectException` → `connectionError`
  - `SSLException` / `CertificateException` → `badCertificate` / `connectionError`
  - その他 `IOException` → 通信エラー

---

## 3. iOS ネイティブ層仕様（`URLSession`）

- **ターゲット・パッケージ構成**:
  - 最小 Deployment Target: `iOS 13.0+`
  - パッケージ管理: Swift Package Manager（SPM）および CocoaPods 両対応
- **クライアント構成・一括受信処理**:
  - `NSURLSessionDataDelegate` は使用せず、`URLSession` の `dataTask(with: request, completionHandler:)` を使用する。
  - CFNetwork 層で結合済みの `Data` を 1 回だけ Dart へ返却する。
- **ライフサイクル・並行管理**:
  - `requestId` と `URLSessionDataTask` をスレッドセーフに辞書管理し、`cancelRequest(requestId)` で `task.cancel()` を呼ぶ。
- **エラー分類**:
  - `URLError.cancelled` → `cancel`
  - `URLError.timedOut` → `connectionTimeout` / `receiveTimeout`
  - `.notConnectedToInternet`, `.cannotFindHost`, `.cannotConnectToHost`, `.networkConnectionLost` → `connectionError`
  - サーバー証明書関連 `URLError` → `badCertificate`
