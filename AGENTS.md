# flutter_turbo_fetch — Agent Instructions

## 1. プロジェクト概要

`flutter_turbo_fetch` は、Flutter（`package:dio`）向け **ネイティブ層 HTTP/2 一括受信 & ゼロコピー変換対応の Flutter Plugin** です。

- **Android (`OkHttp3`) / iOS (`URLSession`)**: バックグラウンドスレッドで HTTP/2 通信〜チャンク結合までを完結し、完成した `Uint8List` を 1 リクエストにつき 1 回だけ Dart へ返却する。
- **Dart (`TurboHttpAdapter` + `TurboTransformer`)**: 単一チャンクの `Uint8List` をゼロコピー受領し、バイト列（`bytes`）・テキスト（`plain`）・JSON パースオブジェクト（`json`）の 3 種類へ変換する。`plain` / `json` は閾値（24KB）未満なら Main Isolate、閾値以上なら `TransferableTypedData` + セマフォ付き `Isolate.run`（`Isolate.exit`）で往復ゼロコピー変換する。

---

## 2. 設計上の絶対原則（アンチパターン防止）

1. **ネイティブ側（Kotlin/Swift）で JSON パースを行わない**: 生のバイト配列（`Uint8List`）の一括結合・返却のみを行う。
2. **Android は `OkHttp`（`com.squareup.okhttp3:okhttp`）を採用する**: `response.body.bytes()` で一括結合する。
3. **iOS は `NSURLSessionDataDelegate` を使わず Completion Handler API を使う**: `dataTask(with:completionHandler:)` で一括取得する。
4. **Dart の Isolate オフロードは「常駐ワーカープール（`SendPort.send`）」ではなく「セマフォ付き `Isolate.run`」を使う**: 行きは `TransferableTypedData`、帰りは `Isolate.exit` による O(1) ゼロコピーとする。

---

## 3. ドキュメント構成（`docs/`）

- **[`docs/roadmap.md`](docs/roadmap.md)**: 全 4 フェーズの開発ステップ一覧と進行チェックリスト。
- **`docs/architectures/`**（設計・実装時に参照する仕様定義）:
  - [`architecture.md`](docs/architectures/architecture.md): 全体パイプライン構造と各層の責務境界。
  - [`native-layer.md`](docs/architectures/native-layer.md): Android / iOS ネイティブ層仕様と通信データモデル。
  - [`dart-layer.md`](docs/architectures/dart-layer.md): `TurboHttpAdapter` と `TurboTransformer` の仕様。
- **`docs/features/`**: 個別機能の詳細仕様。
- **`docs/references/`**（通常時は参照不要・必要時のみオンデマンド参照）:
  - [`design-rationale.md`](docs/references/design-rationale.md): 課題背景・3大ボトルネック・既存パッケージ評価・設計原則の技術的根拠。
  - [`turbo-transformer.md`](docs/references/turbo-transformer.md): `TurboTransformer` のリファレンス実装コード。

---

## 4. 開発ステップ進行時のルール

ユーザーから **「ステップ X-X を進めます」** 等の指示があった場合は、以下の手順で対応すること。

1. [`docs/roadmap.md`](docs/roadmap.md) の該当ステップ要件と関連する `docs/architectures/*.md`（必要に応じて `docs/references/*.md`）を確認する。
2. 該当ステップの設計案提示、確認事項の質問、または実装コード・検証手順の作成を行う。
3. **実装前に判断が必要な選択肢がある場合は、コードを書く前に必ずユーザーへ質問して方針を確定させる。**
4. ステップ完了時は、[`docs/roadmap.md`](docs/roadmap.md) のチェックリストや関連設計書を更新する。
