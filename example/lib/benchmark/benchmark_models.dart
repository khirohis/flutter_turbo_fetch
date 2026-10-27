/// 比較対象の 4 つのアダプタ構成プリセット
enum BenchmarkPreset {
  ioHttp1(
    id: 'io_http1',
    label: '① IOHttpClientAdapter (HTTP/1.1)',
    shortName: '① IO (HTTP/1.1)',
  ),
  dioHttp2(
    id: 'dio_http2',
    label: '② dio_http2_adapter (Dart HTTP/2)',
    shortName: '② dio_http2',
  ),
  nativeDio(
    id: 'native_dio',
    label: '③ native_dio_adapter (Stream Bridge)',
    shortName: '③ native_dio',
  ),
  turboFetch(
    id: 'turbo_fetch',
    label: '④ TurboHttpAdapter + TurboTransformer',
    shortName: '④ TurboFetch',
  );

  const BenchmarkPreset({
    required this.id,
    required this.label,
    required this.shortName,
  });

  final String id;
  final String label;
  final String shortName;
}

/// 計測シナリオ定義
enum BenchmarkScenario {
  scenarioA(
    id: 'scenario_a',
    label: '計測A (5〜45KiB, 3並列, 10周 = 90req)',
    shortLabel: '計測A (5〜45KiB)',
    description: '24KiB閾値の妥当性評価（小サイズ通信での応答性）',
    sizesKiB: [5, 10, 15, 20, 25, 30, 35, 40, 45],
    concurrency: 3,
    rounds: 10,
  ),
  scenarioB(
    id: 'scenario_b',
    label: '計測B (10〜100KiB, 5並列, 10周 = 100req)',
    shortLabel: '計測B (10〜100KiB)',
    description: '実運用標準負荷の総合性能（標準レスポンスサイズ）',
    sizesKiB: [10, 20, 30, 40, 50, 60, 70, 80, 90, 100],
    concurrency: 5,
    rounds: 10,
  ),
  scenarioC(
    id: 'scenario_c',
    label: '計測C (20〜200KiB, 10並列, 10周 = 100req)',
    shortLabel: '計測C (20〜200KiB)',
    description: '高多重・セマフォ上限時の耐性（並列キューイング・メモリ安定性）',
    sizesKiB: [20, 40, 60, 80, 100, 120, 140, 160, 180, 200],
    concurrency: 10,
    rounds: 10,
  );

  const BenchmarkScenario({
    required this.id,
    required this.label,
    required this.shortLabel,
    required this.description,
    required this.sizesKiB,
    required this.concurrency,
    required this.rounds,
  });

  final String id;
  final String label;
  final String shortLabel;
  final String description;
  final List<int> sizesKiB;
  final int concurrency;
  final int rounds;

  int get totalRequests => sizesKiB.length * rounds;
}

/// 1 リクエスト単位の計測結果
class SingleRequestMetric {
  const SingleRequestMetric({
    required this.sizeKiB,
    required this.roundIndex,
    required this.netTimeMs,
    required this.parseTimeMs,
    required this.totalTimeMs,
    required this.statusCode,
    this.errorMessage,
  });

  final int sizeKiB;
  final int roundIndex;
  final double netTimeMs;
  final double parseTimeMs;
  final double totalTimeMs;
  final int statusCode;
  final String? errorMessage;

  bool get isSuccess => errorMessage == null && statusCode >= 200 && statusCode < 300;
}

/// シナリオ実行全体の集計結果
class BenchmarkScenarioResult {
  const BenchmarkScenarioResult({
    required this.scenario,
    required this.preset,
    required this.totalWallTimeMs,
    required this.netTimeAvgMs,
    required this.netTimeP95Ms,
    required this.parseTimeAvgMs,
    required this.parseTimeP95Ms,
    required this.totalFrames,
    required this.jankFrames,
    required this.maxFrameDurationMs,
    required this.rssBeforeMiB,
    required this.rssPeakMiB,
    required this.rssDeltaMiB,
    required this.successCount,
    required this.failureCount,
    required this.requestMetrics,
  });

  final BenchmarkScenario scenario;
  final BenchmarkPreset preset;

  /// 全 10 周完了までの実時間 (ms)
  final int totalWallTimeMs;

  /// ネットワーク通信時間の平均値 (ms)
  final double netTimeAvgMs;

  /// ネットワーク通信時間の 95 パーセンタイル (ms)
  final double netTimeP95Ms;

  /// JSON パース実効完了待ち時間（セマフォ待ち含む）の平均値 (ms)
  final double parseTimeAvgMs;

  /// JSON パース実効完了待ち時間（セマフォ待ち含む）の 95 パーセンタイル (ms)
  final double parseTimeP95Ms;

  /// 計測期間中の総フレーム数
  final int totalFrames;

  /// 16.6ms を超過したジャンクフレーム数
  final int jankFrames;

  /// 最大フレーム所要時間 (ms)
  final double maxFrameDurationMs;

  /// 開始前のプロセス RSS (MiB)
  final double rssBeforeMiB;

  /// 計測中の最大プロセス RSS (MiB)
  final double rssPeakMiB;

  /// メモリ増分 (MiB)
  final double rssDeltaMiB;

  /// 成功リクエスト数
  final int successCount;

  /// 失敗リクエスト数
  final int failureCount;

  /// リクエスト個別メトリクス一覧
  final List<SingleRequestMetric> requestMetrics;

  /// ジャンクフレーム発生率 (%)
  double get jankRatePercent =>
      totalFrames > 0 ? (jankFrames / totalFrames) * 100.0 : 0.0;
}
