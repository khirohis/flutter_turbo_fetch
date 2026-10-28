# ネイティブ層（Android / iOS）および通信ブリッジ仕様

> 通信方式（MethodChannel/Pigeon vs FFI）の比較背景については [`docs/references/design-rationale.md`](../references/design-rationale.md) を参照。

---

## 1. Dart ⇄ ネイティブ間の通信ブリッジ仕様（Pigeon）

通信ブリッジには **Pigeon（`StandardMessageCodec` ベースの型安全コード生成、`@HostApi` + `@async`）** を採用し、完成したレスポンスボディ `Uint8List`（Android `byte[]` / iOS `FlutterStandardTypedData`）を 1 リクエストにつき 1 往復でやり取りする。

### 通信データモデル

- **リクエスト情報**:
  - `requestId`: リクエスト識別子（キャンセル紐付け用）
  - `url`: リクエスト URL
  - `method`: HTTP メソッド（GET / POST / PUT / DELETE / PATCH / HEAD 等）
  - `headers`: リクエストヘッダ
  - `body`: リクエストボディ（nullable `Uint8List`）
  - `connectTimeoutMs` / `receiveTimeoutMs` / `sendTimeoutMs`: タイムアウト設定（ミリ秒）
  - `followRedirects`: リダイレクト追従設定（真偽値。`OkHttp` / `URLSession` ともにリダイレクト上限回数の標準設定プロパティを持たないため、`maxRedirects` による追加の回数制御は行わない）
  - `maxRequestsPerHost`: 1 ホストあたりの最大同時リクエスト数（デフォルト `32`）
  - `allowBadCertificates`: 自己署名証明書等の許可設定（ローカル検証用、デフォルト `false`）
  - `maxResponseBytes`: レスポンスボディの最大許容バイト数（nullable、未指定時は無制限。超過時はデータを切り詰めて正常返却することはせず、上限超過を検知した時点で処理を中断してエラーコード `maxResponseBytesExceeded` を返す）
- **レスポンス情報**:
  - `statusCode`: HTTP ステータスコード
  - `statusMessage`: ステータスメッセージ（任意）
  - `headers`: レスポンスヘッダ（マルチバリュー対応）
  - `body`: 一括結合済みレスポンスボディ（`Uint8List`）
  - `errorCode` / `errorMessage`: エラー発生時の共通エラーコードと詳細メッセージ
- **キャンセル操作**:
  - `cancelRequest(requestId)`: 実行中のネイティブリクエストを即座に中断する

---

## 2. Android ネイティブ層仕様（`OkHttp3`）

- **ターゲット・依存バージョン**: `minSdk = 24`（Android 7.0+）、`com.squareup.okhttp3:okhttp:4.12.0`
- **クライアント構成**:
  - 共有の `OkHttpClient` インスタンス（`ConnectionPool`、HTTP/2 初期 16MiB ウィンドウ）を保持する。
  - `Dispatcher.maxRequestsPerHost` はデフォルト `32`。リクエスト指定値が全体上限 `DEFAULT_MAX_REQUESTS`（64）を超える場合は `maxRequests` も同値へ自動引き上げ、以降のリクエストで指定値が 64 以下に戻った場合は自動的にデフォルト（64）へ引き下げる。
  - リクエスト単位のタイムアウト・リダイレクト・`allowBadCertificates` は `client.newBuilder()` で適用する。
  - `allowBadCertificates = true` 時は全信頼の `X509TrustManager` および全ホスト許可の `HostnameVerifier` を注入する。
- **一括受信処理**:
  - `Call.enqueue(Callback)` のバックグラウンドスレッド上で、`maxResponseBytes` 未指定時は `response.body.bytes()`、指定時は `contentLength()` 事前判定＋`Okio` の `BufferedSource.request(maxResponseBytes + 1)` による上限判定付き `readByteArray()` を実行し、`Handler(Looper.getMainLooper())` 経由で Dart へ 1 回で返却する。
- **ライフサイクル・並行管理**:
  - `ConcurrentHashMap<String, Call>` で実行中リクエストを管理し、`cancelRequest(requestId)` で `call.cancel()` を呼ぶ。
  - `FlutterPlugin.onDetachedFromEngine` 時に未完了リクエストの一括キャンセルおよび `dispatcher.executorService.shutdown()` / `connectionPool.evictAll()` を行う。
- **エラー分類（ネイティブ例外 → 共通エラーコード `errorCode`）**:
  - リクエストボディ送信フェーズを `TrackingRequestBody`（`SendPhaseTracker`）で監視し、ボディ書き込み中のタイムアウトを `sendTimeout`、接続段階を `connectionTimeout`、それ以降を `receiveTimeout` と正確に区別する。
  - `InterruptedIOException` / `call.isCanceled()` → `cancel`
  - `SocketTimeoutException` → `connectionTimeout` / `sendTimeout` / `receiveTimeout`
  - `UnknownHostException` / `ConnectException` / `SocketException` / `ProtocolException` 等 → `connectionError`
  - `SSLException` / `CertificateException` → `badCertificate`
  - `maxResponseBytes` 超過 → `maxResponseBytesExceeded`
  - その他 `IOException` → `connectionError` / `unknown`

---

## 3. iOS ネイティブ層仕様（`URLSession`）

- **ターゲット・パッケージ構成**: `iOS 13.0+`、Swift Package Manager（SPM）および CocoaPods 両対応
- **クライアント構成・一括受信処理**:
  - `NSURLSessionDataDelegate` は使用せず、`URLSession` の `dataTask(with: request, completionHandler:)` を使用する。
  - CFNetwork 層で結合済みの `Data` を 1 回だけ Dart へ返却する（`maxResponseBytes` 指定時は `completionHandler` 内で `response.expectedContentLength` および `data.count` の上限超過を判定し、超過時は処理を中断して `maxResponseBytesExceeded` を返す）。
  - `allowBadCertificates = true` 時は `URLSessionDelegate.urlSession(_:didReceive:completionHandler:)` でサーバー信頼認証（Server Trust）を無条件許可する。
- **ライフサイクル・並行管理**:
  - `requestId` と `URLSessionDataTask` を `NSLock` で辞書管理し、`cancelRequest(requestId)` で `task.cancel()` を呼ぶ。
  - `FlutterPlugin.detachFromEngine` 時に未完了タスクの一括キャンセルおよび共有 `URLSession` の `invalidateAndCancel()` を行う。
- **エラー分類（ネイティブエラー → 共通エラーコード `errorCode`）**:
  - `TurboSessionTaskDelegate`（`URLSessionTaskDelegate`）で `didSendBodyData` などの進捗フェーズを監視し、`URLError.timedOut` 発生時のフェーズ（接続前・送信中・受信待機中）に応じて `connectionTimeout` / `sendTimeout` / `receiveTimeout` を精密に分類する。
  - `URLError.cancelled` → `cancel`
  - `.notConnectedToInternet`, `.cannotFindHost`, `.cannotConnectToHost`, `.networkConnectionLost` → `connectionError`
  - サーバー証明書関連 `URLError`（`.serverCertificateHasBadDate`, `.serverCertificateUntrusted` 等） → `badCertificate`
  - `maxResponseBytes` 超過 → `maxResponseBytesExceeded`
  - その他エラー → `unknown`
