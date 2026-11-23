# ベンチマーク・性能検証仕様（`benchmark.md`）

> 実測結果および技術評価レポートについては [`docs/benchmark-results.md`](../../docs/benchmark-results.md) および [`docs/benchmarks/`](../../docs/benchmarks/)（[`android.md`](../../docs/benchmarks/android.md), [`windows.md`](../../docs/benchmarks/windows.md), [`ios.md`](../../docs/benchmarks/ios.md)）を参照。

---

## 1. 比較対象プリセットと計測シナリオ

### 1.1 比較対象プリセット（3構成）

同一のベンチマークシナリオ・同一の HTTP/2 モックサーバーに対して、以下の 3 構成を公平に比較する（個別実行および 3 構成一括連続実行をサポート）。

| プリセット名 | アダプタ（`HttpClientAdapter`） | トランスフォーマー（`Transformer`） | 方式・特徴 |
| :--- | :--- | :--- | :--- |
| **① IO (HTTP/1.1)** | `IOHttpClientAdapter` | `BackgroundTransformer` | Dio 標準構成（`dart:io`、50KiB 超で都度 `compute`） |
| **② dio_http2** | `Http2Adapter` | `BackgroundTransformer` | 純粋 Dart 実装の HTTP/2（`package:http2`、64KiB 初期ウィンドウ） |
| **③ TurboFetch** | **`TurboHttpAdapter`** | **`TurboTransformer`** | **OS ネイティブ一括受信 ＋ 24KiB 閾値ゼロコピー変換** |

> ※ `native_dio_adapter` は Windows で `IOHttpClientAdapter` へフォールバックされるほか、Android での自己署名証明書許可（`allowBadCertificates`）に非対応であるため比較対象外とする。なお、本プラグインの Android `Cronet` エンジンでも同様に `allowBadCertificates = true` は非サポートであり、Cronet 計測時は後述の Android Network Security Config を用いる。

### 1.2 計測シナリオ（実用スイープ × 10周）

全サイズを順次リクエストするシーケンスを **10周（10セット）** 実行し、プールされたワーカーで最大並列数を制御しながら最後の JSON パース完了までを計測する。

| シナリオ | サイズ構成（1周あたり） | 最大並列度 | 総リクエスト数（10周） | 主な検証観点 |
| :--- | :--- | :---: | :---: | :--- |
| **計測A** | **5KiB 〜 45KiB**（5KiB 刻み・9段階） | **3並列** | **90 req** | **24KiB 境界・軽量通信の応答性**（Main / Isolate 切替挙動） |
| **計測B** | **10KiB 〜 100KiB**（10KiB 刻み・10段階） | **5並列** | **100 req** | **実運用標準負荷の総合性能**（標準 API サイズでの実効速度） |
| **計測C** | **20KiB 〜 200KiB**（20KiB 刻み・10段階） | **10並列** | **100 req** | **高多重・セマフォ上限時の耐性**（HTTP/2 多重化とピークメモリ安定性） |

### 1.3 計測プロトコル体系

1. **分割パイプライン厳密プロトコル（標準プロトコル）**:
   - **適用方針**: iOS 実機検証で確立した共通標準仕様。今後の Linux / macOS 計測および Android（`OkHttp3` / `Cronet` 個別計測）・Windows 再計測でも本プロトコルを採用する（集計・可視化は `tool/benchmark/` の共通 CLI ツールを使用）。
   - **計測方式**: 全方式で `ResponseType.bytes` により全バイト受信完了までを **Net Time**、受信済み生バイト列に対するデコード・オフロード待ち時間を **Parse Time** として厳密に分離計測する。
   - **統計・監査**: **ウォームアップ 1 試行 ＋ 本測定 10 試行（N=10）** をラウンドロビン順で実行して熱スロットリングやキャッシュ偏りを排除し、サーバー側の独立イベントログと 1 リクエスト単位で照合・監査する。
   - **構造化ログ・メタデータ共通仕様**:
     - デバイス出力ログのプレフィックスはプラットフォーム非依存の `TURBO_BENCHMARK ` とする（既存 iOS ログとの後方互換のため、`tool/benchmark/export_benchmark.py` は `IOS_BENCHMARK ` も受理する）。
     - 端末内一時証跡ファイル名は `turbo-benchmark.jsonl` / `turbo-benchmark-diagnostic.txt` とする。
     - `session_start` レコードには `platform`（例: `android`, `ios`, `macos`, `windows`, `linux`）および `engine`（例: `okhttp`, `cronet`, `urlsession`, `winhttp`, `libcurl`、`--dart-define=BENCHMARK_ENGINE=...` で明示指定可能）を含める。
   - **Android 2 エンジン（`OkHttp3` / `Cronet`）の個別計測と証跡配置**:
     - Android はビルド時切替（`flutter_turbo_fetch.androidEngine`）により `okhttp` と `cronet` を個別にビルド・計測する。リポジトリの既定状態は `okhttp`（`example/android/gradle.properties` 未指定）を維持し、`cronet` 計測時のみ一時的に `flutter_turbo_fetch.androidEngine=cronet` を指定する。
     - **ローカル TLS 検証条件**: `okhttp` 計測では従来通り `BENCHMARK_LOCAL_TLS=true`（`allowBadCertificates: true`）を使用する。`cronet` 計測では `allowBadCertificates = true` 非サポートのため `BENCHMARK_LOCAL_TLS=false` とし、Android Network Security Config によりローカルベンチマーク用自己署名証明書を信頼させて計測する。
     - **証跡・レポート配置**: 生データは `tool/benchmark/data/android/<YYYY-MM-DD>-<engine>-*.csv`（例: `2026-11-23-okhttp-raw.csv`, `2026-11-23-cronet-raw.csv`）に保存し、公開レポートは `docs/benchmarks/android.md` 1 本に `OkHttp3` / `Cronet` 双方の実測結果と直接比較を統合する。
