# 全体アーキテクチャ仕様

> 設計背景・ボトルネック分析・既存パッケージの比較根拠については [`docs/references/design-rationale.md`](../references/design-rationale.md) を参照。

---

## 1. 一気通貫パイプライン構造

```text
[ネイティブ層: Android (OkHttp3) / iOS (URLSession dataTask completionHandler)]
  ├─ バックグラウンドスレッドで HTTP/2 並列通信・TLS 復号・ウィンドウ管理（OkHttp は初期 16MB ウィンドウ）
  ├─ チャンク結合までをネイティブメモリ上で完結（Dart との途中往復・デリゲート待機ゼロ）
  └─ 完成した byte[] / Data (Uint8List) を 1 リクエストにつき 1 回だけ Dart へ返却
        │
        ▼ (1 回のみのバッファ転送・再シリアライズ不要)
[Dart Main Isolate (TurboHttpAdapter + TurboTransformer)]
  ├─ 完成した単一チャンク Uint8List を BytesBuilder(copy: false) でゼロコピー受領
  ├─ バイト列取得 (ResponseType.bytes): そのまま Uint8List を O(1) ゼロコピー返却
  ├─ 小さいデータ（〜24KB 未満の plain / json）: Main Isolate 上で直接デコード
  └─ 大きいデータ（24KB 以上の plain / json）: TransferableTypedData (O(1) ゼロコピー) で Isolate.run へ転送
        │
        ▼ (セマフォで同時起動数を制限)
[Dart Worker Isolate (Isolate.run)]
  ├─ plain: utf8.decode / json: utf8.decoder.fuse(json.decoder)（中間 String 生成なし）
  └─ Isolate.exit (O(1) ゼロコピー) で Main Isolate へ返却
```

---

## 2. 各層の責務境界

| レイヤー | 担当コンポーネント | 入力 | 出力 | 禁止事項 |
| :--- | :--- | :--- | :--- | :--- |
| **Android 通信層** | `OkHttp3` (`Call.enqueue`) | リクエスト情報 | 結合済み `byte[]`・ステータス・ヘッダ | チャンク毎の通知、ネイティブ JSON パース |
| **iOS 通信層** | `URLSession` (`dataTask(with:completionHandler:)`) | リクエスト情報 | 結合済み `Data`・ステータス・ヘッダ | `NSURLSessionDataDelegate` の使用、ネイティブ JSON パース |
| **Dart Adapter 層** | `TurboHttpAdapter` (`HttpClientAdapter`) | `RequestOptions`, `Stream<Uint8List>?` | `ResponseBody.fromBytes(Uint8List)` | `ResponseType.stream` の無理な一括化（標準 `IOHttpClientAdapter` へフォールバックすること） |
| **Dart Transform 層** | `TurboTransformer` (`BackgroundTransformer`) | 単一チャンク `ResponseBody` | `Uint8List` / `String` / JSON オブジェクト | JSON 変換時の中間 `String` 生成、常駐 Isolate (`SendPort.send`)、無制限な Isolate 起動 |

---

## 3. ディレクトリ構成

```text
flutter_turbo_fetch/
├── lib/
│   ├── flutter_turbo_fetch.dart           # 公開バレルファイル（TurboHttpAdapter, TurboTransformer）
│   └── src/
│       ├── turbo_http_adapter.dart        # Dio HttpClientAdapter 実装
│       ├── turbo_transformer.dart         # Dio BackgroundTransformer 拡張実装
│       └── messages.g.dart                # Pigeon 自動生成コード（Dart 側）
├── pigeons/
│   └── messages.dart                      # Pigeon スキーマ定義（@HostApi）
├── android/
│   └── src/main/kotlin/io/github/khirohis/flutter_turbo_fetch/
│       ├── FlutterTurboFetchPlugin.kt     # Android FlutterPlugin + OkHttp3 実装
│       └── Messages.g.kt                  # Pigeon 自動生成コード（Kotlin 側）
├── ios/
│   ├── flutter_turbo_fetch.podspec        # CocoaPods 定義（iOS 13.0+）
│   └── flutter_turbo_fetch/
│       ├── Package.swift                  # Swift Package Manager 定義（iOS 13.0+）
│       └── Sources/flutter_turbo_fetch/
│           ├── FlutterTurboFetchPlugin.swift # iOS FlutterPlugin + URLSession 実装
│           └── Messages.g.swift           # Pigeon 自動生成コード（Swift 側）
└── example/                               # PoC・ベンチマーク検証アプリ
```
