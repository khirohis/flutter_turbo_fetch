# 開発ロードマップ・履歴

本プロジェクト（`flutter_turbo_fetch`）の段階的な開発フェーズと達成履歴。各層の現行仕様は [`specs/architectures/`](architectures/) を参照。

---

## Phase 0: 事前設計・アーキテクチャ確定

- [x] **0-1. 既存パッケージ評価**: `rhttp` 等との比較検証を経て、OS 標準/軽量スタックによるネイティブ一括受信プラグインの自作方針を確定（[`docs/design-rationale.md`](../docs/design-rationale.md)）。
- [x] **0-2. Dart 層責務定義**: `TurboHttpAdapter` / `TurboTransformer` / `TurboOptions` の責務と 4 種の `ResponseType` 対応仕様を確定。
- [x] **0-3. 通信ブリッジ選定**: Pigeon（`StandardMessageCodec` ベースの型安全コード生成、`@HostApi` + `@async`）を採用。
- [x] **0-4. 初期ターゲット定義**: Flutter `>=3.24.0`, Dart `^3.5.0`, `dio: ^5.4.0`, Android `minSdk = 24`（`okhttp:4.12.0`）, iOS `13.0+` に確定。

## Phase 1: Plugin 雛形構築 & 最小パイプライン実証（PoC）

- [x] **1-1. プロジェクト雛形構築**: 単一パッケージ構成および Pigeon コード生成基盤を構築。
- [x] **1-2. ベンチマーク環境準備**: HTTP/2 モックサーバー（`example/server/`）と比較ベンチマークアプリ（`example/`）を実装。
- [x] **1-3. Android 基本実装**: `OkHttp3` 共有クライアントによるバックグラウンド一括受信を実装。
- [x] **1-4. iOS 基本実装**: `URLSession.dataTask(completionHandler:)` による一括受信を実装。
- [x] **1-5. パイプライン結合検証**: ネイティブ一括受信〜ゼロコピー Isolate 変換を結合し、既存アダプタに対する速度・UI スムーズネスの優位性を実証。

## Phase 2: プロダクション機能の本実装

- [x] **2-1. Pigeon スキーマ拡張**: `maxResponseBytes`・`errorCode`・`cancelRequest(requestId)` 等を含む本番スキーマを確定。
- [x] **2-2. Android プロダクション対応**: 3 層クラス分離、`maxResponseBytes` 上限チェック、3 フェーズタイムアウト分類、Engine デタッチ時クリーンアップを実装。
- [x] **2-3. iOS プロダクション対応**: 3 層クラス分離、`maxResponseBytes` 上限チェック、フェーズ別タイマー、Engine デタッチ時クリーンアップを実装。
- [x] **2-4. Dart API 仕上げ**: `TurboOptions<T>`（型安全ファクトリ `.json` / `.bytes` / `.plain` 含む）、`ResponseType.stream` 単一チャンク統一、動的デフォルトセマフォ制御を実装。
- [x] **2-5. エラーマッピング統一**: ネイティブ共通エラーコードと `DioExceptionType` の一貫したマッピングおよび異常系検証を完了。

## Phase 3: テスト・パッケージ品質・公開基盤整備

- [x] **3-1. 自動テスト & マイクロベンチマーク整備**: Dart 層・各 OS ネイティブ層の単体テスト、Dart マイクロベンチマーク、結合テストを整備。
- [x] **3-2. ドキュメント整備 & 言語分離**: `specs/`（日本語内部仕様）と `docs/`（英語技術リファレンス）の分離、`README.md` 整備、`pub.dev` 公開設定（v0.0.1 リリース）を完了。

## Phase 4: パフォーマンス比較ベンチマーク基盤の確立

- [x] **4-1. 分離計測エンジン実装**: Net Time / Parse Time 分離計測ラッパー、並列スイープ実行エンジン、Jank（`FrameTiming`）およびメモリ（RSS）監視を実装。
- [x] **4-2. ベンチマーク UI & エクスポート**: シナリオ A / B / C の一括比較実行 UI および Markdown テーブル出力機能を実装。
- [x] **4-3. 実測レポート作成**: 実機・デスクトップでの計測結果および技術分析を [`docs/benchmark-results.md`](../docs/benchmark-results.md) および `docs/benchmarks/` に体系化。

## Phase 5: デスクトップ対応拡張（Windows / macOS / Linux）

- [x] **5-1. Windows サポート追加（v0.0.2）**: C++17 `WinHTTP`（HTTP/2 共有セッション ＋ OS スレッドプール）による 3 層ネイティブ実装、C++ 単体テスト、Windows ベンチマーク実測レポートを追加。
- [x] **5-2. macOS サポート追加 & Darwin 共通化（v0.0.3）**: `sharedDarwinSource: true` による `darwin/` 単一ソース化（CocoaPods / SPM 両対応）を実施し、iOS `13.0+` / macOS `10.15+` 共通の `URLSession` 実装および XCTest を整備。
- [x] **5-3. Linux サポート追加（v0.0.4）**: Pigeon GObject コード生成、C++17 `libcurl`（HTTP/2 共有接続 ＋ ワーカースレッドプール）による 3 層ネイティブ実装、CMake ビルド定義、C++ 単体テスト（WSL2 / Linux 共通）、Linux ベンチマーク実測レポートを追加。
