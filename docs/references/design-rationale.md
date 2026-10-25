# 設計背景・技術的根拠（Design Rationale）

本ドキュメントは、`flutter_turbo_fetch` のアーキテクチャ意思決定に至った背景・ボトルネック分析・既存パッケージの評価・各原則の技術的根拠をまとめたエッセンス資料である（通常時の実装では参照不要、設計見直しや Step 0-1 / 0-3 の検討時に参照する）。

---

## 1. 解決すべき対象課題と 3 大ボトルネック

Flutter（`package:dio`）において、数十KB〜数百KB以上の中〜大容量 JSON データを HTTP/2 並列リクエストした際、以下の 3 つの構造的要因が複合してスループットを低下させる。

1. **Pure Dart 実装（`dio_http2_adapter` / `package:http2`）の限界**
   - クライアント初期受信ウィンドウおよびコネクション全体の受信ウィンドウが RFC デフォルトの **65,535 bytes（約 64KB）固定**（コネクションウィンドウは内部隠蔽されており外部から変更不可）のため、並列通信で合計 64KB を超えると `WINDOW_UPDATE` 待ちの通信ストールが頻発する。
   - 1 ホスト 1 本の `SecureSocket` に集約されるため BoringSSL の TLS 復号が直列化され、さらに 16KB 以下の細切れフレーム解析がすべて Main Isolate の単一イベントループを占有する。
2. **標準ネイティブアダプタ（`native_dio_adapter`: `cronet_http` / `cupertino_http`）のオーバーヘッド**
   - `package:http` のストリーミングインターフェース（`StreamedResponse`）に準拠しているため、**細切れチャンク単位（16KB〜32KB）で Native ⇄ Dart（Main Isolate）間の FFI 通知とメモリコピー・往復待ちが発生**する。
   - **Android (`cronet_http`)**: 初回通信時に GMS（Google Play 開発者サービス）からの動的ロード遅延が発生するほか、1 チャンクごとに Dart から次の `urlRequest.read(byteBuffer)` を呼ぶ Pull 型ピンポンが発生する。
   - **iOS (`cupertino_http`)**: `NSURLSessionDataDelegate` を使用しているため、ヘッダ受信時（`didReceiveResponse:`）に Dart の Main Isolate から `ResponseAllow` が返るまでボディ受信が一時停止し、`didReceiveData:` のたびに FFI 通知が飛ぶ。
3. **JSON パースによる Main Isolate イベントループの阻害（二次被害）**
   - Dio 標準の `BackgroundTransformer` は、`utf8.decode`（バイト列→巨大な中間 `String` 生成）を Main Isolate で同期実行してから別 Isolate へ渡しており、さらに 50KB 超で無制限に `compute`（`Isolate.run`）をスポーンするため並列時にスレッド飽和を起こす。
   - Main Isolate が数ms〜十数msブロックされるだけで、他ストリームのチャンク受領や次チャンク要求が止まり、連鎖的な通信ストール（アプリケーション層の Head-of-Line Blocking）を引き起こす。

---

## 2. `pub.dev` 既存パッケージの評価と採否理由

| パッケージ | 内部エンジン | チャンク結合場所 | 評価と採否理由 |
| :--- | :--- | :--- | :--- |
| **`native_dio_adapter`**<br>(`cronet_http` / `cupertino_http`) | Android: Cronet<br>iOS: `URLSessionDataDelegate` | **Dart 側**<br>（細切れ往復） | **不採用（比較ベースラインとして使用）**<br>`StreamedResponse` 仕様のためチャンク単位の FFI 往復と iOS ヘッダ受信時の一時停止が解消できない。 |
| **`ok_http`** | Android: `OkHttp` (`jnigen`) | **Dart 側**<br>（ストリーム読出） | **不採用**<br>これも `package:http` のストリーミング実装であり、一括 `response.body.bytes()` の恩恵を受けられない。 |
| **`rhttp`** | 全 OS: Rust `reqwest` + `tokio` | **Rust 側で一括結合可** | **不採用（Step 0-1 にて確定）**<br>Rust スレッドで一括バイト結合して FFI 返却する機能（`Body.bytes`）を持つが、Rust バイナリ同梱によるアプリサイズ増加（数 MB）、OS 標準 TLS セッションキャッシュ・証明書ストア・プロキシ統合の非利用、Rust ビルド依存のコストから不採用。 |
| **`flutter_turbo_fetch`**<br>（本プラグイン） | **Android: `OkHttp`**<br>**iOS: `URLSession.dataTask(with:completionHandler:)`** | **各 OS ネイティブ側で一括結合** | **本命アーキテクチャ**<br>OS 標準/定番の軽量スタックを用い、チャンク結合までをネイティブのバックグラウンドスレッドで完結させ、完成した `Uint8List` を 1 回だけ Dart へ渡す。 |

