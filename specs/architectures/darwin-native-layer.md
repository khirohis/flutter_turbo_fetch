# iOS / macOS（Darwin）ネイティブ層仕様（URLSession）

> 通信ブリッジ仕様・データモデル・共通エラーコード定義については [`native-layer.md`](./native-layer.md) を参照。

---

## 1. 概要 & 技術スタック

- **ターゲット**: `iOS 13.0+` / `macOS 10.15+`
- **ソース構成**: Flutter `sharedDarwinSource: true` による `darwin/` ディレクトリ単一ソース共有
- **パッケージマネージャー**: Swift Package Manager（SPM）および CocoaPods 両対応
- **主要基盤**: `Foundation.URLSession`（CFNetwork completionHandler による OS レベル一括受信）
- **主要プロトコル**: HTTP/2（ALPN ネゴシエーション）、HTTP/1.1 フォールバック

---

## 2. クラス構成 & 責務分割（3層構成）

Flutter Engine / Pigeon との結合層、1 リクエストの通信実行・実行中 `URLSessionDataTask` 管理層、および共有 `URLSession`・接続管理層を明確に 3 分割し、単一責任の原則（SRP）とテスタビリティ（`XCTest` による単体テスト）を確保する。

```text
┌────────────────────────────────────────────────────────┐
│  FlutterTurboFetchPlugin (Flutter インターフェース層)   │
│  - FlutterPlugin ライフサイクル (register / detach)     │
│  - Pigeon ホスト API 実装 (executeRequest / cancelRequest) │
│  - メインスレッド返却 (postToMain)                       │
└───────────────────────────▲────────────────────────────┘
                            │ 委譲 (URLSession 依存なし)
┌───────────────────────────▼────────────────────────────┐
│  TurboHttpExecutor (URLSession 通信実行 & Task 管理層)  │
│  - 実行中リクエスト管理 (activeTasks + NSLock)          │
│  - URLRequest の安全な構築 (URL / ヘッダー / ボディ)    │
│  - dataTask(completionHandler:) による非同期一括受信    │
│  - NSURLError / TaskContext の共通エラーコード分類      │
└───────────────────────────▲────────────────────────────┘
                            │ セッション取得 (acquireSession)
┌───────────────────────────▼────────────────────────────┐
│  TurboSessionManager (URLSession セッション・接続管理層) │
│  - 共有 URLSession 管理 & 同時接続数の動的スケーリング   │
│  - Shutdown 後の自動再生成・復帰                        │
│  - TurboSessionTaskDelegate によるタスク単位制御         │
│    (Redirect / Insecure TLS / フェーズ別タイマー)       │
└────────────────────────────────────────────────────────┘
```

### ① `FlutterTurboFetchPlugin`（Flutter インターフェース層）

- **役割**: Flutter Engine（iOS `Flutter` / macOS `FlutterMacOS`）および Pigeon とのブリッジ、メインスレッドへのコールバック返却。`URLSession` への直接依存を持たない。
- **プラットフォーム差分の吸収**:
  - `#if os(iOS) import Flutter #elseif os(macOS) import FlutterMacOS #endif` により両 OS のプラグインフレームワークを単一ファイルで切り替える。
  - `FlutterPluginRegistrar` からの `FlutterBinaryMessenger` 取得差分（iOS: `registrar.messenger()` メソッド / macOS: `registrar.messenger` プロパティ）をヘルパーメソッド `messenger(from:)` で吸収する。
- **保持する状態**:
  - `executor`: `TurboHttpExecutor`
- **公開 / 内部インターフェース**:
  - `register(with:)` / `detachFromEngine(for:)`
  - `executeRequest(request:completion:)` / `cancelRequest(requestId:)`
  - `postToMain(_ action: @escaping () -> Void)`

### ② `TurboHttpExecutor`（URLSession 通信実行 & Task 管理層）

- **役割**: 実行中 `URLSessionDataTask` のライフサイクル管理、および 1 リクエスト単位の通信実行・一括受信・エラー分類。Flutter 依存およびセッション構築の詳細を持たない。
- **保持する状態**:
  - `sessionManager`: `TurboSessionManager`
  - `activeTasks`: `[String: URLSessionDataTask]`（`NSLock` で排他保護）
