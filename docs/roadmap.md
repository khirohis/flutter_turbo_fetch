# 開発ロードマップ・ステップ一覧

本プロジェクト（`flutter_turbo_fetch`）は、Flutter Plugin パッケージ構成（＋ `example/` アプリでのベンチマーク検証）として段階的に開発を進める。各層の詳細仕様は [`docs/architectures/`](architectures/) を参照。

---

## Phase 0: 事前設計・既存パッケージ検証・アーキテクチャ確定

- [x] **0-1. `pub.dev` 既存パッケージ（特に `rhttp`）の適合性判断**
  - `rhttp` は不採用とし、OS 標準/軽量な `OkHttp` + `URLSession` で自作プラグインを構築する方針に確定（[`docs/references/design-rationale.md`](references/design-rationale.md)）。
- [x] **0-2. プラグインの責務（Dio `HttpClientAdapter` & `Transformer` 準拠範囲）の定義**
  - `TurboHttpAdapter` / `TurboTransformer` / `TurboOptions` の責務と 4 種の `ResponseType`（`stream` / `bytes` / `plain` / `json`）＋カスタムマッパー対応、非対応 OS での `UnsupportedError` 仕様を確定（[`docs/architectures/dart-layer.md`](architectures/dart-layer.md)）。
- [x] **0-3. Dart ⇄ ネイティブ間の通信ブリッジ方式の選定**
  - Pigeon（`StandardMessageCodec` ベースの型安全コード生成、`@HostApi` + `@async`）を採用（[`docs/architectures/native-layer.md`](architectures/native-layer.md)）。
- [x] **0-4. ターゲット環境・ネイティブ依存ライブラリのバージョン定義**
  - Dart `^3.5.0`、Flutter `>=3.24.0`、`dio: ^5.4.0`、Android `minSdk = 24`（`okhttp:4.12.0`）、iOS `13.0+`（SPM / CocoaPods 両対応）に確定。

---

## Phase 1: Plugin 雛形構築 & 最小パイプライン実装・ベンチマーク検証（PoC）

- [x] **1-1. Flutter Plugin プロジェクト雛形の作成とディレクトリ設計**
  - 単一パッケージ構成（`lib/`, `pigeons/`, `android/`, `ios/`, `example/`）および Pigeon コード生成基盤を構築（[`docs/architectures/architecture.md`](architectures/architecture.md)）。
- [x] **1-2. ベンチマーク用 `example/` アプリとモック/検証サーバー環境の準備**
  - HTTP/2 モックサーバー（`example/server/http2_mock_server.mjs`）と 4 プリセット比較ベンチマークアプリ（`example/lib/main.dart`）を実装。
- [x] **1-3. Android ネイティブ層（`OkHttp` 一括受信）の基本実装**
  - `FlutterTurboFetchPlugin.kt` にて共有 `OkHttpClient` によるバックグラウンド一括受信・キャンセル・自己署名証明書許可を実装。
- [x] **1-4. iOS ネイティブ層（`URLSession` Completion 一括受信）の基本実装**
  - `FlutterTurboFetchPlugin.swift` にて `URLSession.dataTask(with:completionHandler:)` による一括受信・キャンセル・自己署名証明書許可を実装。
- [x] **1-5. `TurboHttpAdapter` ＋ `TurboTransformer` の結合と並列ベンチマーク検証**
  - ネイティブ一括受信〜ゼロコピー変換パイプラインを結合し、ベースライン（`IOHttpClientAdapter` / `dio_http2_adapter` / `native_dio_adapter`）に対する速度・UI フレームスパイク抑制の優位性を実証。

---

## Phase 2: プロダクション機能の本実装（キャンセル・タイムアウト・エラー統合）

PoC で実証したコアパイプラインに対し、実運用で必須となる通信制御・ライフサイクル管理・エラーマッピング・カスタムオブジェクトマッピングを完備するフェーズ。

- [x] **2-1. 通信インターフェース（Pigeon スキーマ）の確定・拡張**
  - [`docs/architectures/native-layer.md`](architectures/native-layer.md) に基づき、リクエスト情報（`maxResponseBytes` 追加等）・レスポンス情報・`cancelRequest(requestId)` のスキーマを確定しコードを再生成する。
- [x] **2-2. Android Plugin クラス（`FlutterPlugin`）のプロダクション対応**
  - `maxResponseBytes` 指定時のサイズ上限チェック（超過時は即座にストリームを切断し `maxResponseBytesExceeded` を返却）および Engine デタッチ時のリソース解放を実装する。
- [x] **2-3. iOS Plugin クラス（`FlutterPlugin`）のプロダクション対応**
  - `maxResponseBytes` 指定時のサイズ上限チェック（超過時は `maxResponseBytesExceeded` を返却）および Engine デタッチ時のリソース解放（`invalidateAndCancel()`）を実装する。
