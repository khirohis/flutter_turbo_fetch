# Android ネイティブ層仕様（OkHttp3 / Cronet）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. ビルド時エンジン切替仕様（Gradle）

- **切替プロパティ**: アプリ側の `android/gradle.properties`（または Gradle `-P` オプション）で `flutter_turbo_fetch.androidEngine` を指定する。
  - `okhttp`（未指定時のデフォルト）: `OkHttp3` 依存と `src/okhttp/kotlin`（およびテスト `src/testOkhttp/kotlin`）のみを組み込む。
  - `cronet`: Cronet API アーティファクトのみに依存し、`src/cronet/kotlin`（およびテスト `src/testCronet/kotlin`）のみを組み込む。
  - 上記以外の不正値が指定された場合は、暗黙フォールバックせず **`GradleException` でビルドを即座に失敗させる**。
- **未選択ライブラリの完全除外**: 選択されなかった側の依存関係および Kotlin 実装ソースはコンパイル対象から完全に除外する（R8 向けの `-dontwarn` による回避は行わない）。
- **共通インターフェース層（`src/main/kotlin`）**: エンジン非依存の `internal interface TurboHttpExecutor` を定義し、`FlutterTurboFetchPlugin` はこのインターフェースと各エンジン側が提供するデフォルト生成関数（`createDefaultTurboHttpExecutor()`）のみに依存する。`onAttachedToEngine` 時に `applicationContext` を `ensureReady(context)` へ渡して初期化する。

---

## 2. OkHttp3 固有の設計・実装仕様（デフォルト）

- **クライアント共有と派生クライアント再利用（`OkHttpClientManager`）**:
  - 単一のベース `OkHttpClient`（`Dispatcher` / `ConnectionPool`）を共有し、定常リクエスト時の取得で排他ロックによる競合を起こさない設計とする。
  - OkHttp 標準の `Dispatcher.maxRequestsPerHost`（`5`）による HTTP/2 多重化の頭打ちを防ぐため、内部固定設定として `Dispatcher.maxRequestsPerHost = 32` / `Dispatcher.maxRequests = 64` を維持する。
  - タイムアウト、リダイレクト追従（`followRedirects` / `followSslRedirects`）、および `allowBadCertificates` 時の全信頼 `X509TrustManager` + `HostnameVerifier` は、`sharedClient.newBuilder()` による軽量な派生クライアントで適用する。
  - 同一リクエスト設定に対する毎回のクライアント再構築を避けるため、設定の組み合わせごとに派生クライアントを有界キャッシュで再利用し、`shutdown()` およびベースクライアント再生成時にキャッシュを破棄する。
- **送信フェーズ追跡とレスポンスボディ読み出し（`OkHttpTurboHttpExecutor`）**:
  - リクエストボディ送信中は `RequestBody.writeTo(sink)` 内で `isSendingBody` フラグを立て、タイムアウト発生時に `sendTimeout` と `receiveTimeout` を識別する。
  - レスポンスボディの読み出しでは、不要な中間バッファリングによるピークメモリ増大を避けつつ、`maxResponseBytes` 上限を `Content-Length` と実データ量の 2 段階で検証し、超過検知時は全量をメモリ展開せず即座に中断して `maxResponseBytesExceeded` を返却する。

### 2.1 例外マッピング（OkHttp → 共通エラーコード）

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

---

## 3. Cronet 固有の設計・実装仕様

- **プロバイダ依存と `CronetEngine` 管理（`CronetClientManager`）**:
  - プラグインは Cronet API アーティファクトのみに依存し、プロバイダ実装（Google Play Services 版 / 同梱版 / 併用）の追加および Play Services 版利用時の `CronetProviderInstaller.installProvider(context)` 事前実行はアプリ側の責務とする。
  - プラグイン内部の `CronetEngine.Builder(applicationContext)` で単一の共有 `CronetEngine` を生成する（外部からの `CronetEngine` 注入フックは設けない）。
  - プロトコル設定は `enableHttp2(true)` / `enableQuic(false)` を既定とする（将来 HTTP/3 指定へ拡張可能な構成とする）。
  - **遅延シャットダウン**: `CronetEngine.shutdown()` はアクティブな `UrlRequest` 残存時に `IllegalStateException` を送出し、かつネットワークスレッド上からの呼び出しが禁止されている。そのため参照カウント付きの管理ラッパーで保持し、`shutdown()` 時は全アクティブ `UrlRequest.cancel()` 後に実行中カウントが `0` になったタイミングで非ネットワークスレッド（デーモンスレッド）から `engine.shutdown()` を実行する。再アタッチ・再リクエスト時は新しい `CronetEngine` を生成して復帰する。
  - プロバイダ未導入・`installProvider` 未実行等により `CronetEngine` の初期化に失敗した場合は、クラッシュさせず `errorCode = "unknown"` と英語の診断メッセージを返却する。
