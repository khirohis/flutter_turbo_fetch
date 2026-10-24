# flutter_turbo_fetch ドキュメント (`docs/`)

このディレクトリには、`flutter_turbo_fetch` プロジェクトのアーキテクチャ仕様、機能別仕様、および設計背景・リファレンス資料を配置します。
プロジェクト全体の横断ルールについては、ルートの [`AGENTS.md`](../AGENTS.md) を参照してください。

## ドキュメント一覧

### 開発計画・ロードマップ

- [`roadmap.md`](roadmap.md): 全 4 フェーズ（Phase 0〜Phase 3）の開発ステップ一覧と進行管理チェックリスト

### アーキテクチャ仕様 (`architectures/`) — What / How

- [`architectures/architecture.md`](architectures/architecture.md): 一気通貫パイプライン構造と各層の責務境界
- [`architectures/native-layer.md`](architectures/native-layer.md): Android（`OkHttp3`）・iOS（`URLSession`）のネイティブ層仕様と通信データモデル
- [`architectures/dart-layer.md`](architectures/dart-layer.md): Dio `HttpClientAdapter` 準拠スコープ・フォールバック仕様・`ZeroCopyJsonTransformer` 仕様

### 機能別仕様 (`features/`)

- 今後追加する個別機能の詳細仕様・プロンプトを配置します。

### 設計背景・リファレンス資料 (`references/`) — Why / Reference（オンデマンド参照）

- [`references/design-rationale.md`](references/design-rationale.md): 解決すべき 3 大ボトルネック、`pub.dev` 既存パッケージ評価、設計 4 原則の技術的根拠、通信ブリッジ比較背景
- [`references/zero-copy-json-transformer.md`](references/zero-copy-json-transformer.md): `ZeroCopyJsonTransformer` の設計解説およびリファレンス実装コード
