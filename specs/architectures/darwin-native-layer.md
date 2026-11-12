# iOS / macOS（Darwin）ネイティブ層仕様（URLSession）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. Darwin 固有の設計・実装仕様

- **iOS / macOS フレームワーク差分の吸収とライフサイクル（`FlutterTurboFetchPlugin`）**:
  - `#if os(iOS) import Flutter #elseif os(macOS) import FlutterMacOS #endif` により単一ソース（`sharedDarwinSource: true`）で両 OS に対応する。
  - `FlutterPluginRegistrar` からの `FlutterBinaryMessenger` 取得差分（iOS のメソッド `messenger()` vs macOS のプロパティ `messenger`）を内部ヘルパーで吸収する。
  - Engine デタッチ時（`detachFromEngine(for:)`）は Pigeon API 登録を解除（`api: nil`）し、`executor.shutdown()` を呼び出して実行中タスクのキャンセルとセッション破棄を一括実行する。
- **共有セッション切り替えと境界値ガード（`TurboSessionManager`）**:
  - `httpMaximumConnectionsPerHost`（デフォルト `32`）は、非正値（`<= 0`）が渡された場合でも `max(1, maxConnectionsPerHost)` により安全に `1` へクランプする。
  - 指定値が現在値と異なる場合は、既存セッションに `finishTasksAndInvalidate()` を呼び、新しい上限値を持つ `URLSession` へ安全に切り替える。
  - `shutdown()` 後に再リクエストがあった場合は、セッションを自動再生成して復帰する（再アタッチ耐性）。
  - セッション再生成時の `taskIdentifier` 衝突を防ぐため、`ObjectIdentifier(task)` をキーとしてタスク単位の `TaskContext` を `NSLock` 保護下で管理する。
  - `URLSessionTaskDelegate` を介し、`TaskContext` の設定に基づいてリクエスト単位のリダイレクト制御および証明書検証バイパス（`didReceive challenge`）を行う。
- **CFNetwork 一括受信・フェーズ監視・不変条件保証（`TurboHttpExecutor`）**:
  - `URLSessionDataDelegate`（細切れチャンク）を意図的に使わず、`session.dataTask(with:completionHandler:)` を使用することで、CFNetwork C/C++ 層でチャンク一括結合を完結させて Dart へ返却する。
  - タスク開始（`task.resume()`）前に同期的に `activeTasks` へ登録し、開始直後のキャンセル呼び出し（Race Condition）でも安全に中断されることを保証する。
  - 正常終了・エラー・キャンセルの全経路で `activeTasks` から確実に削除してリークゼロを保証し、コールバックを 1 リクエストにつき 1 回のみ呼び出す（Exactly-Once 保証）。
  - `URLSession` 標準の単一タイマーではフェーズを区別できないため、`TaskContext` 内のフェーズ別 `DispatchWorkItem` タイマー（connect / send / receive）と `didSendBodyData` による送信進捗追跡を組み合わせてタイムアウトを検知・分類する。
  - `maxResponseBytes` 指定時は、`HTTPURLResponse.expectedContentLength` によるヘッダ超過判定と、一括受信バッファ `data.count` による実サイズ超過判定の 2 段階で `maxResponseBytesExceeded` を検知・中断する。

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
