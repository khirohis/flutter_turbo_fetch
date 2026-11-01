# Android ネイティブ層仕様（OkHttp3）

> 通信ブリッジ仕様・データモデル・共通エラーコード定義については [`native-layer.md`](./native-layer.md) を参照。

---

## 1. 概要 & 技術スタック

- **ターゲット**: `minSdk = 24`（Android 7.0+）
- **主要ライブラリ**: `com.squareup.okhttp3:okhttp:4.12.0`
- **主要プロトコル**: HTTP/2（ALPN ネゴシエーション、OkHttp デフォルト 16MiB ウィンドウ）、HTTP/1.1 フォールバック

---

## 2. クラス構成 & 責務分割（2分割構成）

Flutter Engine / Pigeon との結合層と、OkHttp 通信・リソース管理層を明確に 2 分割し、関心の分離とテスタビリティ（純粋な Kotlin/JVM 単体テスト）を確保する。

```text
┌────────────────────────────────────────────────────────┐
│  FlutterTurboFetchPlugin (Flutter インターフェース層)   │
│  - FlutterPlugin ライフサイクル (onAttached / onDetached)│
│  - Pigeon ホスト API 実装 (executeRequest / cancelRequest) │
│  - メインスレッド返却 (postToMain)                       │
└───────────────────────────▲────────────────────────────┘
                            │ 委譲 (OkHttp 依存なし)
┌───────────────────────────▼────────────────────────────┐
│  TurboHttpExecutor (OkHttp 通信・管理層)                │
│  - 共有クライアント管理 (OkHttpClient / Dispatcher / Pool) │
│  - 実行中リクエスト管理 (activeCalls: ConcurrentHashMap)  │
│  - リクエスト単位の派生クライアント・Request 構築        │
│  - Call.enqueue による非同期通信・一括受信              │
│  - OkHttp 例外の共通エラーコード分類 (classifyException) │
└────────────────────────────────────────────────────────┘
```

### ① `FlutterTurboFetchPlugin`（Flutter インターフェース層）

- **役割**: Flutter Engine / Pigeon とのブリッジおよびメインスレッドへのコールバック返却。OkHttp への直接依存を持たない。
- **保持する状態**:
  - `mainHandler: Handler?`
  - `executor: TurboHttpExecutor`
- **公開 / 内部インターフェース**:
  - `onAttachedToEngine` / `onDetachedFromEngine`
  - `executeRequest(request, callback)` / `cancelRequest(requestId)`
  - `postToMain(action)`

### ② `TurboHttpExecutor`（OkHttp 通信・管理層）

- **役割**: OkHttp 共有クライアント・実行中 `Call` のライフサイクル管理、および 1 リクエスト単位の通信実行・一括受信・エラー分類。Flutter 依存を一切持たない。
- **保持する状態**:
  - `dispatcher: Dispatcher`, `baseClient: OkHttpClient`, `clientLock: Any`
  - `activeCalls: ConcurrentHashMap<String, Call>`
- **公開 / 内部インターフェース**:
  - `ensureReady()`: Shutdown 状態からのクライアント復帰
  - `execute(request: TurboHttpRequest, onResult: (TurboHttpResponse) -> Unit)`: 非同期リクエスト実行
  - `cancel(requestId: String)`: 指定リクエストの即時キャンセル
  - `shutdown()`: 全実行中リクエストのキャンセルおよび Dispatcher / ConnectionPool の解放
  - `ensureClient(requestedPerHost: Int): OkHttpClient`
  - `classifyException(call: Call, e: IOException, isSendingBody: Boolean): String`
- **実行・キャンセル順序保証（競合防止）**:
  1. リクエスト構築時（URL やヘッダー不正等）にエラーが発生した場合は、`activeCalls` へ登録せず即座に `onResult(errorResponse)` を同期呼び出しして終了する。
  2. `Call` が正常に生成された場合は、**必ず `call.enqueue(...)` を呼び出す前に同期的に `activeCalls[request.requestId] = call` を登録**し、非同期完了や即時キャンセルより先行することを保証する。
  3. 通信の完了または失敗時は、`activeCalls.remove(request.requestId)` を行った上で必ず 1 回のみ `onResult(response)` を呼び出す。

---

## 3. クライアント管理 & 動的スケーリング（`TurboHttpExecutor`）

- **共有クライアント**:
  - 単一のベース `OkHttpClient` と `Dispatcher`、`ConnectionPool(5, 5, TimeUnit.MINUTES)` を共有し、HTTP/2 コネクションおよびリソースを効率的に再利用する。
