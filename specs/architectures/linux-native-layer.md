# Linux ネイティブ層仕様（libcurl）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. Linux 固有の設計・実装仕様

- **GTK メインループ連携とライフサイクル（`FlutterTurboFetchPlugin`）**:
  - Pigeon GObject 生成コード（`linux/messages.g.h` / `linux/messages.g.cc`）を使用し、`fl_plugin_registrar_get_messenger` 経由で Host API メソッドハンドラーを登録する。
  - イベントループスレッドでのリクエスト完了時の Pigeon コールバックは、`g_idle_add` を用いて GLib/GTK メインループからメインスレッドへディスパッチし、プラグイン破棄時は GObject `dispose` で `TurboHttpExecutor::Shutdown()` → `TurboSessionManager::Shutdown()` の順にシャットダウンを実行する。
- **共有接続・セッション管理（`TurboSessionManager`）**:
  - `curl_global_init(CURL_GLOBAL_ALL)` / `curl_global_cleanup()` はスレッドセーフな参照カウント管理により初期化・解放する。
  - 複数リクエスト間で DNS キャッシュ・TLS セッション・HTTP/2 接続プールを競合なく共有するため、単一イベントループスレッド専有のマルチハンドル（`CURLM`、他スレッドからの起床通知 `curl_multi_wakeup()` を除く）を管理し、`CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX` を有効化する（マルチスレッドで非安全な `CURLSH` + `CURL_LOCK_DATA_CONNECT` は使用しない）。
  - `TurboHttpExecutor::Shutdown()` 完了後に `curl_multi_cleanup()` で解放し、解放後に再取得された場合は新しい `CURLM` ハンドルを自動再生成して復帰する。
- **単一イベントループによる非同期多重実行とフェーズ・キャンセル管理（`TurboHttpExecutor`）**:
  - 通信開始前に HTTP メソッド・URL（`http`/`https`・ホスト必須・制御文字禁止）・ヘッダ（RFC 準拠のキー・値）を検証し、不正時はアクティブ管理へ登録せず即座に `connectionError` を返却する。
  - 専用の単一バックグラウンドイベントループスレッド上で `curl_multi_perform` / `curl_multi_poll` / `curl_multi_info_read` を駆動し、各リクエストの `CURL` easy ハンドルを非同期に多重実行する（リクエスト毎の `std::thread::detach()` は行わない）。
  - 各 easy ハンドルでは HTTP/2 多重化（`CURL_HTTP_VERSION_2_0` / `CURLOPT_PIPEWAIT`）とスレッド安全（`CURLOPT_NOSIGNAL`）を有効化し、透過的圧縮解凍（利用者による明示的な `Accept-Encoding` 未指定時）および不要な `Expect: 100-continue` 遅延の抑止を適用する。また、空ボディ `POST` 時もボディ長 `0` を明示してハングを防止する。
  - `Execute()` / `CancelRequest()` / `Shutdown()` はスレッドセーフな状態更新後に `curl_multi_wakeup()` でイベントループを即時起床させる。`CancelRequest()` 時はイベントループスレッド側で該当ハンドルを即座に回収・解放し、`cancel` コールバックを 1 回のみ返却する。
  - `Shutdown()` 時は実行中の全リクエストを一括キャンセル・回収したうえでイベントループスレッドを確実に `join()` し、リソースリークや破棄後参照（UAF）を防止する。
  - 実行フェーズ（`kConnecting` / `kSendingBody` / `kReceiving`）を接続確立・送信進捗・応答開始に応じて正確に遷移させ、フェーズ別タイムアウトを検知・分類する。`send` / `receive` フェーズは無通信アイドル時間がそれぞれのタイムアウトを超過した時点で中断し、イベントループの待機時間はアクティブ要求の最短デッドラインに応じて調整する。
  - リダイレクト追従時はリダイレクト先も `http` / `https` スキームのみに制限し、中間応答のヘッダおよびボディを破棄して最終レスポンスのヘッダ・ボディ・ステータスメッセージのみを採用する。
  - レスポンスボディの受信では不要な中間コピーや再確保を避けつつ、`maxResponseBytes`（指定時は `0` 以上の値として正規化）を `Content-Length` と累積受信量の 2 段階で検証し、超過検知時点で即時中断して `maxResponseBytesExceeded` を返却する（リダイレクト中間応答や `HEAD` の `Content-Length` では誤中断しない）。

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