- **公開 / 内部インターフェース**:
  - `execute(request: TurboHttpRequest, onResult: @escaping (TurboHttpResponse) -> Void)`: 非同期リクエスト実行
  - `cancel(requestId: String)`: 指定リクエストの即時キャンセル
  - `shutdown()`: 全実行中タスクのキャンセルおよび `sessionManager.shutdown()` の実行
  - `classifyError(_ error: Error, task: URLSessionTask?, context: TaskContext?, request: TurboHttpRequest) -> (String, String)`
- **実行・キャンセル順序保証（競合防止）**:
  1. リクエスト構築時（URL やヘッダー不正等）にエラーが発生した場合は、`activeTasks` へ登録せず即座に `onResult(errorResponse)` を同期呼び出しして終了する。
  2. `URLSessionDataTask` が正常に生成された場合は、**必ず `task.resume()` を呼び出す前に同期的に `activeTasks[request.requestId] = task` および `sessionManager.registerTask(...)` を登録**し、非同期完了や即時キャンセルより先行することを保証する。
  3. 通信の完了または失敗時は、`activeTasks` および `sessionManager` から該当タスクを確実に解除した上で、必ず 1 回のみ `onResult(response)` を呼び出す。

### ③ `TurboSessionManager`（URLSession セッション・接続管理層）

- **役割**: 共有 `URLSession` の排他制御・同時接続数の動的スケーリング・Shutdown 復帰、および `TurboSessionTaskDelegate` を介したリクエスト単位のポリシー制御（Redirect / Insecure TLS / フェーズ別タイマー）。
- **保持する状態**:
  - `sharedSession`: 共有 `URLSession?`（`NSLock` で排他保護）
  - `currentMaxConnectionsPerHost`: `Int`
  - `sessionDelegate`: `TurboSessionTaskDelegate`
- **公開 / 内部インターフェース**:
  - `acquireSession(maxConnectionsPerHost: Int) -> URLSession`: 同時接続数設定が適用された共有 `URLSession` の取得（未初期化・Shutdown 後の場合は再生成）
  - `registerTask(task:followRedirects:allowBadCertificates:hasRequestBody:connectTimeoutMs:sendTimeoutMs:receiveTimeoutMs:)`: タスクコンテキストおよびフェーズ別タイマーの登録
  - `removeTask(for task: URLSessionTask) -> TaskContext?`: タスクコンテキストおよびタイマーの解除・取得
  - `shutdown()`: 全タスクコンテキストの解除および共有 `URLSession` の `invalidateAndCancel()` 実行

---

## 3. セッション管理 & 同時リクエスト数制御（`TurboSessionManager`）

- **共有セッション**:
  - `URLSessionConfiguration.default` をベースとした共有 `URLSession` を保持し、HTTP/2 コネクションおよび TLS セッションを効率的に再利用する。
- **同時リクエスト数制御**:
  - `httpMaximumConnectionsPerHost`（デフォルト `32`）を設定する。
  - リクエスト指定の `maxRequestsPerHost`（最小 `1` にクランプ）が現在値と異なる場合は、既存セッションに対して `finishTasksAndInvalidate()` を呼び出し、新しい上限値を持つ `URLSession` を生成して切り替える。
- **リソース復帰（再アタッチ対応）**:
  - `shutdown()` 後に再度リクエストが実行された場合は、自動的に共有 `URLSession` を再生成して復帰する。
- **リクエスト単位の設定適用（`TurboSessionTaskDelegate` + `TaskContext`）**:
  - `URLSession` はリダイレクト・TLS 評価・送信進捗通知をセッションデリゲート（`URLSessionTaskDelegate`）経由で処理する設計のため、`ObjectIdentifier(task)` をキーとして `TaskContext` を `NSLock` 保護下で管理する（`taskIdentifier` はセッション再生成時に `1` から再採番されるため、`ObjectIdentifier(task)` を用いることで新旧セッション並存時のキー衝突を防止する）。
  - **リダイレクト制御**: `urlSession(_:task:willPerformHTTPRedirection:newRequest:completionHandler:)` にて、リクエストごとの `followRedirects` 設定に従い追従可否を制御する。
  - **不安全 TLS 対応**: `urlSession(_:task:didReceive:completionHandler:)` にて、リクエストの `allowBadCertificates = true` かつ `NSURLAuthenticationMethodServerTrust` の場合に `URLCredential(trust: serverTrust)` で許可する。

---

## 4. 通信実行・一括受信・エラー分類（`TurboHttpExecutor`）

