# flutter_turbo_fetch — Agent Instructions

## 1. プロジェクト概要

`flutter_turbo_fetch` は、Flutter（`package:dio`）向け **ネイティブ層 HTTP/2 一括受信 & ゼロコピー変換対応の Flutter Plugin** です。

- **Android (`OkHttp3`) / iOS (`URLSession`)**: バックグラウンドスレッドで HTTP/2 通信〜チャンク結合までを完結し、完成した `Uint8List` を 1 リクエストにつき 1 回だけ Dart へ返却する（`maxResponseBytes` 指定時は上限超過を検知した時点で処理を中断しエラー返却）。
- **Dart (`TurboHttpAdapter` + `TurboTransformer` + `TurboOptions`)**: 単一チャンクの `Uint8List` をゼロコピー受領し、Dio 標準の 4 種類（`stream` / `bytes` / `plain` / `json`）および `TurboOptions(mapper: ...)` 指定時のカスタムオブジェクトへ変換する。閾値（24KiB）未満は Main Isolate、以上は `TransferableTypedData` + セマフォ付き `Isolate.run`（`Isolate.exit`）で往復ゼロコピー変換する。

---

## 2. 設計上の絶対原則（アンチパターン防止）

1. **ネイティブ側（Kotlin/Swift）で JSON パースを行わない**: 生のバイト配列（`Uint8List`）の一括結合・返却のみを行う。
2. **Android は `OkHttp`（`com.squareup.okhttp3:okhttp`）を採用する**: `Okio` バッファで一括結合する。
3. **iOS は `NSURLSessionDataDelegate` を使わず Completion Handler API を使う**: `dataTask(with:completionHandler:)` で一括取得する。
4. **Dart の Isolate オフロードは「常駐ワーカープール（`SendPort.send`）」ではなく「セマフォ付き `Isolate.run`」を使う**: 行きは `TransferableTypedData`、帰りは `Isolate.exit` による O(1) ゼロコピーとする。

---

## 3. ドキュメント構成（`docs/`）

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

## 4. 開発・リファクタリングの標準サイクル

機能追加やリファクタリングを行う際は、必ず以下の **3ステップのサイクル** を順に回すこと。

1. **ドキュメント修正**:
   - まず `docs/architectures/*.md` などの設計書・仕様ドキュメントを修正し、仕様・挙動・入出力・エラーハンドリングの定義を確定させる。
2. **テストコード修正**:
   - 修正したドキュメント仕様に基づき、テストコード（単体テスト・境界値・異常系）を追加・修正する（この時点ではテストが失敗してもよい）。
3. **コード修正**:
   - テストをパスさせるようにプロダクションコード（Dart / Kotlin / Swift）を修正・実装し、リファクタリングを完結させる。

### 開発進行時の共通ルール
- **実装前に判断が必要な選択肢がある場合は、コードを書く前に必ずユーザーへ質問して方針を確定させる。**
- **Flutter / Dart コマンドの実行**: 本リポジトリは FVM（`.fvmrc`）でバージョンを固定しているため、コマンド実行時はグローバルの `flutter` / `dart` を直接呼ばず、必ず `fvm flutter ...` および `fvm dart ...` を使用すること。
