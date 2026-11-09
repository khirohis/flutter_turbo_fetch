# flutter_turbo_fetch — Agent Instructions

## 1. プロジェクト概要

`flutter_turbo_fetch` は、Flutter（`package:dio`）向け **ネイティブ層 HTTP/2 一括受信 & ゼロコピー変換対応の Flutter Plugin** です。

- **ネイティブ層**: HTTP/2 通信およびチャンク結合を OS バックグラウンドで完結させ、完成した単一の生バイト配列（`Uint8List`）を Dart へ返却する。
- **Dart 層**: 一括受領したバイト配列を、サイズやデータ形式に応じてメインスレッドまたはバックグラウンド Isolate でゼロコピーかつ高速にパース・マッピングする。

---

## 2. 対象プラットフォーム & 技術スタック

- **対象プラットフォーム**:
  - Android: `minSdk = 24`（Android 7.0+）
  - iOS: `13.0+` / macOS: `10.15+`（`sharedDarwinSource: true` による `darwin/` 共通実装、CocoaPods / Swift Package Manager 両対応）
  - Windows: Windows 10 バージョン 1809（Build 17763）以降（64-bit: x64 / ARM64）
  - ※ Web / Linux は非サポート（`UnsupportedError`）
- **主要技術スタック**:
  - **Dart / Flutter**: Flutter `>=3.24.0`, Dart `^3.5.0`, `dio: ^5.4.0`
  - **通信ブリッジ**: `pigeon`（型安全コード生成・`StandardMessageCodec`）
  - **Android**: Kotlin, `com.squareup.okhttp3:okhttp:4.12.0`（HTTP/2 共有クライアント）
  - **iOS / macOS (Darwin)**: Swift, `URLSession`（CFNetwork completionHandler 一括受信）
  - **Windows**: C++17, `WinHTTP`（`winhttp.dll` HTTP/2 共有セッション + OS スレッドプール）

---

## 3. コーディング原則（意思決定の優先順位）

トレードオフが生じた際は、常に以下の優先順位に従って判断すること。
**【 安全性（Safety） ＞ シンプルさ（KISS） ＞ メモリ効率（Memory） ＞ CPU効率（CPU） 】**

1. **安全性の最優先（Defensive Programming）**:
   - フェイルセーフ（上限サイズ超過防止・証明書検証厳格化）、厳格な型安全（Pigeon 遵守）、確実なリソース解放（リークゼロ）を徹底する。
2. **シンプルさの維持（KISS 原則 & 驚き最小の原則）**:
   - 独自の複雑な仕組みを作らず OS・言語標準に素直に乗せる。Dio 標準の挙動やエラー体系に忠実であり、利用側に驚きを与えない。
3. **メモリ効率の優先（Avoid Premature Optimization）**:
   - わずかな CPU サイクル短縮のためにメモリを犠牲にしない。中間バッファの排除とゼロコピーによるメモリフットプリント抑制を優先する。

---

## 4. 設計上の絶対原則（アンチパターン防止）

1. **ネイティブ層でのデータパース禁止**: JSON パース等のデータ加工はネイティブで行わず、生のバイト配列（`Uint8List`）の一括結合・返却に徹する。
2. **チャンク毎の Dart 転送禁止**: 途中チャンクを細切れに Dart へ送らず、OS 側で受信完了までバッファリングして 1 リクエストにつき 1 回のみ返却する。
3. **常駐ワーカープールの禁止**: Dart の並列処理では常駐ワーカー（`SendPort` によるディープコピー）を避け、`Isolate.run` によるゼロコピー転送を採用する。

---

## 5. ドキュメント構成（`specs/` & `docs/`）

本プロジェクトでは、「AI・メンテナー向けの作業場（[`specs/`](specs/README.md)）」と「利用者向けの公開リファレンス（[`docs/`](docs/README.md)）」をルートレベルで明確に分離しています。各ファイルの一覧と詳細はそれぞれのインデックスを参照してください。

- **内部仕様・管理（[`specs/README.md`](specs/README.md)）— 日本語正本**:
  - 開発ロードマップ（[`specs/roadmap.md`](specs/roadmap.md)）、運用規約（`specs/guidelines/`）、アーキテクチャ仕様（`specs/architectures/`）。
- **公開リファレンス（[`docs/README.md`](docs/README.md)）— 英語正本**:
  - 設計根拠ホワイトペーパー、リファレンス実装、マルチプラットフォーム比較ベンチマーク実測レポート。

---

## 6. 開発・リファクタリングの標準サイクル & 運用規約

機能追加やリファクタリングを行う際は、必ず以下の **3ステップのサイクル** を順に回すこと。
**各ステップの完了時は必ずユーザーのレビューを受け、ユーザーからの合図（承認・進行指示）を待ってから次のステップへ進むこと。勝手に次のステップへ進んではならない。**

1. **ドキュメント修正（最重要ディスカッション）**:
   - ユーザーとディスカッションを重ねて仕様（`specs/architectures/*.md` 等）を固め、ドキュメント修正についてレビュー・合意を得る。
2. **テストコード修正**:
   - 合意された仕様に基づきテストコード（単体・境界値・異常系）を追加・修正し（この時点では失敗してよい）、レビュー・合意を得る。
3. **コード修正**:
   - テストをパスさせるようにプロダクションコードを実装し、全テスト合格を確認してレビューを受ける。

### 開発進行時の共通ルール

- **事前確認の徹底**: 実装前に判断が必要な選択肢がある場合は、コードを書く前に必ずユーザーへ質問して方針を確定させる。
- **FVM の使用**: 本リポジトリは FVM（`.fvmrc`）でバージョンを固定しているため、必ず `fvm flutter ...` および `fvm dart ...` を使用すること。
- **Git ブランチ・リリース運用**: [`specs/guidelines/branch-strategy.md`](specs/guidelines/branch-strategy.md) に厳格に従うこと（`main` 直接コミット禁止、目的別ブランチでの作業、`--no-ff` マージ必須）。
- **言語運用ポリシー**: [`specs/guidelines/language-policy.md`](specs/guidelines/language-policy.md) に厳格に従うこと（ユーザー対話および `specs/` は**日本語**、コード・テスト・コミット・公開ドキュメント等の成果物は**英語 [US English]**）。
