/// Three adapter configuration presets for comparison.
enum BenchmarkPreset {
  ioHttp1(
    id: 'io_http1',
    label: '(1) IOHttpClientAdapter (HTTP/1.1)',
    shortName: '(1) IO (HTTP/1.1)',
  ),
  dioHttp2(
    id: 'dio_http2',
    label: '(2) dio_http2_adapter (Dart HTTP/2)',
    shortName: '(2) dio_http2',
  ),
  turboFetch(
    id: 'turbo_fetch',
    label: '(3) TurboHttpAdapter + TurboTransformer',
    shortName: '(3) TurboFetch',
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
    label: 'Scenario A (5-45KiB, 3 workers, 10 rounds = 90 req)',
    shortLabel: 'Scenario A (5-45KiB)',
    description: 'Validation of the 24KiB threshold (responsiveness for small payloads)',
    sizesKiB: [5, 10, 15, 20, 25, 30, 35, 40, 45],
    concurrency: 3,
    rounds: 10,
  ),
  scenarioB(
    id: 'scenario_b',
    label: 'Scenario B (10-100KiB, 5 workers, 10 rounds = 100 req)',
    shortLabel: 'Scenario B (10-100KiB)',
    description: 'General performance under standard production load (typical response sizes)',
    sizesKiB: [10, 20, 30, 40, 50, 60, 70, 80, 90, 100],
    concurrency: 5,
    rounds: 10,
  ),
  scenarioC(
    id: 'scenario_c',
    label: 'Scenario C (20-200KiB, 10 workers, 10 rounds = 100 req)',
    shortLabel: 'Scenario C (20-200KiB)',
    description: 'Resilience under high concurrency and semaphore limits (queueing & memory stability)',
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
