# Windows ネイティブ層仕様（WinHTTP）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. 技術スタック

- **ターゲット**: Windows 10 バージョン 1809（Build 17763）以降（64-bit: x64 / ARM64）
- **主要基盤**: C++17, Windows OS 標準 `WinHTTP`（`winhttp.dll`, `WINHTTP_PROTOCOL_FLAG_HTTP2`）＋ Windows Thread Pool API（`TP_POOL`）

---

## 2. Windows 固有の設計・実装仕様

- **共有セッション管理（`TurboSessionManager`）**:
  - OS 自動プロキシ（`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`）および HTTP/2（`WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL`）を有効化した単一の同期 `HINTERNET` セッションを共有する。
  - `WINHTTP_OPTION_MAX_CONNS_PER_SERVER`（デフォルト `32`）により同時接続数を動的に更新する。
- **OS スレッドプール実行とハンドル・フェーズ管理（`TurboHttpExecutor`）**:
  - OS スレッドプール（`TP_POOL` およびクリーンアップグループ）のワーカースレッド上で同期 WinHTTP API を実行し、`hConnect` / `hRequest` ハンドルは RAII で確実に解放する。
  - リクエストハンドル単位でタイムアウト（`WinHttpSetTimeouts`）、リダイレクト設定（`WINHTTP_DISABLE_REDIRECTS`）、および `allowBadCertificates = true` 時の証明書検証スキップフラグ（`WINHTTP_OPTION_SECURITY_FLAGS`）を適用する。
  - キャンセル時は `RequestContext` の排他ロック下で `WinHttpCloseHandle(hRequest)` を呼び出し、待機中の同期通信を即座に中断させる。
  - 実行フェーズ（`kConnecting` / `kSendingBody` / `kReceiving`）を追跡し、`ERROR_WINHTTP_TIMEOUT` 発生時のタイムアウト種別を特定する。
  - `maxResponseBytes` 指定時は、`Content-Length` 事前チェックに加え、`WinHttpReadData` のチャンク読み出しごとに累積バイト数を検証して上限超過時に即時中断する。

---

## 3. エラーマッピング（WinHTTP → 共通エラーコード）

| ネイティブエラー / 状態 | 共通エラーコード | 判定条件・備考 |
| :--- | :--- | :--- |
| `ctx->is_cancelled` / `ERROR_WINHTTP_OPERATION_CANCELLED` | `cancel` | リクエストキャンセル |
| `ERROR_WINHTTP_TIMEOUT`（`kConnecting`） | `connectionTimeout` | 接続・TLS ハンドシェイク時のタイムアウト |
| `ERROR_WINHTTP_TIMEOUT`（`kSendingBody`） | `sendTimeout` | リクエストボディ送信中のタイムアウト |
| `ERROR_WINHTTP_TIMEOUT`（`kReceiving`） | `receiveTimeout` | レスポンス待機・受信中のタイムアウト |
| `ERROR_WINHTTP_SECURE_FAILURE` / `ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED` / `SEC_E_*` | `badCertificate` | 証明書検証・TLS 接続エラー |
| `ERROR_WINHTTP_NAME_NOT_RESOLVED` / `CANNOT_CONNECT` / `CONNECTION_ERROR` / `INVALID_URL` / `UNRECOGNIZED_SCHEME` / `REDIRECT_FAILED` | `connectionError` | ネットワーク接続不良・名前解決失敗・リダイレクト異常・不正 URL / ヘッダ |
| その他未分類のエラー | `unknown` | 予期せぬ実行時エラー |
