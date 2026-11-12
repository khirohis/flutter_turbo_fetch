# ネイティブ層共通仕様および通信ブリッジ仕様

> 通信方式（MethodChannel/Pigeon vs FFI）の比較背景については [`docs/design-rationale.md`](../../docs/design-rationale.md) を参照。  
> 各 OS 固有の設計・エラーマッピングについては [`android-native-layer.md`](./android-native-layer.md) / [`darwin-native-layer.md`](./darwin-native-layer.md) / [`windows-native-layer.md`](./windows-native-layer.md) / [`linux-native-layer.md`](./linux-native-layer.md) を参照。

---

## 1. Dart ⇄ ネイティブ間の通信ブリッジ仕様（Pigeon）

通信ブリッジには **Pigeon（`StandardMessageCodec` ベースの型安全コード生成、`@HostApi` + `@async`）** を採用し、OS バックグラウンドで結合した単一のレスポンスボディ `Uint8List` を 1 リクエストにつき 1 往復でやり取りする。  
※ 生成コードは、iOS / macOS は `darwin/` 単一ソース（Swift）、Windows は `windows/`（Flutter C++ Client Wrapper 準拠の C++）、Linux は `linux/`（Flutter Linux GTK/GObject C API 準拠の GObject）にそれぞれ配置する。

- **リクエスト情報（`TurboHttpRequest`）**:
  - `requestId`: リクエスト識別子（キャンセル紐付け用）
  - `url` / `method` / `headers`（`Map<String?, String?>`） / `body`（nullable `Uint8List`）
  - `connectTimeoutMs` / `receiveTimeoutMs` / `sendTimeoutMs`: 各フェーズのタイムアウト（ミリ秒）
  - `followRedirects`: リダイレクト追従設定（真偽値）
  - `maxRequestsPerHost`: 1 ホストあたりの最大同時リクエスト数（デフォルト `32`）
  - `allowBadCertificates`: 自己署名証明書等の許可設定（ローカル検証用、デフォルト `false`）
  - `maxResponseBytes`: レスポンスボディの最大許容バイト数（nullable、未指定時は無制限）
- **レスポンス情報（`TurboHttpResponse`）**:
  - `statusCode` / `statusMessage`
  - `headers`: マルチバリュー対応レスポンスヘッダ（`Map<String?, Object?>?`。Dart 側で小文字キーの `Map<String, List<String>>` へ正規化）
  - `body`: 一括結合済みレスポンスボディ（`Uint8List`）
  - `errorCode` / `errorMessage`: エラー発生時の共通エラーコードと詳細メッセージ
- **キャンセル API（`TurboFetchHostApi`）**:
  - `cancelRequest(requestId)`: 指定リクエストを即座に中断する。
  - ※ 全リクエストの一括破棄は、Engine デタッチ時に各ネイティブの内部 `shutdown()` で実行されるほか、Dart の `close(force: true)` からは管理中のアクティブ `requestId` ごとに `cancelRequest` が呼び出される。

---

## 2. 全プラットフォーム共通のアーキテクチャ不変条件

すべての対応 OS（Android / iOS / macOS / Windows / Linux）のネイティブ実装は、以下の共通設計原則に従う。

1. **3層クラス責務分離（SRP & テスタビリティ）**:
   - **`FlutterTurboFetchPlugin`（インターフェース層）**: Flutter Engine / Pigeon とのブリッジおよびメインスレッドへのコールバック返却を担当し、HTTP 通信ライブラリへの直接依存を持たない。
   - **`TurboHttpExecutor`（通信実行・タスク管理層）**: 実行中リクエストの排他管理、1 リクエスト単位の非同期一括受信、およびネイティブ例外から共通エラーコードへの分類を担当する。
   - **`TurboClientManager` / `TurboSessionManager`（セッション・接続管理層）**: HTTP/2 共有クライアント/セッションの保持、`maxRequestsPerHost` の動的適用、および Shutdown 後の自動再生成（再アタッチ復帰）を担当する。
2. **実行・キャンセルの競合防止とコールバック保証**:
   - リクエスト構築時のエラー（不正な URL や改行文字を含む不正ヘッダ等）はクラッシュさせず、アクティブ管理マップへ登録する前に即座に `connectionError` レスポンスを返却する。
   - 非同期通信タスクを開始する **前に必ず同期的に `requestId` をアクティブ管理マップへ登録** し、直後のキャンセル呼び出しや即時完了との競合（Race Condition）を防止する。
   - 正常完了・エラー・キャンセルのいずれの経路でも、通信ハンドルと登録情報を確実に解放した上でコールバックを **1 リクエストにつき 1 回のみ** 呼び出す。
3. **`maxResponseBytes` の2段階フェイルセーフ検証**:
   - 上限指定時は、(1) `Content-Length` ヘッダが上限を超える場合はボディ読み出し前に即時中断し、(2) ストリーム/チャンク読み出し中または完了時に実データ量が上限を超えた場合もデータを切り詰めて正常返却することはせず、即座に中断して `maxResponseBytesExceeded` を返却する。
4. **Engine デタッチ時のクリーンアップと自動復帰**:
   - Engine デタッチ（またはプラグイン破棄）時は、実行中の全リクエストを一括キャンセルし、共有セッション・接続プール・ワーカースレッドを確実に解放する。その後再びリクエストが実行された場合は、共有セッションを自動的に再生成して復帰する。

---

## 3. プラットフォーム共通エラーコード体系

ネイティブ層で発生した例外・エラーは、Dart 層の `DioExceptionType` と整合する以下の共通エラーコードへ正規化して返却する。

| 共通エラーコード | 対応する `DioExceptionType` | 発生契機 |
| :--- | :--- | :--- |
| `cancel` | `DioExceptionType.cancel` | `cancelRequest` 呼び出し、または通信中断 |
| `connectionTimeout` | `DioExceptionType.connectionTimeout` | TCP 接続確立 / TLS ハンドシェイク時のタイムアウト |
| `sendTimeout` | `DioExceptionType.sendTimeout` | リクエストボディ送信中のタイムアウト |
| `receiveTimeout` | `DioExceptionType.receiveTimeout` | レスポンス待機・ボディ受信中のタイムアウト |
| `badCertificate` | `DioExceptionType.badCertificate` | SSL/TLS 証明書検証エラー |
| `maxResponseBytesExceeded` | `DioExceptionType.unknown` | 受信データ量が `maxResponseBytes` 上限を超過（上限値をメッセージに明記） |
| `connectionError` | `DioExceptionType.connectionError` | DNS 解決失敗、接続拒否、到達不能、不正な URL / ヘッダ等 |
| `unknown` | `DioExceptionType.unknown` | プラットフォーム内部の予期せぬ実行時例外 |
