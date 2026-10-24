# 全体アーキテクチャ仕様

> 設計背景・ボトルネック分析・既存パッケージの比較根拠については [`docs/references/design-rationale.md`](../references/design-rationale.md) を参照。

---

## 1. 一気通貫パイプライン構造

```text
[ネイティブ層: Android (OkHttp3) / iOS (URLSession Completion or async/await)]
  ├─ バックグラウンドスレッドで HTTP/2 並列通信・TLS 復号・ウィンドウ管理（OkHttp は初期 16MB ウィンドウ）
  ├─ チャンク結合までをネイティブメモリ上で完結（Dart との途中往復・デリゲート待機ゼロ）
  └─ 完成した byte[] / Data (Uint8List) を 1 リクエストにつき 1 回だけ Dart へ返却
        │
        ▼ (1 回のみのバッファ転送・再シリアライズ不要)
[Dart Main Isolate (Dio HttpClientAdapter + ZeroCopyJsonTransformer)]
  ├─ 完成した Uint8List を受領（Dart 側でのチャンク結合ループ不要）
  ├─ 小さいデータ（〜24KB 未満）: Main Isolate 上で直接 utf8.decoder.fuse(json.decoder) により変換
  └─ 大きいデータ（24KB 以上）: TransferableTypedData (O(1) ゼロコピー) で Isolate.run へ転送
        │
        ▼ (セマフォで同時起動数を 2〜3 に制限)
[Dart Worker Isolate (Isolate.run)]
  ├─ utf8.decoder.fuse(json.decoder) で中間 String を作らずバイト列から直接オブジェクト生成
  └─ Isolate.exit (O(1) ゼロコピー) で Main Isolate へ返却
```

---

## 2. 各層の責務境界

| レイヤー | 担当コンポーネント | 入力 | 出力 | 禁止事項 |
| :--- | :--- | :--- | :--- | :--- |
| **Android 通信層** | `OkHttp3` (`Call.enqueue`) | リクエスト情報 | 結合済み `byte[]`・ステータス・ヘッダ | チャンク毎の通知、ネイティブ JSON パース |
| **iOS 通信層** | `URLSession` (`data(for:)` / `completionHandler`) | リクエスト情報 | 結合済み `Data`・ステータス・ヘッダ | `NSURLSessionDataDelegate` の使用、ネイティブ JSON パース |
| **Dart Adapter 層** | Dio `HttpClientAdapter` 実装 | `RequestOptions`, `Stream<Uint8List>?` | `ResponseBody.fromBytes(Uint8List)` | ストリーミングレスポンスの無理な一括化（標準 Adapter へフォールバックすること） |
| **Dart Transform 層** | `ZeroCopyJsonTransformer` | 単一チャンク `ResponseBody` | デコード済み JSON オブジェクト | 中間 `String` 生成（`utf8.decode` 単体呼び出し）、常駐 Isolate (`SendPort.send`)、無制限な Isolate 起動 |
