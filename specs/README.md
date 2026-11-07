# flutter_turbo_fetch 内部仕様・開発管理 (`specs/`)

このディレクトリは、`flutter_turbo_fetch` プロジェクトの **AI エージェントおよびメンテナー向けの内部仕様、開発ガイドライン、および進行管理ロードマップ（日本語正本）** を配置する作業領域です。

プロジェクト全体の横断ルールについては、ルートの [`AGENTS.md`](../AGENTS.md) を参照してください。  
利用者向けの公開技術資料・リファレンス（英語）については、[`../docs/`](../docs/README.md) を参照してください。

---

## 構成一覧

### 1. 開発計画・ロードマップ

- [`roadmap.md`](roadmap.md): 全 5 フェーズ（Phase 0〜Phase 4）の開発ステップ一覧と進行管理チェックリスト

### 2. 開発ガイドライン・運用規約 (`guidelines/`)

- [`guidelines/language-policy.md`](guidelines/language-policy.md): 言語運用ポリシー（制御系＝日本語、成果物系＝英語）
- [`guidelines/branch-strategy.md`](guidelines/branch-strategy.md): ブランチ戦略・リリース運用ガイドライン（GitHub Flow 拡張型、pub.dev リリース手順）

### 3. アーキテクチャ仕様 (`architectures/`) — What / How

- [`architectures/architecture.md`](architectures/architecture.md): 一気通貫パイプライン構造と各層の責務境界
- [`architectures/native-layer.md`](architectures/native-layer.md): 通信ブリッジ仕様（Pigeon）と共通データモデル・エラーコード体系
- [`architectures/android-native-layer.md`](architectures/android-native-layer.md): Android ネイティブ層仕様（OkHttp3 / クラス構成）
- [`architectures/darwin-native-layer.md`](architectures/darwin-native-layer.md): iOS / macOS（Darwin）ネイティブ層仕様（URLSession / クラス構成）
- [`architectures/windows-native-layer.md`](architectures/windows-native-layer.md): Windows ネイティブ層仕様（WinHTTP / クラス構成）
- [`architectures/dart-layer.md`](architectures/dart-layer.md): `TurboHttpAdapter`・`TurboTransformer`・`TurboOptions` の仕様
- [`architectures/benchmark.md`](architectures/benchmark.md): パフォーマンス比較ベンチマーク基盤の仕様および実装プロンプト

### 4. 機能別仕様 (`features/`)

- 個別機能の詳細仕様・プロンプトを配置（現在未使用・将来の拡張用）。
