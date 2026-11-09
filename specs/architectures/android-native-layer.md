# Android ネイティブ層仕様（OkHttp3）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. 技術スタック

- **ターゲット**: `minSdk = 24`（Android 7.0+）
- **主要基盤**: Kotlin, `com.squareup.okhttp3:okhttp:4.12.0`（HTTP/2 ALPN、デフォルト 16MiB ウィンドウ、HTTP/1.1 フォールバック）

---

## 2. Android 固有の設計・実装仕様

- **クライアント共有とリクエスト単位派生（`TurboClientManager`）**:
  - 単一のベース `OkHttpClient`（`Dispatcher` / `ConnectionPool`）を共有する。
  - `Dispatcher.maxRequestsPerHost`（デフォルト `32`）が全体上限 `maxRequests`（デフォルト `64`）を超える場合は `maxRequests` も同値へ自動引き上げし、64 以下に戻った際はデフォルト（`64`）へ戻す。
  - リクエストごとのタイムアウト（connect / read / write）、リダイレクト追従（`followRedirects` / `followSslRedirects`）、および `allowBadCertificates = true` 時の全信頼 `X509TrustManager` + `HostnameVerifier` は、`sharedClient.newBuilder()` による軽量な派生クライアントで適用する。
- **送信フェーズ追跡とストリーム先読み上限判定（`TurboHttpExecutor`）**:
  - リクエストボディが存在する場合は `RequestBody.writeTo(sink)` 実行中フラグ（`isSendingBody`）を追跡し、タイムアウト発生時に `sendTimeout` と `receiveTimeout` を識別する。
  - `maxResponseBytes` 指定時は、`Content-Length` 事前チェックに加え、`BufferedSource.request(safeMaxBytes + 1L)` で先行読み込みを行い、全バイト配列をメモリ展開する前に上限超過を検知して即時中断する。

---

## 3. 例外マッピング（OkHttp → 共通エラーコード）

| ネイティブ例外 / 状態 | 共通エラーコード | 判定条件・備考 |
| :--- | :--- | :--- |
| `call.isCanceled()` | `cancel` | リクエストキャンセル |
| `SocketTimeoutException` | `connectionTimeout` | 例外メッセージに `connect` を含む場合 |
| `SocketTimeoutException` / `InterruptedIOException` | `sendTimeout` | `isSendingBody == true`（ボディ送信中）の場合 |
| `SocketTimeoutException` / `InterruptedIOException` | `receiveTimeout` | 上記以外のタイムアウト |
| `InterruptedIOException` | `cancel` | メッセージに `timeout` を含まないスレッド中断 |
| `SSLException` / `CertificateException` | `badCertificate` | TLS / 証明書検証エラー |
| `UnknownHostException` / `ConnectException` / `NoRouteToHostException` / `SocketException` / `ProtocolException` / `UnknownServiceException` | `connectionError` | ネットワーク接続不良・プロトコル違反・不正 URL / ヘッダ |
| その他未分類の `IOException` / `Throwable` | `unknown` | 予期せぬ実行時例外 |
