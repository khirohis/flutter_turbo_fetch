# iOS / macOS（Darwin）ネイティブ層仕様（URLSession）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. Darwin 固有の設計・実装仕様

- **iOS / macOS フレームワーク差分の吸収（`FlutterTurboFetchPlugin`）**:
  - `#if os(iOS) import Flutter #elseif os(macOS) import FlutterMacOS #endif` により単一ソースで両 OS に対応する。
  - `FlutterPluginRegistrar` からの `FlutterBinaryMessenger` 取得差分（iOS のメソッド `messenger()` vs macOS のプロパティ `messenger`）を共通ヘルパーで吸収する。
- **共有セッション切り替えとタスクコンテキスト管理（`TurboSessionManager`）**:
  - `httpMaximumConnectionsPerHost`（デフォルト `32`）の指定値が現在値と異なる場合は、既存セッションに `finishTasksAndInvalidate()` を呼び、新しい上限値を持つ `URLSession` へ切り替える。
  - セッション再生成時の `taskIdentifier` 衝突を防ぐため、`ObjectIdentifier(task)` をキーとしてタスク単位の `TaskContext` を `NSLock` 保護下で管理する。
  - `URLSessionTaskDelegate` を介し、`TaskContext` の設定に基づいてリクエスト単位のリダイレクト制御および証明書検証バイパス（`didReceive challenge`）を行う。
- **フェーズ別タイムアウト監視（`TurboHttpExecutor`）**:
  - `URLSession` 標準の単一タイマーではフェーズを区別できないため、`TaskContext` 内のフェーズ別 `DispatchWorkItem` タイマー（connect / send / receive）と `didSendBodyData` による送信進捗追跡を組み合わせてタイムアウトを検知・分類する。

---

## 2. エラーマッピング（NSURLError / TaskContext → 共通エラーコード）

| ネイティブエラー / 状態 | 共通エラーコード | 判定条件・備考 |
| :--- | :--- | :--- |
| `TaskContext.timedOutErrorCode` 設定済み | `connectionTimeout` / `sendTimeout` / `receiveTimeout` | フェーズ別タイマー発火によるタイムアウト中断 |
| `NSURLErrorCancelled` | `cancel` | リクエストキャンセル |
| `NSURLErrorTimedOut` | `connectionTimeout` | 送受信バイト 0 かつ `connectTimeoutMs` 指定あり |
| `NSURLErrorTimedOut` | `sendTimeout` | ボディ未送信完了かつ `sendTimeoutMs` 指定あり |
| `NSURLErrorTimedOut` | `receiveTimeout` | 上記以外の OS タイマーによるタイムアウト |
| `NSURLErrorServerCertificate*` / `ClientCertificate*` / `SecureConnectionFailed` | `badCertificate` | サーバー/クライアント証明書検証・TLS 接続エラー |
| `NSURLErrorNotConnectedToInternet` / `CannotFindHost` / `CannotConnectToHost` / `NetworkConnectionLost` / `DNSLookupFailed` / `HTTPTooManyRedirects` / `ResourceUnavailable` / `RedirectToNonExistentLocation` / `InternationalRoamingOff` / `CallIsActive` / `DataNotAllowed` | `connectionError` | ネットワーク接続不良・名前解決失敗・リダイレクト異常・不正 URL / ヘッダ |
| その他未分類のエラー | `unknown` | 予期せぬ実行時エラー |