- [x] **2-4. `TurboHttpAdapter`、`TurboTransformer` & `TurboOptions` の API 仕上げ**
  - [`docs/architectures/dart-layer.md`](architectures/dart-layer.md) に基づき、`TurboOptions<T>`（`mapper` / `maxResponseBytes`）の実装、`TurboHttpAdapter` のフォールバック廃止（`ResponseType.stream` の単一チャンクラップ統一）、`TurboTransformer` の 4 データ種別＋マッパー対応および動的デフォルトセマフォ数設定を実装する。
- [x] **2-5. エラーハンドリングと `DioException` マッピングの統一・異常系検証**
  - Android / iOS のネイティブエラーおよび `maxResponseBytesExceeded` を `DioExceptionType` へ一貫してマッピングし、キャンセル・タイムアウト・TLS エラー・サイズ上限超過・非 200 系ステータスの挙動を検証する。

---

## Phase 3: テスト・パッケージ品質整備・プライベート導入（非公開運用）

- [x] **3-1. 自動テスト（Unit Test / Integration Test）およびパフォーマンス計測の整備**
  - Dart 層の単体機能テスト（アダプタ入出力、エラーマッピング、`maxResponseBytes`、`TurboTransformer` の全 `ResponseType`・`TurboOptions(mapper: ...)`・閾値分岐・セマフォ制御・単一 `Dio` インスタンス並列非干渉）を整備する。
  - Dart 層マイクロベンチマーク（`test/turbo_benchmark_test.dart`: Dio 標準 `BackgroundTransformer` vs `TurboTransformer` + `TurboOptions(mapper)` の変換速度・Main Isolate イベントループ最大遅延・RSS メモリ比較）と、`example/` の `integration_test`（ネイティブ実通信の機能検証＋4プリセット並列ベンチマーク）を整備する。
- [x] **3-2. 導入ドキュメント（`README.md`）と設定・ベンチマークガイドの作成**
  - `path:` / `git:` 導入手順、Android R8 / iOS ATS 注意事項、各チューニングパラメータ・`TurboOptions` の利用ガイド、およびパフォーマンス計測（単体ベンチマーク / 実機ベンチマーク）の実行手順を作成する。

---

## Phase 4: パフォーマンス比較ベンチマーク基盤の実装と性能実証

[`docs/architectures/benchmark.md`](architectures/benchmark.md) に基づき、実用的な3種類のシナリオ（計測A・B・C）を通じて4つのアダプタ構成の性能差・24KiB閾値の妥当性・多重化耐性・メモリ消費を精密に検証・証明するフェーズ。

> **Phase 3-1 との位置づけの違い**: Phase 3-1 では品質担保としての単体マイクロベンチマークおよび単一固定ワークロード（100KiB等）でのPoC並列検証を整備した。本 Phase 4 では、実アプリの画面遷移や連続通信を模した本格的な実用ワークロード（サイズ別スイープ×10周、計測A・B・C）を対象とし、通信時間とパース時間の分離計測、24KiB閾値の妥当性評価、多重化時の劣化耐性検証を完結させる。

- [x] **4-1. 計測コア・メトリクス分離エンジンおよび並列キューの実装**
  - 4プリセット共通の Net Time（通信完了）/ Parse Time（パース完了）分離計測ラッパー・インターセプターを構築する。
  - 指定並列数（3 / 5 / 10）制御付きのスイープ（全サイズ順次 × 10周）実行キューエンジンを実装する。
  - UIフレーム遅延（`FrameTiming`）およびプロセスRSSメモリ（`ProcessInfo.currentRss`）の監視ロジックを統合する。
- [ ] **4-2. `example/` UI への組み込みと Markdown エクスポート機能の実装**
  - `example/lib/main.dart` に計測シナリオA / B / C および個別・4構成一括実行UIを実装する。
  - 実行中のリアルタイム進捗表示および完了後の比較テーブル表示を実装する。
  - 結果の Markdown テーブル生成、コンソール出力（`debugPrint`）、クリップボードコピー機能を実装する。
- [ ] **4-3. ベンチマーク計測の実施と性能評価レポート（結果検証）の作成**
  - HTTP/2 モックサーバーと連携し、計測A・B・Cを4プリセットで実行する。
  - 得られた実測結果（Markdownテーブル）を [`docs/architectures/benchmark.md`](architectures/benchmark.md) に記録する。
  - 24KiB閾値の妥当性（50KiB未満差分）、多重リクエスト時の劣化抑制、メモリ消費量（RSS）の安定性を分析・評価する。
