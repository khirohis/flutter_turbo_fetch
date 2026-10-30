# flutter_turbo_fetch — Agent Instructions

## 1. プロジェクト概要

`flutter_turbo_fetch` は、Flutter（`package:dio`）向け **ネイティブ層 HTTP/2 一括受信 & ゼロコピー変換対応の Flutter Plugin** です。

- **ネイティブ層**: HTTP/2 通信およびチャンク結合を OS バックグラウンドで完結させ、完成した単一の生バイト配列（`Uint8List`）を Dart へ返却する。
- **Dart 層**: 一括受領したバイト配列を、サイズやデータ形式に応じてメインスレッドまたはバックグラウンド Isolate でゼロコピーかつ高速にパース・マッピングする。

---

## 2. 対象プラットフォーム & 技術スタック

- **対象プラットフォーム**:
  - Android: `minSdk = 24`（Android 7.0+）
  - iOS: `13.0+`（CocoaPods / Swift Package Manager 両対応）
  - ※ Web / Desktop は非サポート（`UnsupportedError`）
- **主要技術スタック**:
  - **Dart / Flutter**: Flutter `>=3.24.0`, Dart `^3.5.0`, `dio: ^5.4.0`
  - **通信ブリッジ**: `pigeon`（型安全コード生成・`StandardMessageCodec`）
  - **Android**: Kotlin, `com.squareup.okhttp3:okhttp:4.12.0`（HTTP/2 共有クライアント）
  - **iOS**: Swift, `URLSession`（CFNetwork completionHandler 一括受信）

---

## 3. コーディング原則（意思決定の優先順位）

トレードオフが生じた際は、常に以下の優先順位に従って判断すること。
**【 安全性（Safety） ＞ シンプルさ（KISS） ＞ メモリ効率（Memory） ＞ CPU効率（CPU） 】**

1. **安全性の最優先（Defensive Programming）**:
   - フェイルセーフ（上限サイズ超過防止・証明書検証厳格化）、厳格な型安全（Pigeon 遵守）、確実なリソース解放（リークゼロ）を徹底する。
2. **シンプルさの維持（KISS 原則 & 最小驚きの原則）**:
   - 独自の複雑な仕組みを作らず OS・言語標準に素直に乗せる。Dio 標準の挙動やエラー体系に忠実であり、利用側に驚きを与えない。
3. **メモリ効率の優先（Avoid Premature Optimization）**:
   - わずかな CPU サイクル短縮のためにメモリを犠牲にしない。中間バッファの排除とゼロコピーによるメモリフットプリント抑制を優先する。

---

## 4. 設計上の絶対原則（アンチパターン防止）

1. **ネイティブ層でのデータパース禁止**: JSON パース等のデータ加工はネイティブで行わず、生のバイト配列（`Uint8List`）の一括結合・返却に徹する。
2. **チャンク毎の Dart 転送禁止**: 途中チャンクを細切れに Dart へ送らず、OS 側で受信完了までバッファリングして 1 リクエストにつき 1 回のみ返却する。
3. **常駐ワーカープールの禁止**: Dart の並列処理では常駐ワーカー（`SendPort` によるディープコピー）を避け、`Isolate.run` によるゼロコピー転送を採用する。

---

## 5. ドキュメント構成（`docs/`）

- **[`docs/roadmap.md`](docs/roadmap.md)**: 開発ステップ一覧と進行チェックリスト。
- **`docs/architectures/`**（設計・実装時に参照する仕様定義）:
  - [`architecture.md`](docs/architectures/architecture.md): 全体パイプライン構造と各層の責務境界。
  - [`native-layer.md`](docs/architectures/native-layer.md): Android / iOS ネイティブ層仕様と通信データモデル。
  - [`dart-layer.md`](docs/architectures/dart-layer.md): `TurboHttpAdapter`・`TurboTransformer`・`TurboOptions` の仕様。
  - [`benchmark.md`](docs/architectures/benchmark.md): パフォーマンス比較ベンチマーク基盤の仕様および実装プロンプト。
- **`docs/features/`**: 個別機能の詳細仕様（現在未使用・将来の拡張用）。
- **`docs/references/`**（通常時は参照不要・必要時のみオンデマンド参照）:
  - [`design-rationale.md`](docs/references/design-rationale.md): 課題背景・3大ボトルネック・既存パッケージ評価・設計原則の技術的根拠。
  - [`turbo-transformer.md`](docs/references/turbo-transformer.md): `TurboTransformer` & `TurboOptions` のリファレンス実装コード。
  - [`benchmark-results.md`](docs/references/benchmark-results.md): Android エミュレータ環境におけるパフォーマンス比較ベンチマーク実測結果と技術分析。

---

## 6. 開発・リファクタリングの標準サイクル

機能追加やリファクタリングを行う際は、必ず以下の **3ステップのサイクル** を順に回すこと。
**各ステップの完了時は必ずユーザーのレビューを受け、ユーザーからの合図（承認・進行指示）を待ってから次のステップへ進むこと。勝手に次のステップへ進んではならない。**

1. **ドキュメント修正（最重要ディスカッション）**:
   - 時間と労力をかけ、ユーザーとじっくりディスカッションを重ねて設計思想・仕様（`docs/architectures/*.md` 等）を固める。
   - 仕様・挙動・入出力・エラーハンドリングのドキュメント修正を行い、ユーザーのレビュー・合意を得る。
2. **テストコード修正**:
   - 合意されたドキュメント仕様に基づき、テストコード（単体テスト・境界値・異常系）を追加・修正する（この時点ではテストが失敗してもよい）。
   - テスト修正内容を提示してユーザーのレビューを受け、合図を待つ。
3. **コード修正**:
   - テストをパスさせるようにプロダクションコード（Dart / Kotlin / Swift）を修正・実装し、テスト全パスを確認する。
   - 実装内容とテスト結果を提示し、ユーザーのレビュー OK をもってサイクルを完結させる。

### 開発進行時の共通ルール

- **実装前に判断が必要な選択肢がある場合は、コードを書く前に必ずユーザーへ質問して方針を確定させる。**
- **Flutter / Dart コマンドの実行**: 本リポジトリは FVM（`.fvmrc`）でバージョンを固定しているため、コマンド実行時はグローバルの `flutter` / `dart` を直接呼ばず、必ず `fvm flutter ...` および `fvm dart ...` を使用すること。
