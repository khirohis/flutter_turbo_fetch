# 開発ロードマップ・ステップ一覧

本プロジェクト（`flutter_turbo_fetch`）は、最初から **Flutter Plugin パッケージ構成（＋ `example/` アプリでの PoC・ベンチマーク検証）** として段階的に開発を進める。

---

## Phase 0: 事前設計・既存パッケージ検証・アーキテクチャ確定

コードを書く前に、既存パッケージで代替可能かの見極めと、プラグインの責務・ネイティブ⇄Dart 間の通信仕様を確定させるフェーズ。

- [x] **0-1. `pub.dev` 既存パッケージ（特に `rhttp`）の適合性・代替可能性の最終判断**
  - **決定事項**: `rhttp` は不採用（Rust バイナリサイズ増・OS 標準 TLS/ネットワーク統合非利用のため）とし、ベンチマーク比較対象にも含めず「OS 標準/軽量な `OkHttp` + `URLSession` で自作プラグイン（`flutter_turbo_fetch`）を作る」方針で確定（[`docs/references/design-rationale.md`](references/design-rationale.md) 参照）。
- [x] **0-2. プラグインの責務（Dio `HttpClientAdapter` & `Transformer` 準拠範囲）の定義**
  - **決定事項**（[`docs/architectures/dart-layer.md`](architectures/dart-layer.md) 参照）:
    - **公開クラス名**: `TurboHttpAdapter` / `TurboTransformer`
    - **`TurboTransformer` の対応出力データ**: バイト列（`ResponseType.bytes` → `Uint8List`）、テキスト（`ResponseType.plain` → `String`）、JSON パースオブジェクト（`ResponseType.json` → `dynamic`）の 3 種類すべてをゼロコピー＆閾値分岐対応とする。
    - **`TurboHttpAdapter` の必須スコープ**: GET/POST/PUT/DELETE/PATCH/HEAD 等の基本メソッド、リクエストヘッダ/ボディ（サイズ制限なしで `Uint8List` 化）、レスポンスヘッダ/ステータスコード/一括ボディ（`Uint8List`）、タイムアウト制御（connect/receive/send）、キャンセル（`CancelToken` 連動）、リダイレクト追従設定。
    - **フォールバック・プラットフォーム仕様**: `ResponseType.stream` のみ標準 `IOHttpClientAdapter` へ自動フォールバックする。また、Android / iOS 以外のプラットフォームで実行された場合は `UnsupportedError` をスローする。
- [x] **0-3. Dart ⇄ ネイティブ間の通信ブリッジ方式の選定**
  - **決定事項**: **Pigeon（`StandardMessageCodec` ベースの型安全コード生成）** を採用する。1 リクエスト 1 往復のみのためメモリコピーコストは無視できる水準であり、バックグラウンドスレッドからの安全なコールバック返却と Dart/Kotlin/Swift 間の型安全性を優先する（[`docs/architectures/native-layer.md`](architectures/native-layer.md) および [`docs/references/design-rationale.md`](references/design-rationale.md) 参照）。
- [x] **0-4. ターゲット環境・ネイティブ依存ライブラリのバージョン定義**
  - **決定事項**（[`docs/architectures/native-layer.md`](architectures/native-layer.md) 参照）:
    - **Dart / Flutter / Dio**: Dart `^3.5.0`、Flutter `>=3.24.0`、`dio: ^5.4.0`（Dio 5系）
    - **Android**: `minSdk = 24`（Android 7.0+）、`com.squareup.okhttp3:okhttp:4.12.0`
    - **iOS**: 最小 Deployment Target `iOS 13.0+`、`URLSession.dataTask(with: request, completionHandler:)` を採用（`URLSessionDataTask` によるキャンセル制御）、Swift Package Manager（SPM）＋ CocoaPods 両対応。

---

## Phase 1: Plugin 雛形構築 & 最小パイプライン実装・ベンチマーク検証（PoC）

`flutter_turbo_fetch` の Plugin 構造と `example/` アプリを立ち上げ、最小のネイティブ一括受信＋`TurboTransformer` パイプラインを実装してベンチマークでアーキテクチャの実効性を証明するフェーズ。

- [x] **1-1. Flutter Plugin プロジェクト雛形の作成とディレクトリ設計**
  - **完了事項**: `io.github.khirohis` を Organization Identifier とする単一パッケージ構成（`lib/`, `pigeons/`, `android/`, `ios/`, `example/`）を作成。iOS は Swift Package Manager (`Package.swift`) + CocoaPods (`flutter_turbo_fetch.podspec`) 両対応（iOS 13.0+）、Android は `minSdk = 24` とし、不要な `PlatformInterface` ボイラープレートを排除したシンプル構成（`lib/src/turbo_http_adapter.dart`, `lib/src/turbo_transformer.dart`, `lib/src/messages.g.dart`）を構築。
- [x] **1-2. ベンチマーク用 `example/` アプリとモック/検証サーバー環境の準備**
  - **完了事項**:
    - `example/server/http2_mock_server.mjs` に Node.js 標準 `node:http2` による HTTP/2 モックサーバー（任意の KB サイズ JSON・遅延指定対応）を用意。
    - `example/lib/main.dart` に 4 プリセット（① 標準 `IOHttpClientAdapter` / ② `dio_http2_adapter` / ③ `native_dio_adapter` / ④ `TurboHttpAdapter` + `TurboTransformer`）の切り替え、並列数（10 / 15 / 20）、`ResponseType`（`json` / `plain` / `bytes`）選択、および所要時間（初回完了・全体完了・平均/最大レイテンシ）・UI ジャンクフレーム数（`FrameTiming` > 16.67ms）・メモリ使用量（RSS Before / Peak / After）を計測・比較するハーネスを実装。
