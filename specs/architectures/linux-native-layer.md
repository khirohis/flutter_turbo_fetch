# Linux ネイティブ層仕様（libcurl）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. Linux 固有の設計・実装仕様

- **GTK メインループ連携とライフサイクル（`FlutterTurboFetchPlugin`）**:
  - Pigeon GObject 生成コード（`linux/messages.g.h` / `linux/messages.g.cc`）を使用し、`fl_plugin_registrar_get_messenger` 経由で Host API メソッドハンドラーを登録する。
  - ワーカースレッド完了時の Pigeon コールバックは、`g_idle_add` を用いて GLib/GTK メインループからメインスレッドへディスパッチし、プラグイン破棄時は GObject `dispose` でシャットダウンを実行する。
- **共有接続・セッション管理（`TurboSessionManager`）**:
  - `curl_global_init(CURL_GLOBAL_ALL)` / `curl_global_cleanup()` は `TurboSessionManager` の参照カウント管理により初期化・解放する。
  - DNS キャッシュ・TLS セッション・コネクションプールを複数リクエスト間で共有するため、共有ハンドル（`CURLSH`）を排他制御付きで保持する。
  - HTTP/2（`CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2_0`）による多重化通信を行い、同時接続数および多重化制御は `libcurl` 標準の接続プールに委ねる。
- **ワーカースレッド実行とフェーズ・キャンセル管理（`TurboHttpExecutor`）**:
  - バックグラウンドワーカースレッド上で同期 `curl_easy_perform` を実行し、CURL ハンドルおよびリソースは RAII で確実に解放する。
  - プログレスコールバック（`CURLOPT_XFERINFOFUNCTION`）により、キャンセル要求（`is_cancelled`）を検知して即座に中断する。
  - 実行フェーズ（`kConnecting` / `kSendingBody` / `kReceiving`）を追跡し、接続タイムアウト（`CURLOPT_CONNECTTIMEOUT_MS`）および送受信時の超過を特定して分類する。
  - `maxResponseBytes` 超過時は `WriteCallback` の中断（`CURLE_WRITE_ERROR` / `ctx->is_exceeded_limit`）により即時停止し、`maxResponseBytesExceeded` を返却する。

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
| `CURLE_COULDNT_RESOLVE_HOST` / `CURLE_COULDNT_CONNECT` / `CURLE_HTTP2` / `CURLE_UNSUPPORTED_PROTOCOL` / `CURLE_TOO_MANY_REDIRECTS` / `CURLE_URL_MALFORMAT` | `connectionError` | ネットワーク接続不良・名前解決失敗・リダイレクト異常・不正 URL / ヘッダ |
| その他未分類の `CURLcode` | `unknown` | 予期せぬ実行時エラー |
