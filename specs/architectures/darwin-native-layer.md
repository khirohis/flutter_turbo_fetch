# iOS / macOS（Darwin）ネイティブ層仕様（URLSession）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. 技術スタック

- **ターゲット**: `iOS 13.0+` / `macOS 10.15+`
- **パッケージ構成**: `sharedDarwinSource: true` による `darwin/` 単一ソース共有（Swift Package Manager / CocoaPods 両対応）
- **主要基盤**: Swift, `Foundation.URLSession`（`dataTask(with:completionHandler:)` による CFNetwork 層一括受信、HTTP/2 ALPN）

---

## 2. Darwin 固有の設計・実装仕様

- **iOS / macOS フレームワーク差分の吸収（`FlutterTurboFetchPlugin`）**:
  - `#if os(iOS) import Flutter #elseif os(macOS) import FlutterMacOS #endif` により単一コードで両 OS に対応する。
  - `FlutterPluginRegistrar` からの `FlutterBinaryMessenger` 取得差分（iOS のメソッド `messenger()` vs macOS のプロパティ `messenger`）をヘルパーで吸収する。
- **共有セッション切り替えとタスクコンテキスト管理（`TurboSessionManager`）**:
  - `httpMaximumConnectionsPerHost`（デフォルト `32`）の指定値が現在値と異なる場合は、既存セッションに `finishTasksAndInvalidate()` を呼び、新しい上限値を持つ `URLSession` へ切り替える。
  - セッション再生成時に `taskIdentifier` が `1` から再採番されて衝突するのを防ぐため、`ObjectIdentifier(task)` をキーとしてタスク単位の `TaskContext` を `NSLock` 保護下で管理する。
  - `URLSessionTaskDelegate` を介し、`TaskContext` の設定に基づいてリクエスト単位のリダイレクト制御（`willPerformHTTPRedirection`）および `allowBadCertificates = true` 時の証明書許可（`didReceive challenge`）を行う。
- **フェーズ別タイムアウト監視（`TurboHttpExecutor`）**:
  - `URLSession` 標準の単一タイマーではタイムアウト発生フェーズを区別できないため、`TaskContext` 内のフェーズ別 `DispatchWorkItem` タイマー（connect / send / receive）と `didSendBodyData` デリゲートによる送信進捗追跡を組み合わせてタイムアウトを検知・分類する。

---

## 3. エラーマッピング（NSURLError / TaskContext → 共通エラーコード）

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