- [x] **1-3. Android ネイティブ層（`OkHttp` 一括受信）の基本実装**
  - **完了事項**:
    - `android/build.gradle.kts` に `com.squareup.okhttp3:okhttp:4.12.0` を導入し、共有の `OkHttpClient`（`ConnectionPool`、`Dispatcher.maxRequestsPerHost = 32` デフォルト＋指定値に応じた `maxRequests` 自動引き上げ）を構成。
    - `FlutterTurboFetchPlugin.kt` にて `Call.enqueue(Callback)` のバックグラウンドスレッド上で `response.body?.bytes()` による一括結合を行い、`Handler(Looper.getMainLooper())` 経由で Dart へ 1 回で返却する処理を実装（`allowBadCertificates` オプションおよび `cancelRequest` 対応を含む）。
- [x] **1-4. iOS ネイティブ層（`URLSession` Completion 一括受信）の基本実装**
  - **完了事項**:
    - `FlutterTurboFetchPlugin.swift` にて `URLSessionDataDelegate` を使わず、`URLSession.dataTask(with: request, completionHandler:)` を用いて CFNetwork 層で一括結合された `Data` を 1 回で Dart へ返却する処理を実装（`httpMaximumConnectionsPerHost` 設定、`NSLock` による `URLSessionDataTask` 辞書管理・`cancelRequest`、`URLError` 分類、および `allowBadCertificates` / `followRedirects` 制御を含む）。
- [ ] **1-5. `TurboHttpAdapter` ＋ `TurboTransformer` の結合と並列ベンチマーク検証**
  - ネイティブから受け取った `Uint8List` を `ResponseBody.fromBytes(bytes, statusCode, headers: ...)` で Dio へ渡し、`TurboTransformer` へ流す一気通貫パイプラインを動かす。
  - `example/` アプリ上でベースライン（①〜③）との並列通信パフォーマンス比較を行う。

---

## Phase 2: プロダクション機能の本実装（キャンセル・タイムアウト・エラー統合）

PoC で実証したコアパイプラインに対し、実運用で必須となる通信制御・ライフサイクル管理・エラーマッピングを完備するフェーズ。

- [ ] **2-1. 通信インターフェース（Pigeon スキーマ）の確定・拡張**
  - リクエスト情報（URL、Method、Headers、Body、各種 Timeout ミリ秒、リダイレクト設定、RequestID 等）と、レスポンス情報（StatusCode、Headers、Body `Uint8List`、ErrorCode/Message）の型定義を確定させる。
  - リクエスト中断用のインターフェース（`cancelRequest(requestId)`）を定義する。
- [ ] **2-2. Android Plugin クラス（`FlutterPlugin`）のプロダクション対応**
  - リクエスト ID と `okhttp3.Call` の `ConcurrentHashMap` 管理（キャンセル用）、リクエスト別のタイムアウト/リダイレクト設定（`newBuilder()`）、Plugin デタッチ時のリソース解放（`dispatcher.executorService.shutdown()` 等）を実装する。
- [ ] **2-3. iOS Plugin クラス（`FlutterPlugin`）のプロダクション対応**
  - リクエスト ID と `URLSessionDataTask` の辞書管理（キャンセル用）、HTTP ヘッダやタイムアウトのマッピング、エラー（`URLError`）の変換処理、リソース解放を実装する。
- [ ] **2-4. `TurboHttpAdapter` & `TurboTransformer` の API 仕上げ**
  - 利用側アプリが `dio.httpClientAdapter = TurboHttpAdapter(...)` と `dio.transformer = TurboTransformer(...)` の 2 行で導入できるよう公開 Dart API とチューニングパラメータ（閾値・最大並列 Isolate 数）を整える。
  - `options.responseType == ResponseType.stream` が指定された場合の標準 `IOHttpClientAdapter` への自動フォールバック機能を実装する。
- [ ] **2-5. エラーハンドリングと `DioException` マッピングの統一・異常系検証**
  - Android（`SocketTimeoutException`, `UnknownHostException`, `IOException` 等）と iOS（`URLError.timedOut`, `.notConnectedToInternet`, `.cancelled` 等）のエラーコードを共通化し、Dio 標準の `DioExceptionType`（`connectionTimeout`, `receiveTimeout`, `cancel`, `badCertificate`, `connectionError`）へ正確に変換する。
  - キャンセル時の挙動（`Call.cancel()` / `URLSessionTask.cancel()`）およびエラー時（タイムアウト・TLS エラー・非 200 系ステータス）の挙動を `example/` アプリ上で検証する。

---

## Phase 3: テスト・パッケージ品質整備・プライベート導入（非公開運用）

`pub.dev` には公開しない前提で、実案件へ安全に組み込むための自動テスト・ドキュメント・依存管理を整えるフェーズ。

- [ ] **3-1. 自動テスト（Unit Test / Integration Test）の整備**
  - Dart 層のモックテスト（アダプタ変換、エラーマッピング、ストリーム時のフォールバック、`TurboTransformer` の 3 データ種別・閾値分岐・セマフォ制御）と、`example/` アプリを用いた Android/iOS 実機・シミュレータでの `integration_test` を作成する。
- [ ] **3-2. 導入ドキュメント（`README.md`）と設定ガイドの作成**
  - `pubspec.yaml` での `path:` / `git:` 指定方法、ProGuard / R8 ルール（Android の OkHttp 関連）、ATS（App Transport Security）等の OS 別注意事項、およびチューニングパラメータ（Isolate 閾値・セマフォ同時実行数・タイムアウト）の解説を記述する。