- **同時リクエスト数制御**:
  - `Dispatcher.maxRequestsPerHost` はデフォルト `32`。
  - リクエスト指定値が全体上限 `DEFAULT_MAX_REQUESTS`（64）を超える場合は `maxRequests` も同値へ自動引き上げ、以降のリクエストで指定値が 64 以下に戻った場合は自動的にデフォルト（64）へ引き下げる。
- **リソース復帰（再アタッチ対応）**:
  - `Dispatcher.executorService.isShutdown` を検知した場合、自動的に Dispatcher およびベースクライアントを再生成して復帰する。

---

## 4. 通信実行・一括受信・エラー分類（`TurboHttpExecutor`）

- **リクエストごとのクライアント派生と不安全 TLS 対応**:
  - `client.newBuilder()` により、ベースの接続プールや Dispatcher を共有したまま、タイムアウト（connect / read / write）およびリダイレクト設定（`followRedirects` / `followSslRedirects`）をリクエスト単位で適用する。
  - `allowBadCertificates = true` 時は全信頼の `X509TrustManager` および全ホスト許可の `HostnameVerifier` を注入する。
- **リクエスト構築の安全性**:
  - 不正なヘッダー名/値（改行文字等）や無効な URL が渡された場合でも、未捕捉例外（クラッシュ）とせず、`try-catch` 内で安全に `connectionError` レスポンスを生成する。
- **ボディ送信フェーズ監視**:
  - リクエストボディが存在する場合は `TrackingRequestBody`（`SendPhaseTracker`）でラップし、`writeTo(sink)` 実行中フラグ（`isSendingBody`）を追跡する。
- **一括受信処理**:
  - `Call.enqueue(Callback)` のバックグラウンドスレッド上で実行し、`response.use { res -> ... }` スコープ内で確実にストリーム・ソケットを解放する。
- **`maxResponseBytes` の判定ロジック**:
  - `maxResponseBytes == null` 時:
    - `res.body?.bytes() ?: ByteArray(0)` で全フレームを一括結合する。
  - `maxResponseBytes` 指定時（`safeMaxBytes = maxResponseBytes.coerceAtLeast(0L)`）:
    1. `res.body?.contentLength()`（不明時は `-1L`）が `safeMaxBytes` を超える場合（`contentLength > safeMaxBytes`）は、ストリーム読み出し前に即時中断し `maxResponseBytesExceeded` を返却する。
    2. `safeMaxBytes < Long.MAX_VALUE` の場合、`BufferedSource.request(safeMaxBytes + 1L)` で先行バイト読み込みを実施。上限超過（`true`）を検知した時点で中断し `maxResponseBytesExceeded` を返却する。
    3. 上限内の場合、`source.readByteArray()` で全データを読み出す（`safeMaxBytes == Long.MAX_VALUE` 時もスキップされず確実に全バイトを取得する）。
- **エラー分類（OkHttp 例外 → 共通エラーコード）**:

| ネイティブ例外 / 状態 | 共通エラーコード | 備考 |
| :--- | :--- | :--- |
| `call.isCanceled()` | `cancel` | リクエストキャンセル |
| `SocketTimeoutException`（メッセージに `connect` を含む） | `connectionTimeout` | 接続タイムアウト |
| `SocketTimeoutException` / `InterruptedIOException`（`isSendingBody == true`） | `sendTimeout` | リクエストボディ送信中のタイムアウト |
| `SocketTimeoutException` / `InterruptedIOException`（上記以外） | `receiveTimeout` | レスポンス受信待機中のタイムアウト |
| `InterruptedIOException`（メッセージに `timeout` を含まない） | `cancel` | スレッド中断等によるキャンセル |
| `SSLException` / `CertificateException` | `badCertificate` | 証明書検証エラー |
| `UnknownHostException` / `ConnectException` / `NoRouteToHostException` / `SocketException` / `ProtocolException` / `UnknownServiceException` | `connectionError` | ネットワーク接続不良・プロトコル違反 |
| その他未分類の `IOException` / `Throwable` | `unknown` | 予期せぬ実行時例外・メモリ不足等 |

---

## 5. ライフサイクル & 並行管理

- **キャンセル処理**:
  - `cancelRequest(requestId)` 呼び出し時、`executor.cancel(requestId)`（`activeCalls.remove(requestId)?.cancel()`）を即座に実行する。
- **Engine デタッチ時のクリーンアップ**:
  - `onDetachedFromEngine` で `executor.shutdown()` を呼び出し、実行中の全 `Call` の一括キャンセル（`cancel()` & `activeCalls.clear()`）、および `dispatcher.executorService.shutdown()` と `baseClient.connectionPool.evictAll()` によるリソース解放を行う。
