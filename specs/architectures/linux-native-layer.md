# Linux ネイティブ層仕様（libcurl）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. Linux 固有の設計・実装仕様

- **GTK メインループ連携とライフサイクル（`FlutterTurboFetchPlugin`）**:
  - Pigeon GObject 生成コード（`linux/messages.g.h` / `linux/messages.g.cc`）を使用し、`fl_plugin_registrar_get_messenger` 経由で Host API メソッドハンドラーを登録する。
  - イベントループスレッドでのリクエスト完了時の Pigeon コールバックは、`g_idle_add` を用いて GLib/GTK メインループからメインスレッドへディスパッチし、プラグイン破棄時は GObject `dispose` で `TurboHttpExecutor::Shutdown()` → `TurboSessionManager::Shutdown()` の順にシャットダウンを実行する。
- **共有接続・セッション管理（`TurboSessionManager`）**:
  - `curl_global_init(CURL_GLOBAL_ALL)` / `curl_global_cleanup()` は `TurboSessionManager` の参照カウント管理により初期化・解放する。
  - 複数リクエスト間で DNS キャッシュ・TLS セッション・HTTP/2 接続プールを競合なく共有するため、単一イベントループスレッド専有のマルチハンドル（`CURLM`、他スレッドからの起床通知 `curl_multi_wakeup()` を除く）を管理し、`CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX` を有効化する（マルチスレッドで非安全な `CURLSH` + `CURL_LOCK_DATA_CONNECT` は使用しない）。
  - `TurboHttpExecutor::Shutdown()` 完了後に `curl_multi_cleanup()` で解放し、解放後に再取得された場合は新しい `CURLM` ハンドルを自動再生成して復帰する。
- **単一イベントループによる非同期多重実行とフェーズ・キャンセル管理（`TurboHttpExecutor`）**:
  - 専用の単一バックグラウンドイベントループスレッド上で `curl_multi_perform` / `curl_multi_poll` / `curl_multi_info_read` を駆動し、各リクエストの `CURL` easy ハンドルを非同期に多重実行する（リクエスト毎の `std::thread::detach()` は行わない）。
  - 各 easy ハンドルには `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2_0`、`CURLOPT_PIPEWAIT = 1L`（HTTP/2 多重化待機）、および `CURLOPT_NOSIGNAL = 1L`（マルチスレッド安全）を設定する。
  - `Execute()` / `CancelRequest()` / `Shutdown()` はスレッドセーフなキュー・フラグ更新後に `curl_multi_wakeup()` でイベントループを即時起床させる。`CancelRequest()` はキャンセルフラグ設定と起床通知のみを行い、イベントループスレッド側で該当ハンドルを即座に `curl_multi_remove_handle` + `curl_easy_cleanup` し、`cancel` コールバックを 1 回のみ返却する。
  - `Shutdown()` 時は実行中の全リクエストを一括キャンセル・回収したうえでイベントループスレッドを確実に `join()` し、リソースリークや破棄後参照（UAF）を防止する。
  - プログレスコールバック（`CURLOPT_XFERINFOFUNCTION`）およびイベントループ上の経過時間チェックにより、実行フェーズ（`kConnecting` / `kSendingBody` / `kReceiving`）のタイムアウトを特定して分類する。
  - `maxResponseBytes` 超過時は `HeaderCallback` / `WriteCallback` の中断（`CURLE_WRITE_ERROR` / `ctx->is_exceeded_limit`）により即時停止し、`maxResponseBytesExceeded` を返却する。

---

## 2. エラーマッピング（libcurl → 共通エラーコード）

| ネイティブエラー / 状態 | 共通エラーコード | 判定条件・備考 |
| :--- | :--- | :--- |
| `ctx->is_exceeded_limit` 設定済み | `maxResponseBytesExceeded` | **最優先判定**: `HeaderCallback` / `WriteCallback` の上限超過検知による即時中断 |
| `ctx->timed_out_error_code` 設定済み | `connectionTimeout` / `sendTimeout` / `receiveTimeout` | プログレス監視またはイベントループ上の経過時間チェックによるフェーズ別タイムアウト中断 |
| `ctx->is_cancelled` / `CURLE_ABORTED_BY_CALLBACK` | `cancel` | リクエストキャンセル（タイムアウト未設定時） |
| `CURLE_OPERATION_TIMEDOUT`（`kConnecting`） | `connectionTimeout` | 接続・名前解決・TLS ハンドシェイク時のタイムアウト |
| `CURLE_OPERATION_TIMEDOUT`（`kSendingBody`） | `sendTimeout` | リクエストボディ送信中のタイムアウト |
| `CURLE_OPERATION_TIMEDOUT`（`kReceiving`） | `receiveTimeout` | レスポンス待機・ボディ受信中のタイムアウト |
| `CURLE_PEER_FAILED_VERIFICATION` / `CURLE_SSL_*` | `badCertificate` | SSL/TLS 証明書検証・ハンドシェイク失敗 |
| `CURLE_COULDNT_RESOLVE_HOST` / `CURLE_COULDNT_CONNECT` / `CURLE_HTTP2` / `CURLE_HTTP2_STREAM` / `CURLE_UNSUPPORTED_PROTOCOL` / `CURLE_TOO_MANY_REDIRECTS` / `CURLE_URL_MALFORMAT` | `connectionError` | ネットワーク接続不良・名前解決失敗・リダイレクト異常・不正 URL / ヘッダ |
| その他未分類の `CURLcode` | `unknown` | 予期せぬ実行時エラー |
