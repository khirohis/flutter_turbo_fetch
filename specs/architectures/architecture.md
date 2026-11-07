# 全体アーキテクチャ仕様

> 設計背景・ボトルネック分析・既存パッケージの比較根拠については [`docs/design-rationale.md`](../../docs/design-rationale.md) を参照。

---

## 1. 一気通貫パイプライン構造

```text
[ネイティブ層: Android (OkHttp3) / iOS & macOS (URLSession dataTask completionHandler) / Windows (WinHTTP)]
  ├─ バックグラウンドスレッドで HTTP/2 並列通信・TLS 復号・ウィンドウ管理（OkHttp デフォルト 16MiB ウィンドウ / CFNetwork / WinHTTP SChannel）
  ├─ チャンク結合までをネイティブメモリ上で完結（デフォルトでサイズ制限なく最後まで受信、maxResponseBytes 指定時は上限超過を検知した時点で処理を中断しエラー返却）
  └─ 完成した byte[] / Data / std::vector<uint8_t> (Uint8List) を 1 リクエストにつき 1 回だけ Dart へ返却
        │
        ▼ (1 回のみのバッファ転送・再シリアライズ不要)
[Dart Main Isolate (TurboHttpAdapter + TurboTransformer + TurboOptions)]
  ├─ ResponseBody.fromBytes(bytes) により単一チャンクの Stream<Uint8List> としてラップ
  ├─ ストリーム取得 (ResponseType.stream): 単一チャンク Stream を持つ ResponseBody を O(1) 返却（指定時は mapper 実行）
  ├─ バイト列取得 (ResponseType.bytes, mapper なし): BytesBuilder(copy: false) で Uint8List を O(1) ゼロコピー返却
  ├─ 小さいデータ（〜24KiB 未満）: Main Isolate 上で直接デコード（＋指定時は mapper 実行）
  └─ 大きいデータ（24KiB 以上）: TransferableTypedData (O(1) ゼロコピー) で Isolate.run へ転送
        │
        ▼ (セマフォで同時起動数を制限: デフォルト max(2, Platform.numberOfProcessors - 2))
[Dart Worker Isolate (Isolate.run)]
  ├─ 一次デコード (ResponseType に準拠): bytes: rawBytes / plain: utf8.decode / json: utf8.decoder.fuse(json.decoder)（中間 String 生成なし）
  ├─ 追加フック (TurboOptions(mapper: ...)): デコード結果を入力とし、Worker Isolate 内でオブジェクト生成や追加処理を実行
  └─ Isolate.exit (O(1) ゼロコピー) で Main Isolate へ返却
```

---

## 2. 各層の責務境界

| レイヤー | 担当コンポーネント | 入力 | 出力 | 禁止事項 |
| :--- | :--- | :--- | :--- | :--- |
| **Android 通信層** | `FlutterTurboFetchPlugin` + `TurboHttpExecutor` + `TurboClientManager` (`OkHttp3`) | リクエスト情報 | 結合済み `byte[]`・ステータス・ヘッダ | チャンク毎の通知、ネイティブ JSON パース、途中データでの正常リターン |
| **iOS / macOS 通信層** | `FlutterTurboFetchPlugin` + `TurboHttpExecutor` + `TurboSessionManager` (`URLSession` completionHandler) | リクエスト情報 | 結合済み `Data`・ステータス・ヘッダ | `NSURLSessionDataDelegate` の使用、ネイティブ JSON パース、途中データでの正常リターン |
| **Windows 通信層** | `FlutterTurboFetchPlugin` + `TurboHttpExecutor` + `TurboSessionManager` (`WinHTTP`) | リクエスト情報 | 結合済み `std::vector<uint8_t>`・ステータス・ヘッダ | チャンク毎の通知、ネイティブ JSON パース、途中データでの正常リターン |
| **Dart Adapter 層** | `TurboHttpAdapter` (`HttpClientAdapter`) | `RequestOptions`, `Stream<Uint8List>?` | `ResponseBody.fromBytes(Uint8List)`（単一チャンクストリーム） | チャンク分割送信・別アダプタへの不要なフォールバック |
| **Dart Transform 層** | `TurboTransformer` (`BackgroundTransformer`) + `TurboOptions` | 単一チャンク `ResponseBody`, `RequestOptions` | `ResponseType` に応じた一次デコードデータ（`stream`, `bytes`, `plain`, `json`）、または `mapper` フック実行後の変換オブジェクト | JSON 変換時の中間 `String` 生成、常駐 Isolate (`SendPort.send`)、無制限な Isolate 起動 |

---

## 3. ディレクトリ構成

```text
flutter_turbo_fetch/
├── lib/
│   ├── flutter_turbo_fetch.dart           # 公開バレルファイル（TurboHttpAdapter, TurboTransformer, TurboOptions）
│   └── src/
│       ├── turbo_http_adapter.dart        # Dio HttpClientAdapter 実装
│       ├── turbo_transformer.dart         # Dio BackgroundTransformer 拡張実装
│       ├── turbo_options.dart             # リクエスト単位マッパー・上限サイズ指定用 Options 拡張クラス
│       └── messages.g.dart                # Pigeon 自動生成コード（Dart 側）
├── pigeons/
│   └── messages.dart                      # Pigeon スキーマ定義（@HostApi）
├── android/
│   └── src/main/kotlin/io/github/khirohis/flutter_turbo_fetch/
│       ├── FlutterTurboFetchPlugin.kt     # Android FlutterPlugin インターフェース層
│       ├── TurboHttpExecutor.kt           # Android OkHttp3 通信実行・Call 管理層
│       ├── TurboClientManager.kt          # Android OkHttp3 クライアント・接続管理層
│       └── Messages.g.kt                  # Pigeon 自動生成コード（Kotlin 側）
├── darwin/
│   ├── flutter_turbo_fetch.podspec        # CocoaPods 定義（iOS 13.0+ / macOS 10.15+ 共通）
│   └── flutter_turbo_fetch/
│       ├── Package.swift                  # Swift Package Manager 定義（iOS 13.0+ / macOS 10.15+ 共通）
│       └── Sources/flutter_turbo_fetch/
│           ├── FlutterTurboFetchPlugin.swift # iOS / macOS FlutterPlugin インターフェース層
│           ├── TurboHttpExecutor.swift       # URLSession 通信実行・Task 管理層
│           ├── TurboSessionManager.swift     # URLSession 共有セッション・Delegate 管理層
│           └── Messages.g.swift           # Pigeon 自動生成コード（Swift 側）
├── windows/
│   ├── CMakeLists.txt                     # Windows CMake ビルド定義（winhttp リンク）
│   ├── include/flutter_turbo_fetch/
│   │   └── flutter_turbo_fetch_plugin_c_api.h # Windows プラグイン公開 C API ヘッダ
│   ├── flutter_turbo_fetch_plugin_c_api.cpp   # Windows プラグイン登録エントリポイント
│   ├── flutter_turbo_fetch_plugin.h/.cpp      # Windows FlutterPlugin インターフェース層
│   ├── turbo_http_executor.h/.cpp             # Windows WinHTTP 通信実行・スレッドプール管理層
│   ├── turbo_session_manager.h/.cpp           # Windows WinHTTP 共有セッション管理層
│   └── messages.g.h/.cpp                  # Pigeon 自動生成コード（C++ 側）
└── example/                               # PoC・ベンチマーク検証アプリ
```
