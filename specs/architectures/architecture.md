# 全体アーキテクチャ仕様

> 設計背景・ボトルネック分析・既存パッケージの比較根拠については [`docs/design-rationale.md`](../../docs/design-rationale.md) を参照。

---

## 1. 一気通貫パイプライン構造

```text
[ネイティブ層: Android (OkHttp3) / iOS & macOS (URLSession) / Windows (WinHTTP) / Linux (libcurl)]
  ├─ OS バックグラウンドスレッドで HTTP/2 多重化通信・TLS 復号・チャンク結合を完結
  ├─ maxResponseBytes 指定時は上限超過を検知した時点で通信を中断しエラー返却
  └─ 完成した単一の生バイト配列 (Uint8List) を 1 リクエストにつき 1 回のみ Dart へ返却
        │
        ▼ (Pigeon / StandardMessageCodec による 1 往復転送)
[Dart Main Isolate (TurboHttpAdapter + TurboTransformer + TurboOptions)]
  ├─ ResponseBody.fromBytes(bytes) により単一チャンクの Stream<Uint8List> としてラップ
  ├─ 閾値未満または stream / mapper なし bytes: Main Isolate で O(1) 解決
  └─ 閾値以上: TransferableTypedData (O(1) ゼロコピー) で Worker Isolate へ転送
        │
        ▼ (セマフォで同時起動数を制御)
[Dart Worker Isolate (Isolate.run)]
  ├─ 一次デコード (plain: utf8.decode / json: utf8.decoder.fuse(json.decoder))
  ├─ 後処理フック (TurboOptions.mapper): 同一 Isolate 内でドメインモデル等への変換まで完結
  └─ Isolate.exit (O(1) ゼロコピー) で最終成果物のみを Main Isolate へ返却
```

---

## 2. 各層の責務境界

| レイヤー | 担当コンポーネント | 主な責務（入出力） |
| :--- | :--- | :--- |
| **ネイティブ通信層**<br>(Android / Darwin / Windows / Linux) | `FlutterTurboFetchPlugin`<br>`TurboHttpExecutor`<br>`TurboClientManager` / `TurboSessionManager` | リクエスト情報を受け取り、OS バックグラウンドで一括受信した結合済みバイト配列・ステータス・ヘッダを返す |
| **Dart Adapter 層** | `TurboHttpAdapter`<br>(`HttpClientAdapter`) | `RequestOptions` とボディストリームをネイティブへ渡し、返却された `Uint8List` を単一チャンクの `ResponseBody` としてラップする |
| **Dart Transform 層** | `TurboTransformer` + `TurboOptions`<br>(`BackgroundTransformer`) | 単一チャンク `ResponseBody` から `ResponseType` に応じた一次デコードおよび `mapper` 後処理を実行する |

---

## 3. ディレクトリ構成

| パス | 役割 |
| :--- | :--- |
| `lib/` | Dart 公開 API（`TurboHttpAdapter`, `TurboTransformer`, `TurboOptions`）および Pigeon 生成コード |
| `pigeons/` | Dart ⇄ ネイティブ間の Pigeon スキーマ定義（`messages.dart`） |
| `android/` | Android ネイティブ層実装（Kotlin / `OkHttp3`）および JUnit 単体テスト |
| `darwin/` | iOS / macOS 共通ネイティブ層実装（Swift / `URLSession`, `sharedDarwinSource`）および XCTest |
| `windows/` | Windows ネイティブ層実装（C++17 / `WinHTTP`）および C++ 単体テスト |
| `linux/` | Linux ネイティブ層実装（C++17 / `libcurl` / CMake）および C++ 単体テスト |
| `common/` | Windows / Linux 共通 C++ 資産（Pigeon 生成コード `messages.g.h` / `.cpp` 等） |
| `test/` | Dart 層の単体テストおよびマイクロベンチマーク |
| `example/` | HTTP/2 モックサーバーおよびマルチプラットフォーム比較ベンチマークアプリ |