- **リクエスト実行・バッファリング・タイムアウト・制約（`CronetTurboHttpExecutor`）**:
  - **読み出しバッファ**: 1 リクエストにつき固定 **64KiB** の Direct `ByteBuffer` を確保・再利用（`clear()`）し、ネイティブメモリ上で一括結合した単一の `ByteArray` を Dart へ返却する。
  - **`maxResponseBytes` の 2 段階検証**: (1) `onResponseStarted` で `Content-Length` ヘッダ超過を検知した場合は読み出し前に即 `request.cancel()`、(2) `onReadCompleted` で累積受信バイト数が上限を超えた時点で即 `request.cancel()` し、いずれも `maxResponseBytesExceeded` を返却する。
  - **フェーズ別タイムアウト**: `UrlRequest` には組み込みのフェーズ別タイムアウトがないため、`ScheduledExecutorService` でフェーズ別タイマー（`connect` / `send` / `receive`）を監視し、発火時は `request.cancel()` して対応するエラーコードを返却する（`start()` で `connect` 開始、ボディ送信中〜完了まで `send` 管理、送信完了または `onResponseStarted` で `receive` 開始、`onReadCompleted` ごとに `receive` タイマーをアイドルリセット）。
  - **`allowBadCertificates = true` 非サポート**: Cronet は標準 API で動的な自己署名証明書バイパスを提供しないため、指定時は通信を開始せず即座に `errorCode = "unknown"` と英語の非サポートメッセージ（Android Network Security Config の利用案内を含む）を返却する。
  - **`followRedirects = false` の挙動**: `onRedirectReceived` で `UrlResponseInfo` から 3xx のステータスコード・ステータスメッセージ・ヘッダ（`Location` 含む）を採取し、空ボディ（`ByteArray(0)`）の通常レスポンスとして返却した上で内部キャンセルする（Cronet の API 制約により 3xx レスポンスボディは取得しない）。
  - **非空ボディ送信時の `Content-Type` 必須制約**: Cronet は `setUploadDataProvider()` 呼び出し時に `Content-Type` ヘッダを必須とする。プラグイン側で勝手なヘッダ補完は行わず、未指定時はリクエスト構築エラー（`connectionError`）として英語メッセージを返却する（空ボディまたは `null` ボディ時は `UploadDataProvider` を設定せず送信する）。

### 3.1 例外マッピング（Cronet → 共通エラーコード）

| ネイティブ例外 / 状態 | 共通エラーコード | 判定条件・備考 |
| :--- | :--- | :--- |
| フェーズ別タイマー発火済み | `connectionTimeout` / `sendTimeout` / `receiveTimeout` | **最優先判定**: 発火したフェーズに応じたタイムアウトコード |
| `onCanceled`（タイマー・上限超過・非追従リダイレクト以外） | `cancel` | `cancelRequest` / `shutdown` によるリクエストキャンセル |
| `NetworkException.ERROR_CONNECTION_TIMED_OUT` | `connectionTimeout` | TCP / TLS 接続確立時のタイムアウト |
| `NetworkException.ERROR_TIMED_OUT` | `connectionTimeout` / `sendTimeout` / `receiveTimeout` | 発生時の実行フェーズに応じて分類 |
| `NetworkException`（`cronetInternalErrorCode` が `-200`〜`-299` または SSL/TLS ハンドシェイク系エラー） | `badCertificate` | Chromium `net_error_list.h` 証明書・TLS 検証エラー |
| `NetworkException`（`ERROR_HOSTNAME_NOT_RESOLVED` / `ERROR_INTERNET_DISCONNECTED` / `ERROR_NETWORK_CHANGED` / `ERROR_CONNECTION_CLOSED` / `ERROR_CONNECTION_REFUSED` / `ERROR_CONNECTION_RESET` / `ERROR_ADDRESS_UNREACHABLE` / `ERROR_QUIC_PROTOCOL_FAILED` / `ERROR_OTHER` の非証明書系） | `connectionError` | ネットワーク接続不良・名前解決失敗・プロトコル異常 |
| リクエスト構築時例外（不正 URL / 不正ヘッダ / 非空ボディ時の `Content-Type` 未指定等） | `connectionError` | リクエストパラメータ不正 |
| `allowBadCertificates = true` 指定 / `CronetEngine` 初期化失敗 / その他未分類の `CronetException`・`Throwable` | `unknown` | 非サポート設定・プロバイダ未導入・予期せぬ実行時例外 |
