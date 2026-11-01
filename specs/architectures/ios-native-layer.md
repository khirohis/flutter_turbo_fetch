# iOS ネイティブ層仕様（URLSession）

> 通信ブリッジ仕様・データモデル・共通エラーコード定義については [`native-layer.md`](./native-layer.md) を参照。

---

## 1. 概要 & 技術スタック

- **ターゲット**: `iOS 13.0+`
- **パッケージマネージャー**: Swift Package Manager（SPM）および CocoaPods 両対応
- **主要基盤**: `Foundation.URLSession`（CFNetwork completionHandler による OS レベル一括受信）
- **主要プロトコル**: HTTP/2（ALPN ネゴシエーション）、HTTP/1.1 フォールバック

---

## 2. クライアント構成 & 一括受信処理

- **共有セッション管理 & 同時リクエスト数制御（`ensureSession`）**:
  - `URLSessionConfiguration.default` の `httpMaximumConnectionsPerHost`（デフォルト `32`）を設定した共有 `URLSession` を保持する。
  - リクエスト指定の `maxRequestsPerHost` が現在値と異なる場合は、既存セッションに対して `finishTasksAndInvalidate()` を呼び出し、新しい上限値を持つ `URLSession` を生成して切り替える。
- **一括受信パイプライン**:
  - `URLSessionDataDelegate` による細切れのチャンク受信は一切実装せず、`URLSession.dataTask(with: request, completionHandler:)` を採用する。
  - OS の CFNetwork 層（C/C++）で受信完了までバッファリングされた完成済みの `Data` を、1 リクエストにつき 1 回のみ `FlutterStandardTypedData`（`Uint8List`）として Dart へ返却する。
- **`maxResponseBytes` の判定ロジック**:
  - `maxResponseBytes` 指定時（`safeMaxBytes = max(0, maxResponseBytes)`）、`completionHandler` 内で以下の 2 段階で超過を検証する:
    1. `HTTPURLResponse.expectedContentLength`（不明時は `-1`）が正の値かつ `safeMaxBytes` を超えている場合（`expectedLength > 0 && expectedLength > safeMaxBytes`）。
    2. 受信完了した実データのサイズ `Int64(data?.count ?? 0)` が `safeMaxBytes` を超えている場合（`actualBytes > safeMaxBytes`）。
  - 超過を検知した時点で処理を即座に中断し、エラーコード `maxResponseBytesExceeded` を返却する。
- **リクエスト単位の制御（`TurboSessionTaskDelegate` + `TaskContext`）**:
  - 共有 `URLSession` 上でリクエスト単位のポリシーを適用するため、`taskIdentifier` をキーとして `TaskContext` を `NSLock` 保護下で管理する。
  - **タイムアウト制御**: `URLRequest.timeoutInterval` に各タイムアウト指定値の最大値を設定するほか、`TaskContext` 内で `DispatchWorkItem` タイマー（`connectTimer` / `sendTimer` / `receiveTimer`）を起動し、接続確立前・ボディ送信中・レスポンス待機中の各フェーズでタイムアウトを検知して `task.cancel()` する。
  - **リダイレクト制御**: `URLSessionTaskDelegate.urlSession(_:task:willPerformHTTPRedirection:newRequest:completionHandler:)` にて、リクエストごとの `followRedirects` 設定に従い追従可否を制御する。
  - **ローカル検証用 不安全証明書対応**: `URLSessionTaskDelegate.urlSession(_:task:didReceive:completionHandler:)` にて、リクエストの `allowBadCertificates = true` かつ `NSURLAuthenticationMethodServerTrust` の場合に `URLCredential(trust: serverTrust)` で許可する。

---

## 3. ライフサイクル & 並行管理

- **リクエストの追跡とキャンセル**:
  - `requestId` と `URLSessionDataTask` のマッピング（`activeTasks`）を `NSLock` 保護下で管理する。
  - `cancelRequest(requestId)` 呼び出し時、`activeTasks` および `TurboSessionTaskDelegate` から該当タスクのコンテキスト・タイマーを解除し、`task.cancel()` を即座に実行する。
- **Engine デタッチ時のクリーンアップ**:
  - `FlutterPlugin.detachFromEngine` 時に全タイマーの解除、未完了タスクの一括キャンセル、および共有 `URLSession` の `invalidateAndCancel()` を実行してリソースを完全に解放する。

---

## 4. エラー分類（NSURLError / TaskContext → 共通エラーコード）

`TaskContext` のフェーズ別タイマー発火状態（`timedOutErrorCode`）および `TurboSessionTaskDelegate.urlSession(_:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:)` の進捗状態に基づき、エラーを精密に分類する。

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