2. **通常パイプラインプロトコル（補助・従来計測）**:
   - **適用環境**: 従来の Android（API 36 Emulator）・Windows（Win11 Desktop）計測、および実利用時の総合所要時間の補助収集。
   - **特徴・制約**: `ResponseType.json` のまま `fetch()` 開始〜ヘッダ受領を Net Time、ボディ受信＋デコードを Parse Time として計測するため、ストリーミング型（IO / dio_http2）では Parse Time にダウンロード時間が混入する。

---

## 2. 収集メトリクスと計測境界

### 2.1 メトリクス定義

| メトリクス項目 | 単位 | 計測境界・定義 |
| :--- | :---: | :--- |
| **Total Wall Time** | ms | 10周の全リクエスト完了までの総所要時間（実効スループット） |
| **Net Time (Avg / P50 / P95)** | ms | `fetch()` 開始 〜 アダプタから全本文バイト列（または `ResponseBody`）が返却されるまでの経過時間 |
| **Parse Time (Avg / P50 / P95)** | ms | `transformResponse` 開始 〜 デコード・マッピング完了までの実効待ち時間（セマフォ獲得待ち時間を含む） |
| **Jank Frames** | 回 (%) | `SchedulerBinding`（`FrameTiming`）で 16.6ms（60fps 基準）を超過したフレーム数と発生率（※Flutter の仕様上、Release ビルドでは無効化されるため実機では N/A） |
| **Max Frame Duration** | ms | 計測中に発生した単一フレームの最大所要時間（UI フリーズの最大幅） |
| **Peak RSS / Delta** | MiB | `ProcessInfo.currentRss` による計測中の最大メモリ消費量および開始時からの増分 |

### 2.2 欠損値および安全性の原則

- 未取得の計測、全失敗時の統計、RSS 取得例外、フレーム未取得（Release 実機等）はゼロに置換せず **N/A** とする。
- 公開ログおよびレポートには端末 ID、ユーザー名、LAN アドレス、秘密鍵、証明書、署名情報を含めない。
- Simulator やホストテストの数値を実機結果として流用・偽装しない。実機計測が未実施の場合は未実施と明記する。

---

## 3. 実行前提条件と手順

1. **Release ビルド必須**: JIT オーバーヘッドや assert、デバッガ通信による歪みを排除するため、必ず `--release` モードで実行する。
2. **HTTP/2 モックサーバーの起動**:
   - 事前に `node example/server/http2_mock_server.mjs`（ポート `8443` または LAN `8444`）を起動しておく。
   - デフォルト接続先は Android エミュレータが `https://10.0.2.2:8443`、デスクトップ（Windows / macOS / Linux） / iOS シミュレータが `https://localhost:8443`（実機検証時は LAN IP または `adb reverse tcp:8443 tcp:8443` を使用）。
3. **実行コマンド（`example/` ディレクトリ）**:
   - **GUI 通常スイート実行**: `fvm flutter run --release -d <device_id>`
   - **CLI 統合テスト実行**: `fvm flutter test integration_test/benchmark_suite_test.dart -d <device_id> --release`
   - **厳密分割プロトコル実行（共通）**:
     - **iOS 実機**:
       ```bash
       fvm flutter build ios --release -t lib/main.dart \
         --dart-define=BENCHMARK_BASE_URL=https://<LAN_IP>:8444 \
         --dart-define=BENCHMARK_LOCAL_TLS=true \
         --dart-define=BENCHMARK_ENGINE=urlsession \
         --dart-define=BENCHMARK_SOURCE_SHA=$(git rev-parse HEAD)
       ```
     - **Android（実機 `adb reverse` / Wi-Fi LAN、またはエミュレータ）**:
       - 実機で USB 経由ローカル接続する場合は事前に `adb reverse tcp:8443 tcp:8443` を実行し、`BENCHMARK_BASE_URL=https://127.0.0.1:8443` を指定する（Wi-Fi LAN 計測時は `https://<LAN_IP>:8444`、エミュレータ時は `https://10.0.2.2:8443`）。
       - **OkHttp3 計測（デフォルト）**:
         ```bash
         fvm flutter build apk --release -t lib/main.dart \
           --dart-define=BENCHMARK_BASE_URL=https://127.0.0.1:8443 \
           --dart-define=BENCHMARK_LOCAL_TLS=true \
           --dart-define=BENCHMARK_ENGINE=okhttp \
           --dart-define=BENCHMARK_SOURCE_SHA=$(git rev-parse HEAD)
         ```
       - **Cronet 計測**（`example/android/gradle.properties` に `flutter_turbo_fetch.androidEngine=cronet` を一時設定し、Network Security Config でローカル証明書を信頼させた状態）:
         ```bash
         fvm flutter build apk --release -t lib/main.dart \
           --dart-define=BENCHMARK_BASE_URL=https://127.0.0.1:8443 \
           --dart-define=BENCHMARK_LOCAL_TLS=false \
           --dart-define=BENCHMARK_ENGINE=cronet \
           --dart-define=BENCHMARK_SOURCE_SHA=$(git rev-parse HEAD)
         ```
       - ビルドした APK を `adb install -r build/app/outputs/flutter-apk/app-release.apk` で配置して実行し、`adb logcat`（`TURBO_BENCHMARK` 行）または端末内 `turbo-benchmark.jsonl` とサーバーログを `python tool/benchmark/export_benchmark.py <device_log> <server_log> tool/benchmark/data/android/<YYYY-MM-DD>-<engine>` へ渡して検証・集計する。
