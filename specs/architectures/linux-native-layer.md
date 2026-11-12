# Linux ネイティブ層仕様（libcurl）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. Linux 固有の設計・実装仕様

- **GTK メインループ連携とライフサイクル（`FlutterTurboFetchPlugin`）**:
  - Flutter Linux 公式仕様に準拠した Pigeon GObject 生成コード（`linux/messages.g.h` / `linux/messages.g.cc`）を使用し、`fl_plugin_registrar_get_messenger` 経由で Host API メソッドハンドラーを登録する。
  - GObject ハンドラーから C++ の `TurboHttpExecutor` インスタンスへ処理を委譲し、OS バックグラウンドでの通信を実行する。
  - `curl_global_init(CURL_GLOBAL_ALL)` をプラグイン初期化時に呼び出し、Engine デタッチ時に `curl_global_cleanup()` で解放する。
  - ワーカースレッド完了時の Pigeon コールバックは、`g_idle_add` 等を用いて GLib/GTK メインループを介してメインスレッド側へディスパッチする。
  - Engine デタッチ時（`fl_plugin_registrar_get_messenger` 破棄等）は、未完了リクエストのキャンセルとセッションクリーンアップを実行する。
- **共有接続・セッション管理（`TurboSessionManager`）**:
  - DNS キャッシュ・TLS セッション・コネクションプールを複数リクエスト間で共有するため、共有ハンドル（`CURLSH`）を排他制御付きで保持する。
  - HTTP/2（`CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2_0`）による多重化通信を行い、`maxRequestsPerHost` に基づくホスト単位の同時実行制限（セマフォ制御等）を適用する。
- **ワーカースレッド実行とフェーズ・キャンセル管理（`TurboHttpExecutor`）**:
  - バックグラウンドワーカースレッド上で同期 `curl_easy_perform` を実行し、CURL ハンドルおよびリソースは RAII で確実に解放する。
  - プログレスコールバック（`CURLOPT_XFERINFOFUNCTION`）により、キャンセル要求（`is_cancelled`）を検知して即座に中断する。
  - 実行フェーズ（`kConnecting` / `kSendingBody` / `kReceiving`）を追跡し、接続タイムアウト（`CURLOPT_CONNECTTIMEOUT_MS`）および送受信時の超過を特定して分類する。

---

## 2. エラーマッピング（libcurl → 共通エラーコード）

| ネイティブエラー / 状態 | 共通エラーコード | 判定条件・備考 |
| :--- | :--- | :--- |
| `ctx->timed_out_error_code` 設定済み | `connectionTimeout` / `sendTimeout` / `receiveTimeout` | **最優先判定**: プログレス監視によるフェーズ別タイムアウト中断 |
| `ctx->is_cancelled` / `CURLE_ABORTED_BY_CALLBACK` | `cancel` | リクエストキャンセル（タイムアウト未設定時） |
| `CURLE_OPERATION_TIMEDOUT`（`kConnecting`） | `connectionTimeout` | 接続・名前解決・TLS ハンドシェイク時のタイムアウト |
| `CURLE_OPERATION_TIMEDOUT`（`kSendingBody`） | `sendTimeout` | リクエストボディ送信中のタイムアウト |
| `CURLE_OPERATION_TIMEDOUT`（`kReceiving`） | `receiveTimeout` | レスポンス待機・ボディ受信中のタイムアウト |
| `CURLE_PEER_FAILED_VERIFICATION` / `CURLE_SSL_*` | `badCertificate` | SSL/TLS 証明書検証・ハンドシェイク失敗 |
| `ctx->is_exceeded_limit` / `CURLE_WRITE_ERROR`（上限超過時） | `maxResponseBytesExceeded` | 受信データ量が `maxResponseBytes` 上限を超過 |
| `CURLE_COULDNT_RESOLVE_HOST` / `CURLE_COULDNT_CONNECT` / `CURLE_HTTP2` / `CURLE_UNSUPPORTED_PROTOCOL` / `CURLE_TOO_MANY_REDIRECTS` / `CURLE_URL_MALFORMAT` | `connectionError` | ネットワーク接続不良・名前解決失敗・リダイレクト異常・不正 URL / ヘッダ |
| その他未分類の `CURLcode` | `unknown` | 予期せぬ実行時エラー |