---

## 3. 設計 4 原則の技術的根拠（なぜアンチパターンなのか）

1. **なぜネイティブ側で JSON パースまで行わないのか**
   - ネイティブ側（Kotlin/Swift）で JSON パースしても Dart ヒープへ直接 Map/List オブジェクトを渡すことはできず、「ネイティブでの再シリアライズ（Codec エンコード）＋Dart でのデシリアライズ」の二重コストが発生するため。ネイティブ側は「生のバイト配列（`Uint8List`）の一括結合・返却」に徹するのが最速となる。
2. **なぜ Android エンジンは Cronet ではなく `OkHttp` なのか**
   - Cronet のような GMS 動的ロード遅延がなく、HTTP/2 初期ウィンドウがデフォルトで `16MB` に拡張されており、`Okio` のセグメントプールを用いた `response.body.bytes()` の 1 行で高速なチャンク結合が完結するため（HTTP/3 QUIC が必須要件の場合のみ Cronet カスタムを検討）。
3. **なぜ iOS エンジンは `NSURLSessionDataDelegate` を使わないのか**
   - デリゲート方式では `didReceiveResponse:` / `didReceiveData:` のたびにスレッド間・言語間の通知コストが発生するため。`dataTask(with:completionHandler:)` を使用すれば、ヘッダ処理からチャンク結合までを iOS の CFNetwork 層（C/C++ バックグラウンドスレッド）内部で完結でき、`URLSessionDataTask` によるリクエスト単位のキャンセル制御も直接行える。
4. **なぜ常駐ワーカープール（`SendPort.send`）ではなく「セマフォ付き `Isolate.run`」なのか**
   - 常駐ワーカーが生き続けると、戻り値返却時の `SendPort.send(result)` で共有ヒープ内のオブジェクトツリー全体のディープコピー（Transitive Copy: パース時間の 25〜40% 相当のオーバーヘッド）が発生するため。`Isolate.run`（内部で `Isolate.exit` を使用）であれば、ワーカー終了に伴い共有ヒープのポインタ所有権を移すだけの **O(1) ゼロコピー（数μs）** で Main Isolate へ返却できる。

---

## 4. Dart ⇄ ネイティブ通信ブリッジ方式の比較背景（Step 0-3 にて案Aに確定）

- **案A（採用）: Pigeon (`StandardMessageCodec` + 型安全コード生成)**
  - Android の `byte[]` / iOS の `FlutterStandardTypedData(bytes: Data)` として 1 回返却する。
  - チャンク毎ではなく **1 リクエストにつき 1 往復のみ** であるため、Channel のシリアライズ/メモリコピー（100KB で約 0.02〜0.05ms）のオーバーヘッドは極小であり、バックグラウンドスレッドからのコールバック返却・スレッド安全性・型安全な保守性に優れる。
- **案B（不採用）: FFI / JNI (`jnigen` / `ffigen`)**
  - メモリコピー回数をさらに 1 回削減できる可能性があるが、非同期コールバックのライフサイクル管理や GC 連動（Finalizer）、スレッド制御の複雑性が大幅に増すため見送り。
