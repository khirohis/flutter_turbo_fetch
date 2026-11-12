# テスト方針・品質ガイドライン（`testing-policy.md`）

本ドキュメントは、`flutter_turbo_fetch` におけるテストコードの設計方針、モック境界、および品質評価基準を規定する。

---

## 1. 基本方針

本プロジェクトの原則 **【 安全性（Safety） ＞ シンプルさ（KISS） ＞ メモリ効率（Memory） ＞ CPU効率（CPU） 】** に基づき、以下を遵守する。

1. **契約の検証（Contract over Implementation）**:
   - 内部実装の詳細ではなく、公開 API（`TurboHttpAdapter`, `TurboTransformer`, `TurboOptions`）および Pigeon 通信ブリッジの入出力・例外・ライフサイクル契約を検証する。
2. **過度なモックの排除**:
   - モック境界は Dart ⇄ ネイティブ間の境界（`TurboFetchHostApi`）に限定し、Dart 内部ロジックの過剰なモック化（脆いテスト）を禁止する。
3. **英語での記述**:
   - [`language-policy.md`](./language-policy.md) に従い、テストコード、テスト名（`test` / `group`）、およびコード内コメントはすべて **英語（US English）** で記述する。

---

## 2. テストの階層と責務境界

| 階層 | 対象ディレクトリ | モック方針 | 主な検証責務 |
| :--- | :--- | :--- | :--- |
| **Dart 層単体テスト** | `test/` | Pigeon `TurboFetchHostApi` を Fake 化 | リクエスト/レスポンス変換、`ResponseType` デコード、Isolate 分岐（24KiB）、エラーマッピング、キャンセル伝播、クローズ状態遷移 |
| **ネイティブ層単体テスト** | `android/`<br>`example/ios/RunnerTests/`<br>`example/macos/RunnerTests/`<br>`windows/test/`<br>`linux/test/` | OS 標準/モック HTTP サーバー等 | 3層責務分離、全 OS 共通不変条件、OS 例外から共通エラーコード（8種）へのマッピング、Engine デタッチ時の解放と復帰 |
| **統合テスト (E2E)** | `example/integration_test/` | 実通信環境 / 実機 / シミュレータ | プラットフォームチャネルを介した実 HTTP/2 通信およびベンチマーク基盤の結合動作 |

### Dart 層テストのファイル構成

1ファイル集中（`flutter_turbo_fetch_test.dart`）を避け、責務単位でファイルを分割・整理する：

- `adapter_test.dart`: `TurboHttpAdapter` のリクエスト/レスポンス変換、キャンセル、ライフサイクル、エラー変換。
- `transformer_test.dart`: `TurboTransformer` の型変換、Isolate 分岐、セマフォ並行制御。
- `options_test.dart`: `TurboOptions` の引数・mapper 型安全性。
- `benchmark_test.dart`: ベンチマーク集計・フォーマット検証。

---

## 3. 網羅性・品質評価基準（Quality Gates）

形骸化しやすい数値目標（行カバレッジ XX% 等）は設定しない。代わりに、**システムの安全性と一貫性を保証する以下の重要パスの網羅** を品質基準とする。

1. **共通エラーコード（8種）のマッピング網羅**:
   - [`native-layer.md`](../architectures/native-layer.md) に規定された 8 種の共通エラーコード（`cancel`, `connectionTimeout`, `sendTimeout`, `receiveTimeout`, `badCertificate`, `maxResponseBytesExceeded`, `connectionError`, `unknown`）が、Dart 側で期待通りの `DioExceptionType` へ変換されること。
2. **全プラットフォーム共通不変条件の網羅**:
   - ① 非同期タスク開始前の同期登録（キャンセル競合防止）
   - ② 正常・エラー・キャンセルいずれの経路でもコールバックが 1 回のみ呼ばれること
   - ③ `maxResponseBytes` の 2 段階検証（ヘッダ時および受信中）
   - ④ Engine デタッチ時の確実な解放と、その後のリクエスト時の自動再生成
3. **Dart 層の変換・分岐境界値**:
   - 4種の `ResponseType`（`stream`, `bytes`, `plain`, `json`）
   - 空ボディ（0 byte）時の挙動
   - `isolateThresholdBytes`（24KiB）境界値（Main Isolate 保持 vs Worker Isolate オフロード）
4. **キャンセルとライフサイクル**:
   - `cancelFuture` 発火時のネイティブへのキャンセル伝播
   - `close({force: false})`（完了待ち）および `close({force: true})`（即時キャンセル）後の状態遷移

---

## 4. テスト実装・精査の実践規約

1. **決定論的同期の徹底（Flaky タイマーの排除）**:
   - `Future.delayed` 等のスリープ・タイマー待ちによる非同期アサーションを禁止する。
   - `scheduleMicrotask`、`Completer`、または同期イベントを用いて 100% 決定論的（Flaky フリー）に待機すること。
2. **仕様書との厳格な照合**:
   - 精査時は [`specs/architectures/*.md`](../architectures/) の各定義表（エラーコード一覧、境界値表、OS 共通不変条件）とテストケースを突き合わせ、仕様の抜け漏れを洗い出すこと。
3. **他モジュールとの責務重複排除**:
   - 単体テストの範囲を超えて、別モジュールですでに結合検証されているロジックを無駄に重複実行しない（単体テストとしての関心事に集中する）。
4. **テストヘルパー・モデルの設計作法**:
   - Fake クラスは標準的な正常系レスポンス（200 OK・空ボディ）をデフォルト挙動とし、テスト側のボイラープレートを排除する。
   - テスト用モデルには `toString()` を必ず実装し、アサーション失敗時の差分ログを明瞭化する。
