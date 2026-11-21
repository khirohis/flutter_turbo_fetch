# iOS / macOS（Darwin）ネイティブ層仕様（URLSession）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. Darwin 固有の設計・実装仕様

- **iOS / macOS フレームワーク差分の吸収とライフサイクル（`FlutterTurboFetchPlugin`）**:
  - `#if os(iOS) import Flutter #elseif os(macOS) import FlutterMacOS #endif` により単一ソース（`sharedDarwinSource: true`）で両 OS に対応する。
  - `FlutterPluginRegistrar` からの `FlutterBinaryMessenger` 取得差分（iOS のメソッド `messenger()` vs macOS のプロパティ `messenger`）を内部ヘルパーで吸収する。
  - `detachFromEngine(for:)` で Pigeon API 登録解除（`api: nil`）とシャットダウンを実行する。
- **共有セッション管理（`TurboSessionManager`）**:
  - OS 標準設定（`URLSessionConfiguration.default`）の単一共有 `URLSession` を保持する（HTTP/1.1 の接続数制限および HTTP/2 多重化は OS 標準制御に委ねる）。
  - セッション再生成時の `taskIdentifier` 衝突を防ぐため、`ObjectIdentifier(task)` をキーとしてタスク単位の `TaskContext` を `NSLock` 保護下で管理する。
  - `URLSessionTaskDelegate` を介し、`TaskContext` の設定に基づいてリクエスト単位のリダイレクト制御および証明書検証バイパス（`didReceive challenge`）を行う。
- **CFNetwork 一括受信・フェーズ監視（`TurboHttpExecutor`）**:
  - `URLSessionDataDelegate`（細切れチャンク）を意図的に使わず、`session.dataTask(with:completionHandler:)` を使用することで、CFNetwork C/C++ 層でチャンク一括結合を完結させて Dart へ返却する。
  - `URLSession` 標準の単一タイマーではフェーズを区別できないため、`TaskContext` 内のフェーズ別 `DispatchWorkItem` タイマー（connect / send / receive）と `didSendBodyData` による送信進捗追跡を組み合わせてタイムアウトを検知・分類する。
  - `maxResponseBytes` 指定時は、`completionHandler` 内で `HTTPURLResponse.expectedContentLength` と一括受信データ `data.count` の 2 段階検証を行い、超過時は `maxResponseBytesExceeded` を返却する（`URLSessionDataDelegate` による途中中断は行わない）。

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
