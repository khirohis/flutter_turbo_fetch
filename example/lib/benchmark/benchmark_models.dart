/// Four adapter configuration presets for comparison.
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

/// Benchmark scenario definitions.
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

/// Metrics recorded for a single request.
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

/// Aggregated results for an entire scenario run.
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

  /// Total elapsed wall-clock time for all rounds (ms).
  final int totalWallTimeMs;

  /// Average network communication time (ms).
  final double netTimeAvgMs;

  /// 95th percentile (P95) network communication time (ms).
  final double netTimeP95Ms;

  /// Average JSON parse completion time, including semaphore wait (ms).
  final double parseTimeAvgMs;

  /// 95th percentile (P95) JSON parse completion time, including semaphore wait (ms).
  final double parseTimeP95Ms;

  /// Total frames rendered during the benchmark.
  final int totalFrames;

  /// Number of jank frames exceeding 16.67ms.
  final int jankFrames;

  /// Maximum frame duration (ms).
  final double maxFrameDurationMs;

  /// Process RSS before benchmark execution (MiB).
  final double rssBeforeMiB;

  /// Peak process RSS during benchmark execution (MiB).
  final double rssPeakMiB;

  /// Memory delta (MiB).
  final double rssDeltaMiB;

  /// Total count of successful requests.
  final int successCount;

  /// Total count of failed requests.
  final int failureCount;

  /// List of individual request metrics.
  final List<SingleRequestMetric> requestMetrics;

  /// Jank frame occurrence rate (%).
  double get jankRatePercent =>
      totalFrames > 0 ? (jankFrames / totalFrames) * 100.0 : 0.0;
}
