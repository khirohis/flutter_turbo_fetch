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

> ※ `native_dio_adapter` は Android Cronet が自己署名証明書許可（`allowBadCertificates`）に非対応であり、Windows では `IOHttpClientAdapter` へフォールバックされるため比較対象外とする。

### 1.2 計測シナリオ（実用スイープ × 10周）

全サイズを順次リクエストするシーケンスを **10周（10セット）** 実行し、プールされたワーカーで最大並列数を制御しながら最後の JSON パース完了までを計測する。

| シナリオ | サイズ構成（1周あたり） | 最大並列度 | 総リクエスト数（10周） | 主な検証観点 |
| :--- | :--- | :---: | :---: | :--- |
| **計測A** | **5KiB 〜 45KiB**（5KiB 刻み・9段階） | **3並列** | **90 req** | **24KiB 境界・軽量通信の応答性**（Main / Isolate 切替挙動） |
| **計測B** | **10KiB 〜 100KiB**（10KiB 刻み・10段階） | **5並列** | **100 req** | **実運用標準負荷の総合性能**（標準 API サイズでの実効速度） |
| **計測C** | **20KiB 〜 200KiB**（20KiB 刻み・10段階） | **10並列** | **100 req** | **高多重・セマフォ上限時の耐性**（HTTP/2 多重化とピークメモリ安定性） |

### 1.3 計測プロトコル体系（通常パイプライン vs. 厳密プロトコル）

現在、本プロジェクトには以下の 2 つの計測プロトコルが存在する：

1. **通常パイプライン・1試行プロトコル（Historical Suite）**
   - **採用環境**: Android（API 36 Emulator）、Windows（Win11 Desktop）
   - **特徴**: 通常の Dio パイプライン（`ResponseType.json`）のまま、`fetch()` 開始〜ヘッダ受領までを Net Time、その後のボディ受信＋JSON デコードを Parse Time として計測。固定順の 1 試行。
   - **課題**: ストリーミング型アダプタ（IO, dio_http2）では Parse Time にボディダウンロード時間が混入し、純粋なデコード時間との分離が非対称になる。
2. **分割パイプライン厳密プロトコル（Split-Pipeline Rigorous Protocol）**
   - **採用環境**: iOS（iPhone 17 Physical Device Release）
   - **特徴**:
     - 全方式で `ResponseType.bytes` により全バイト受信完了までを「Network Time」として統一計測。
     - 受信済み生バイト配列に対して各プリセットのトランスフォーマーを実行し、純粋なデコード・オフロード待ち時間を「Parse Time」として計測。
     - **ウォームアップ 1 試行 ＋ 本測定 10 試行（N=10）** を実行し、試行ごとに実行順序を循環（ラウンドロビン）させて熱ダウントロットリングやキャッシュ偏りを排除。
     - サーバー側の独立イベントログと 1 リクエスト単位で照合・監査。

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

- 未取得の計測、全失敗時の統計、RSS 取得例外、フレーム未取得（Release 実機での `FrameTiming` コールバック無効化等）はゼロに置換せず **N/A** とする。
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
   - **厳密分割プロトコル実行（iOS 実機等）**:
     ```bash
     fvm flutter build ios --release -t lib/main.dart \
       --dart-define=BENCHMARK_BASE_URL=https://<LAN_IP>:8444 \
       --dart-define=BENCHMARK_LOCAL_TLS=true \
       --dart-define=BENCHMARK_SOURCE_SHA=$(git rev-parse HEAD)
     ```

---

## 4. 今後の標準化方針（全プラットフォームへの展開）

iOS 実機検証において確立された **「分割パイプライン厳密プロトコル（ウォームアップ ＋ 10試行ラウンドロビン ＋ サーバーログ監査）」** は、従来の単一試行ストリーミング計測よりも科学的・統計的に極めて優れていることが証明された。

そのため、今後のロードマップにおいて以下の段階的な統一を進める：

1. **全プラットフォームの共通標準化**:
   - 今後実施する Linux、macOS のベンチマーク、および Android、Windows の再ベンチマーク計測においては、本厳密プロトコルを共通標準仕様として採用する。
2. **共通ベンチマークツールの整備**:
   - `tool/benchmark/` 配下の集計・エクスポート・プロットスクリプトを、全 OS のログ形式に対応した共通 CLI ツールとして維持・拡充する。
3. **通常パイプラインとの併記**:
   - 通常パイプライン（`ResponseType.json` でのそのままの Dio 利用時）の実測値も補助的に収集し、実利用時の総合所要時間と分割計測時の内訳の双方を公開ドキュメントで説明可能とする。
