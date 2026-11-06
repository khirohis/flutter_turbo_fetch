# Windows ネイティブ層仕様（WinHTTP）

> 通信ブリッジ仕様・データモデル・共通エラーコード定義については [`native-layer.md`](./native-layer.md) を参照。

---

## 1. 概要 & 技術スタック

- **ターゲット**: Windows 10 バージョン 1809（Build 17763）以降（64-bit: x64 / ARM64）
- **主要基盤**: Windows OS 標準 `WinHTTP`（`winhttp.dll`）＋ Windows Thread Pool API（C++17）
- **主要プロトコル**: HTTP/2（`WINHTTP_PROTOCOL_FLAG_HTTP2` による ALPN ネゴシエーション・多重化）、HTTP/1.1 フォールバック

---

## 2. クラス構成 & 責務分割（3層構成）

Flutter Engine / Pigeon との結合層、1 リクエストの通信実行・スレッドプール・実行中リクエスト管理層、および共有 WinHTTP セッション管理層を明確に 3 分割し、単一責任の原則（SRP）と RAII による確実なリソース解放を確保する。

```text
┌────────────────────────────────────────────────────────┐
│  FlutterTurboFetchPlugin (Flutter インターフェース層)   │
│  - flutter::Plugin ライフサイクル (Register / Dtor)     │
│  - Pigeon ホスト API 実装 (ExecuteRequest / CancelRequest) │
└───────────────────────────▲────────────────────────────┘
                            │ 委譲 (WinHTTP 依存なし)
┌───────────────────────────▼────────────────────────────┐
│  TurboHttpExecutor (WinHTTP 通信実行 & スレッド管理層)  │
│  - OS スレッドプール管理 (TP_POOL)                      │
│  - 実行中リクエスト管理 (active_requests_ + std::mutex) │
│  - 同期 WinHTTP API による一括受信 & フェーズ追跡       │
│  - WinHTTP エラーの共通エラーコード分類                 │
└───────────────────────────▲────────────────────────────┘
                            │ セッション取得 (AcquireSession)
┌───────────────────────────▼────────────────────────────┐
│  TurboSessionManager (WinHTTP セッション・接続管理層)   │
│  - 共有セッション管理 (HINTERNET: WinHttpOpen)          │
│  - HTTP/2 有効化 (WINHTTP_PROTOCOL_FLAG_HTTP2)          │
│  - 1 ホストあたり同時接続数の動的適用 & Shutdown 復帰   │
└────────────────────────────────────────────────────────┘
```

### ① `FlutterTurboFetchPlugin`（Flutter インターフェース層）

- **役割**: Flutter Engine / Pigeon とのブリッジ。WinHTTP への直接依存を持たない。
- **保持する状態**:
  - `executor_`: `TurboHttpExecutor`
- **公開 / 内部インターフェース**:
  - `RegisterWithRegistrar` / `~FlutterTurboFetchPlugin`
  - `ExecuteRequest(request, result)` / `CancelRequest(request_id, result)`

### ② `TurboHttpExecutor`（WinHTTP 通信実行 & スレッド管理層）

- **役割**: OS スレッドプール（`TP_POOL`）および実行中リクエスト（`RequestContext`）のライフサイクル管理、1 リクエスト単位の通信実行・一括受信・エラー分類。
- **保持する状態**:
  - `session_manager_`: `TurboSessionManager`
  - `pool_`: スレッドプールおよびクリーンアップグループ
  - `active_requests_`: `requestId` と `std::shared_ptr<RequestContext>` のマップ（`std::mutex` で排他保護）
- **公開 / 内部インターフェース**:
  - `Execute(request, on_result)`: 非同期リクエスト実行
  - `Cancel(request_id)`: 指定リクエストの即時キャンセル
  - `Shutdown()`: 全実行中リクエストのキャンセル・スレッドプール待機および `session_manager_.Shutdown()` の実行
  - `ClassifyError(ctx, win32_error, phase)`: WinHTTP エラーコードの共通エラーコード分類
- **実行・キャンセル順序保証（競合防止）**:
  1. リクエスト構築時（URL やヘッダー不正等）にエラーが発生した場合は、`active_requests_` へ登録せず即座に `on_result(errorResponse)` を同期呼び出しして終了する。
  2. スレッドプールへタスクを投入する前に、**必ず同期的に `active_requests_[request_id] = ctx` を登録**し、非同期完了や即時キャンセルより先行することを保証する。
  3. 通信の完了または失敗時は、ハンドルを確実にクローズし、`active_requests_.erase(request_id)` を行った上で必ず 1 回のみ `on_result(response)` を呼び出す。

### ③ `TurboSessionManager`（WinHTTP セッション・接続管理層）

- **役割**: 共有 `HINTERNET` セッションハンドルの排他管理・HTTP/2 設定・同時接続数設定・Shutdown 復帰。
- **保持する状態**:
  - `session_handle_`: 共有 `HINTERNET`（`std::mutex` で排他保護）
