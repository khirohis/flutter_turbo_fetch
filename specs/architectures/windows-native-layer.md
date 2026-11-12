# Windows ネイティブ層仕様（WinHTTP）

> 共通データモデル・3層責務分離・不変条件・共通エラーコードについては [`native-layer.md`](./native-layer.md) を参照。

---

## 1. Windows 固有の設計・実装仕様

- **Pigeon ブリッジ連携とライフサイクル（`FlutterTurboFetchPluginCApi`）**:
  - Pigeon 生成 C++ コードは `common/cpp/` の共通ファイルを参照し、Windows C API 経由で登録を行う。
- **共有セッション管理（`TurboSessionManager`）**:
  - OS 自動プロキシ（`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`）および HTTP/2（`WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL`）を有効化した単一の同期 `HINTERNET` セッションを共有する。
  - `WINHTTP_OPTION_MAX_CONNS_PER_SERVER`（デフォルト `32`）により同時接続数を動的に更新する。
- **OS スレッドプール実行とハンドル・フェーズ管理（`TurboHttpExecutor`）**:
  - OS スレッドプール（`TP_POOL`）のワーカースレッド上で同期 WinHTTP API を実行し、`hConnect` / `hRequest` ハンドルは RAII で確実に解放する。
  - キャンセル時は `RequestContext` の排他ロック下で `WinHttpCloseHandle(hRequest)` を呼び出し、待機中の同期通信を即座に中断させる。
  - 実行フェーズ（`kConnecting` / `kSendingBody` / `kReceiving`）を追跡し、`ERROR_WINHTTP_TIMEOUT` 発生時のタイムアウト種別を特定する。

---

## 2. エラーマッピング（WinHTTP → 共通エラーコード）

| ネイティブエラー / 状態 | 共通エラーコード | 判定条件・備考 |
| :--- | :--- | :--- |
| `ctx->is_cancelled` / `ERROR_WINHTTP_OPERATION_CANCELLED` | `cancel` | リクエストキャンセル |
| `ERROR_WINHTTP_TIMEOUT`（`kConnecting`） | `connectionTimeout` | 接続・TLS ハンドシェイク時のタイムアウト |
| `ERROR_WINHTTP_TIMEOUT`（`kSendingBody`） | `sendTimeout` | リクエストボディ送信中のタイムアウト |
| `ERROR_WINHTTP_TIMEOUT`（`kReceiving`） | `receiveTimeout` | レスポンス待機・受信中のタイムアウト |
| `ERROR_WINHTTP_SECURE_FAILURE` / `ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED` / `SEC_E_*` | `badCertificate` | 証明書検証・TLS 接続エラー |
| `ERROR_WINHTTP_NAME_NOT_RESOLVED` / `CANNOT_CONNECT` / `CONNECTION_ERROR` / `INVALID_URL` / `UNRECOGNIZED_SCHEME` / `REDIRECT_FAILED` | `connectionError` | ネットワーク接続不良・名前解決失敗・リダイレクト異常・不正 URL / ヘッダ |
| その他未分類のエラー | `unknown` | 予期せぬ実行時エラー |
