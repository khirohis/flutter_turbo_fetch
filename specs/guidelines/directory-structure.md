# ディレクトリ構成・配置ガイドライン（`directory-structure.md`）

本ドキュメントは、`flutter_turbo_fetch` における **ルート階層の責務分離** および **各ディレクトリ配下のファイル命名・配置ルール** を規定する。

---

## 1. 基本原則

1. **責務の厳格な分離**:
   - 利用者向け公開リファレンス（`docs/`）、内部仕様・管理（`specs/`）、アプリ実装（`example/`）、支援スクリプト（`tool/`）の境界を厳守する。
2. **生データによるドキュメント汚染の禁止**:
   - `docs/` には利用者が読むためのレポート（Markdown）と表示用画像（`assets/`）のみを配置する。
3. **エントリポイントの一本化（KISS 原則）**:
   - `example/` 内のエントリポイントは `lib/main.dart` 1 本に集約する。
4. **命名規則の統一とフラット性の維持**:
   - 同一カテゴリのファイル・ドキュメントは命名規則を揃え、フラットな配置を維持する。

---

## 2. ルートディレクトリ階層と責務一覧

| ディレクトリ | 主な役割・責務 | 言語 | 配置・命名ルール |
| :--- | :--- | :---: | :--- |
| **`lib/`** | プラグイン本体の Dart 公開 API およびコアロジック | 英語 | Flutter パッケージ標準構造（`src/` 配下に内部実装） |
| **`android/`** | Android ネイティブ実装（Kotlin, OkHttp3） | 英語 | Android プラグイン標準構造 |
| **`darwin/`** | Apple プラットフォーム共通実装（Swift, URLSession） | 英語 | `sharedDarwinSource: true`（iOS / macOS 共通） |
| **`windows/`** | Windows ネイティブ実装（C++17, WinHTTP） | 英語 | CMake ビルド構成 |
| **`linux/`** | Linux ネイティブ実装（C++17, libcurl） | 英語 | CMake ビルド構成 |
| **`pigeons/`** | ネイティブ ↔ Dart 間の型安全通信スキーマ定義 | 英語 | `pigeons/*.dart` |
| **`example/`** | 利用者向けサンプル 兼 ベンチマーク実行 Flutter アプリ | 英語 | エントリポイントは `lib/main.dart` のみ |
| **`docs/`** | **利用者向けの公開技術ドキュメント** | **英語** | 設計根拠、共通まとめ、各 OS 実測レポート |
| **`specs/`** | **メンテナー・AI 向けの作業場・内部仕様書** | **日本語** | 設計仕様（`architectures/`）、運用規約（`guidelines/`） |
| **`tool/`** | 開発・ベンチマーク支援 CLI ツールおよび検証データ | 英語 | `tool/<category>/` 配下に配置 |
| **`test/`** | プラグイン本体の Dart 単体・結合テスト | 英語 | `test/*_test.dart` |

---

## 3. 重要サブディレクトリの配置ルール

- **`docs/benchmarks/`**:
  - 実測レポートは `<platform>.md`（日付なし・フラット）で配置する。
  - レポート用画像は `assets/` に格納する。
- **`specs/`**:
  - 設計仕様は `architectures/<feature>.md`、運用規約は `guidelines/<topic>.md` に集約する。
- **`example/`**:
  - 起動ファイルは `lib/main.dart` 1 本に集約する。実行モードの違いは引数や環境定義で分岐する。
- **`tool/`**:
  - 支援スクリプトは `tool/<category>/`、検証用データは `data/<platform>/` に格納する。

---

## 4. アンチパターン（違反チェックリスト）

変更や PR を作成する際は、以下のアンチパターンに該当していないかを必ず確認すること。

- ❌ **`docs/` に CSV や生ログファイルを置く**:
  - → `tool/benchmark/data/<platform>/` に隔離する。
- ❌ **特定のプラットフォームだけフォルダ階層を深くする**:
  - → `docs/benchmarks/ios/2026-11-14-release.md` のような個別フォルダ・日付ファイル名を禁止し、`docs/benchmarks/ios.md` に統一する。
- ❌ **`example/lib/` に個別の `xxx_main.dart` を生やす**:
  - → `example/lib/main.dart` に一本化し、条件分岐で処理する。
- ❌ **`example/` 内に Python スクリプト等のツールを置く**:
  - → プロジェクトルートの `tool/` に配置する。
- ❌ **特定プラットフォーム専用の小粒度な仕様書を `specs/architectures/` に追加する**:
  - → 既存の共通仕様書（`benchmark.md` 等）に章として統合する。
