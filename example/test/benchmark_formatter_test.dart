import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch_example/benchmark/benchmark_formatter.dart';
import 'package:flutter_turbo_fetch_example/benchmark/benchmark_models.dart';

void main() {
  group('BenchmarkFormatter', () {
    test('formatMarkdownTable formats table and highlights fastest preset', () {
      final results = [
        const BenchmarkScenarioResult(
          scenario: BenchmarkScenario.scenarioA,
          preset: BenchmarkPreset.ioHttp1,
          totalWallTimeMs: 3420,
          netTimeAvgMs: 32.1,
          netTimeP95Ms: 58.4,
          parseTimeAvgMs: 4.8,
          parseTimeP95Ms: 7.2,
          totalFrames: 100,
          jankFrames: 15,
          maxFrameDurationMs: 42.1,
          rssBeforeMiB: 67.2,
          rssPeakMiB: 85.4,
          rssDeltaMiB: 18.2,
          successCount: 90,
          failureCount: 0,
          requestMetrics: [],
        ),
        const BenchmarkScenarioResult(
          scenario: BenchmarkScenario.scenarioA,
          preset: BenchmarkPreset.turboFetch,
          totalWallTimeMs: 1820,
          netTimeAvgMs: 18.5,
          netTimeP95Ms: 29.0,
          parseTimeAvgMs: 1.2,
          parseTimeP95Ms: 2.1,
          totalFrames: 100,
          jankFrames: 0,
          maxFrameDurationMs: 8.2,
          rssBeforeMiB: 67.2,
          rssPeakMiB: 74.0,
          rssDeltaMiB: 6.8,
          successCount: 90,
          failureCount: 0,
          requestMetrics: [],
        ),
      ];

      final markdown = BenchmarkFormatter.formatMarkdownTable(
        scenario: BenchmarkScenario.scenarioA,
        results: results,
      );

      // Verify header contains scenario label
      expect(markdown, contains('### Benchmark Results: Scenario A'));
      expect(markdown, contains('| Adapter Configuration | Total Time | Net Time (Avg/P95) |'));

      // Verify normal row for ioHttp1
      expect(markdown, contains('| (1) IO (HTTP/1.1) | 3,420 ms | 32.1 ms / 58.4 ms |'));

      // Verify fastest row for turboFetch is bolded
      expect(markdown, contains('| **(4) TurboFetch** | **1,820 ms** | **18.5 ms / 29.0 ms** |'));
      expect(markdown, contains('**74.0 MiB (+6.8 MiB)**'));
    });

    test('formatMarkdownTable handles empty results', () {
      final markdown = BenchmarkFormatter.formatMarkdownTable(
        scenario: BenchmarkScenario.scenarioB,
        results: [],
      );
      expect(markdown, contains('*No benchmark results recorded*'));
    });
  });
}