- **リクエスト構築の安全性**:
  - 不正なヘッダー名/値（空キー、改行文字 `\r` / `\n` 等）や無効な URL（`http` / `https` 以外のスキーム、ホスト名欠落等）が渡された場合でも、未捕捉例外や不正通信とせず、安全に `connectionError` レスポンスを生成する。
- **フェーズ追跡とタイムアウト監視**:
  - `URLRequest.timeoutInterval` に各タイムアウト指定値の最大値を設定するほか、`TaskContext` 内で `DispatchWorkItem` タイマー（`connectTimer` / `sendTimer` / `receiveTimer`）と `urlSession(_:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:)` の進捗追跡を組み合わせ、接続確立前・ボディ送信中・レスポンス待機中の各フェーズでタイムアウトを検知して `task.cancel()` する。
- **一括受信処理**:
  - `URLSessionDataDelegate` による細切れのチャンク受信は一切実装せず、`URLSession.dataTask(with: request, completionHandler:)` を採用する。
  - OS の CFNetwork 層（C/C++）で受信完了までバッファリングされた完成済みの `Data` を、1 リクエストにつき 1 回のみ `FlutterStandardTypedData`（`Uint8List`）として返却する。
- **`maxResponseBytes` の判定ロジック**:
  - `maxResponseBytes` 指定時（`safeMaxBytes = max(0, maxResponseBytes)`）は、`completionHandler` 内で以下の 2 段階で超過を検証する:
    1. `HTTPURLResponse.expectedContentLength`（不明時は `-1`）が正の値かつ `safeMaxBytes` を超えている場合（`expectedLength > 0 && expectedLength > safeMaxBytes`）は、即座に中断し `maxResponseBytesExceeded` を返却する。
    2. 受信完了した実データのサイズ `Int64(data?.count ?? 0)` が `safeMaxBytes` を超えている場合（`actualBytes > safeMaxBytes`）は、即座に中断し `maxResponseBytesExceeded` を返却する。
- **エラー分類（NSURLError / TaskContext → 共通エラーコード）**:

| ネイティブエラー / 状態 | 共通エラーコード | 備考 |
| :--- | :--- | :--- |
| `TaskContext.timedOutErrorCode` 設定済み | `connectionTimeout` / `sendTimeout` / `receiveTimeout` | フェーズ別タイマー発火によるタイムアウト中断 |
| `NSURLErrorCancelled` | `cancel` | リクエストキャンセル |
| `NSURLErrorTimedOut`（送受信バイト 0 かつ `connectTimeoutMs` 指定あり） | `connectionTimeout` | OS 標準タイマーでの接続タイムアウト |
| `NSURLErrorTimedOut`（ボディ未送信完了かつ `sendTimeoutMs` 指定あり） | `sendTimeout` | OS 標準タイマーでの送信タイムアウト |
| `NSURLErrorTimedOut`（上記以外） | `receiveTimeout` | OS 標準タイマーでの受信タイムアウト |
| `NSURLErrorServerCertificateHasBadDate` / `Untrusted` / `HasUnknownRoot` / `NotYetValid` / `ClientCertificateRejected` / `ClientCertificateRequired` / `SecureConnectionFailed` | `badCertificate` | サーバー/クライアント証明書検証・TLS 接続エラー |
| `NSURLErrorNotConnectedToInternet` / `CannotFindHost` / `CannotConnectToHost` / `NetworkConnectionLost` / `DNSLookupFailed` / `HTTPTooManyRedirects` / `ResourceUnavailable` / `RedirectToNonExistentLocation` / `InternationalRoamingOff` / `CallIsActive` / `DataNotAllowed` | `connectionError` | ネットワーク接続不良・名前解決失敗・リダイレクト異常等 |
| その他未分類のエラー | `unknown` | 予期せぬ実行時エラー |

---

## 5. ライフサイクル & 並行管理

- **キャンセル処理**:
  - `cancelRequest(requestId)` 呼び出し時、`executor.cancel(requestId)` を実行し、`activeTasks` および `TurboSessionManager` から該当タスクのコンテキスト・タイマーを解除した上で `task.cancel()` を即座に実行する。
- **Engine デタッチ時のクリーンアップ**:
  - `FlutterPlugin.detachFromEngine` で `executor.shutdown()` を呼び出し、実行中の全タスクの一括キャンセル（`task.cancel()` & `activeTasks.removeAll()`）、および `sessionManager.shutdown()`（全タイマー解除・共有 `URLSession` の `invalidateAndCancel()`）によるリソース解放を行う。
