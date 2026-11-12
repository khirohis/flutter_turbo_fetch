# flutter_turbo_fetch 内部仕様・開発管理 (`specs/`)

このディレクトリは、`flutter_turbo_fetch` プロジェクトの **AI エージェントおよびメンテナー向けの内部仕様、開発ガイドライン、および開発履歴（日本語正本）** を配置する作業領域です。

プロジェクト全体の横断ルールについては、ルートの [`AGENTS.md`](../AGENTS.md) を参照してください。  
利用者向けの公開技術資料・リファレンス（英語）については、[`../docs/`](../docs/README.md) を参照してください。

---

## 構成一覧

### 1. 開発ロードマップ・履歴

- [`roadmap.md`](roadmap.md): Phase 0〜Phase 5 の開発ステップと達成履歴

### 2. 開発ガイドライン・運用規約 (`guidelines/`)

- [`guidelines/language-policy.md`](guidelines/language-policy.md): 言語運用ポリシー（制御系＝日本語、成果物系＝英語）
- [`guidelines/branch-strategy.md`](guidelines/branch-strategy.md): ブランチ戦略・リリース運用ガイドライン（GitHub Flow 拡張型、`pub.dev` 公開手順）
- [`guidelines/testing-policy.md`](guidelines/testing-policy.md): テスト方針・品質ガイドライン（階層責務、検証マトリクス、モック方針、実装規約）

### 3. アーキテクチャ仕様 (`architectures/`) — What / Why

- [`architectures/architecture.md`](architectures/architecture.md): 一気通貫パイプライン構造と各層の責務境界
- [`architectures/native-layer.md`](architectures/native-layer.md): 通信ブリッジ仕様（Pigeon）、共通エラーコード、全 OS 共通のネイティブ層不変条件
- [`architectures/android-native-layer.md`](architectures/android-native-layer.md): Android ネイティブ層仕様（OkHttp3 固有設計・例外マッピング）
- [`architectures/darwin-native-layer.md`](architectures/darwin-native-layer.md): iOS / macOS（Darwin）ネイティブ層仕様（URLSession 固有設計・例外マッピング）
- [`architectures/windows-native-layer.md`](architectures/windows-native-layer.md): Windows ネイティブ層仕様（WinHTTP 固有設計・例外マッピング）
- [`architectures/dart-layer.md`](architectures/dart-layer.md): `TurboHttpAdapter`・`TurboTransformer`・`TurboOptions` の仕様
- [`architectures/benchmark.md`](architectures/benchmark.md): パフォーマンス比較ベンチマーク基盤の仕様

### 4. 機能別仕様 (`features/`)

- 個別機能の詳細仕様を配置（現在未使用・将来の拡張用）。
