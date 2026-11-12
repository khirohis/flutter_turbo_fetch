# ベンチマーク・性能検証仕様（`benchmark.md`）

> 実測結果および技術評価レポートについては [`docs/benchmark-results.md`](../../docs/benchmark-results.md) および [`docs/benchmarks/`](../../docs/benchmarks/) を参照。

---

## 1. 比較対象プリセットと計測シナリオ

### 1.1 比較対象プリセット（3構成）

同一のベンチマークシナリオ・同一の HTTP/2 モックサーバーに対して、以下の 3 構成を公平に比較する（個別実行および 3 構成一括連続実行をサポート）。

| プリセット名 | アダプタ（`HttpClientAdapter`） | トランスフォーマー（`Transformer`） | 方式・特徴 |
| :--- | :--- | :--- | :--- |
| **① IO (HTTP/1.1)** | `IOHttpClientAdapter` | `BackgroundTransformer` | Dio 標準構成（`dart:io`、50KiB 超で都度 `compute`） |
| **② dio_http2** | `Http2Adapter` | `BackgroundTransformer` | 純粋 Dart 実装の HTTP/2（`package:http2`、64KiB 初期ウィンドウ） |
| **③ TurboFetch** | **`TurboHttpAdapter`** | **`TurboTransformer`** | **OS ネイティブ一括受信 ＋ 24KiB 閾値ゼロコピー変換** |

> ※ `native_dio_adapter` は Android Cronet が自己署名証明書許可（`allowBadCertificates`）に非対応であり、Windows では `IOHttpClientAdapter` へフォールバックされるため比較対象外とする。

### 1.2 計測シナリオ（実用スイープ × 10周）

全サイズを順次リクエストするシーケンスを **10周（10セット）** 実行し、プールされたワーカーで最大並列数を制御しながら最後の JSON パース完了までを計測する。

| シナリオ | サイズ構成（1周あたり） | 最大並列度 | 総リクエスト数（10周） | 主な検証観点 |
| :--- | :--- | :---: | :---: | :--- |
| **計測A** | **5KiB 〜 45KiB**（5KiB 刻み・9段階） | **3並列** | **90 req** | **24KiB 境界・軽量通信の応答性**（Main / Isolate 切替挙動） |
| **計測B** | **10KiB 〜 100KiB**（10KiB 刻み・10段階） | **5並列** | **100 req** | **実運用標準負荷の総合性能**（標準 API サイズでの実効速度） |
| **計測C** | **20KiB 〜 200KiB**（20KiB 刻み・10段階） | **10並列** | **100 req** | **高多重・セマフォ上限時の耐性**（HTTP/2 多重化とピークメモリ安定性） |

---

## 2. 収集メトリクスと計測境界

全プリセット共通の計測ラッパー（アダプタラッパー & トランスフォーマーラッパー）により、以下の 6 指標を収集し、UI テーブル表示・`debugPrint` 出力・Markdown クリップボードコピーを提供する。

| メトリクス項目 | 単位 | 計測境界・定義 |
| :--- | :---: | :--- |
| **Total Wall Time** | ms | 10周の全リクエスト完了までの総所要時間（実効スループット） |
| **Net Time (Avg / P95)** | ms | `fetch()` 開始 〜 アダプタから `ResponseBody` が返却されるまでの経過時間 |
| **Parse Time (Avg / P95)** | ms | `transformResponse` 開始 〜 デコード・マッピング完了までの実効待ち時間（セマフォ獲得待ち時間を含む） |
| **Jank Frames** | 回 (%) | `SchedulerBinding`（`FrameTiming`）で 16.6ms（60fps 基準）を超過したフレーム数と発生率 |
| **Max Frame Duration** | ms | 計測中に発生した単一フレームの最大所要時間（UI フリーズの最大幅） |
| **Peak RSS / Delta** | MiB | `ProcessInfo.currentRss` による計測中の最大メモリ消費量および開始時からの増分 |

---

## 3. 実行前提条件と手順

1. **Release ビルド必須**: JIT オーバーヘッドや assert、デバッガ通信による歪みを排除するため、必ず `--release` モードで実行する。
2. **HTTP/2 モックサーバーの起動**:
   - 事前に `node example/server/http2_mock_server.mjs`（ポート `8443`）を起動しておく。
   - デフォルト接続先は Android エミュレータが `https://10.0.2.2:8443`、デスクトップ（Windows / macOS / Linux） / iOS シミュレータが `https://localhost:8443`（実機検証時は LAN IP または `adb reverse tcp:8443 tcp:8443` を使用）。
3. **実行コマンド（`example/` ディレクトリ）**:
   - **GUI 実行**: `fvm flutter run --release -d <device_id>`
   - **CLI 統合テスト実行**: `fvm flutter test integration_test/benchmark_suite_test.dart -d <device_id> --release`
