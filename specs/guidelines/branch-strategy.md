# ブランチ戦略・リリース運用ガイドライン（`branch-strategy.md`）

本ドキュメントは、Git ブランチ戦略および `pub.dev` リリース時の品質ゲートを規定する。

---

## 1. 基本方針

1. **`main` 1 本化（GitHub Flow 拡張型）**: `main` を唯一の安定ブランチ（常にテスト全パス・リリース可能状態）とし、`develop` ブランチは設けない。
2. **直接コミット禁止**: すべての変更は目的別の作業ブランチから行う。
3. **`--no-ff` マージ必須**: 履歴可視化とトレーサビリティのため、`main` への統合時は例外なく `--no-ff` でマージコミットを残す（Fast-forward 禁止）。
4. **ブランチの速やかな破棄**: マージ完了後、作業ブランチは削除する。

---

## 2. ブランチ体系

| ブランチ種別 | 命名規則 | 用途 |
| :--- | :--- | :--- |
| **Main** | `main` | リリース可能安定ブランチ（保護対象） |
| **Feature** | `feature/<name>` | 新機能の実装 |
| **Fix** | `fix/<name>` | バグ修正 |
| **Refactor** | `refactor/<name>` | 外部仕様を変えない内部構造の改善 |
| **Docs** | `docs/<name>` | ドキュメント・内部仕様書の更新 |
| **Chore / CI** | `chore/<name>` / `ci/<name>` | CI/CD・依存関係・パッケージ設定等の整備 |
| **Release** | `release/vX.Y.Z` | バージョン更新・公開準備専用（機能変更不可） |
| **Hotfix** | `hotfix/vX.Y.Z` | 公開済みバージョンへの緊急パッチ |

---

## 3. 日常の開発マージ手順

1. 最新の `main` から作業ブランチを作成し、標準開発サイクル（ドキュメント → テスト → コード）に沿って Conventional Commits 形式でコミットする。
2. マージ前品質ゲートを通過させる：
   - `fvm flutter test`: 全テスト合格
   - `fvm dart analyze`: 警告・エラー 0 件
3. `main` へ `--no-ff` マージし、作業ブランチを削除する。

---

## 4. リリース運用フロー（`pub.dev` 公開）

1. **準備ブランチ作成**: `release/vX.Y.Z`（緊急時は `hotfix/vX.Y.Z`）を作成し、`pubspec.yaml` の `version` 更新と `CHANGELOG.md` の追記を行う。
2. **公開前品質ゲート**:
   - `fvm flutter test`: 全テスト合格
   - `fvm dart pub publish --dry-run`: **警告 0 件** の確認
3. **統合 & タグ付け**: `main` へ `--no-ff` マージし、マージコミットに Git タグ（`vX.Y.Z`）を付与してリモートへプッシュ（`--tags`）する。
4. **公開 & 完了**: CI/CD または `fvm dart pub publish` で公開し、準備ブランチを削除する。