- **公開 / 内部インターフェース**:
  - `AcquireSession(max_requests_per_host)`: HTTP/2 および同時接続数設定が適用された共有セッションハンドルの取得（未初期化・Shutdown 後の場合は再生成）
  - `Shutdown()`: 共有セッションハンドルのクローズ

---

## 3. セッション管理 & 同時リクエスト数制御（`TurboSessionManager`）

- **共有セッション**:
  - 同期モードかつ OS 自動プロキシ設定（`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`）の単一 `HINTERNET` セッションを共有し、HTTP/2 コネクションおよび TLS セッションを効率的に再利用する。
  - セッション生成時に `WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL`（`WINHTTP_PROTOCOL_FLAG_HTTP2`）を有効化する。
- **同時リクエスト数制御**:
  - `WINHTTP_OPTION_MAX_CONNS_PER_SERVER`（デフォルト `32`）を設定し、リクエスト指定の `maxRequestsPerHost` が変化した場合はセッションオプションを動的に更新する。
- **リソース復帰（再アタッチ対応）**:
  - `Shutdown()` 後に再度リクエストが実行された場合は、自動的に共有セッションを再生成して復帰する。

---

## 4. 通信実行・一括受信・エラー分類（`TurboHttpExecutor`）

- **リクエスト構築の安全性**:
  - 不正なヘッダー名/値（改行文字等）や無効な URL（`WinHttpCrackUrl` 失敗・非対応スキーム）が渡された場合でもクラッシュさせず、安全に `connectionError` レスポンスを生成する。
- **リクエスト単位の設定適用**:
  - リクエストハンドル（`hRequest`）単位でタイムアウト（`WinHttpSetTimeouts`: connect / send / receive）、リダイレクト追従設定（`WINHTTP_DISABLE_REDIRECTS`）、および `allowBadCertificates = true` 時の証明書検証スキップフラグ（`WINHTTP_OPTION_SECURITY_FLAGS`）を適用する。
- **フェーズ追跡**:
  - 接続・送信・受信の実行フェーズ（`kConnecting` / `kSendingBody` / `kReceiving`）を追跡し、タイムアウト発生時のエラー分類に使用する。
- **一括受信処理と `maxResponseBytes` の判定ロジック**:
  - OS スレッドプールのワーカースレッド上で実行し、RAII によりスコープ終了時に確実に `hRequest` / `hConnect` を解放する。
  - `maxResponseBytes` 指定時（`safeMaxBytes = max(0, maxResponseBytes)`）は、以下の 2 段階で超過を検証する:
    1. レスポンスヘッダの `Content-Length` が `safeMaxBytes` を超える場合（`contentLength > safeMaxBytes`）は、ボディ読み出し前に即時中断し `maxResponseBytesExceeded` を返却する。
    2. `WinHttpReadData` によるチャンク読み出しごとに累積バイト数を確認し、`safeMaxBytes` を超過した時点で即座に中断して `maxResponseBytesExceeded` を返却する。
  - 上限内の場合は、全チャンクを結合した単一の `std::vector<uint8_t>` を返却する。
- **エラー分類（WinHTTP エラー → 共通エラーコード）**:

| ネイティブエラー / 状態 | 共通エラーコード | 備考 |
| :--- | :--- | :--- |
| `ctx->is_cancelled` / `ERROR_WINHTTP_OPERATION_CANCELLED` | `cancel` | リクエストキャンセル |
| `ERROR_WINHTTP_TIMEOUT`（フェーズ = `kConnecting`） | `connectionTimeout` | 接続・TLS ハンドシェイク時のタイムアウト |
| `ERROR_WINHTTP_TIMEOUT`（フェーズ = `kSendingBody`） | `sendTimeout` | リクエストボディ送信中のタイムアウト |
| `ERROR_WINHTTP_TIMEOUT`（フェーズ = `kReceiving`） | `receiveTimeout` | レスポンス待機・受信中のタイムアウト |
| `ERROR_WINHTTP_SECURE_FAILURE` / `ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED` / `SEC_E_*` | `badCertificate` | 証明書検証・TLS 接続エラー |
| `ERROR_WINHTTP_NAME_NOT_RESOLVED` / `CANNOT_CONNECT` / `CONNECTION_ERROR` / `INVALID_URL` / `UNRECOGNIZED_SCHEME` / `REDIRECT_FAILED` | `connectionError` | ネットワーク接続不良・名前解決失敗・リダイレクト異常等 |
| その他未分類のエラー | `unknown` | 予期せぬ実行時エラー |

---

## 5. ライフサイクル & 並行管理

- **キャンセル処理**:
  - `CancelRequest(requestId)` 呼び出し時、`executor_->Cancel(requestId)` を実行し、`RequestContext` の排他ロック下で `WinHttpCloseHandle(hRequest)` を呼び出して待機中の通信を即座に中断させる。
- **Engine デタッチ時のクリーンアップ**:
  - プラグイン破棄時に `executor_->Shutdown()` を呼び出し、実行中の全リクエストの一括キャンセル、スレッドプール上の全ワーカー完了待機、および `session_manager_.Shutdown()` による共有セッション解放を行う。
