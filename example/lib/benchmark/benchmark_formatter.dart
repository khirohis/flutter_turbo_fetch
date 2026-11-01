import 'benchmark_models.dart';

/// Utility for formatting benchmark results into Markdown tables.
class BenchmarkFormatter {
  /// Generates a Markdown table string from the scenario and result list.
  static String formatMarkdownTable({
    required BenchmarkScenario scenario,
    required List<BenchmarkScenarioResult> results,
  }) {
    if (results.isEmpty) {
      return '### ベンチマーク結果: ${scenario.label}\n\n*計測結果がありません*';
    }

    final buffer = StringBuffer();
    buffer.writeln('### ベンチマーク結果: ${scenario.label}');
    buffer.writeln();
    buffer.writeln(
      '| アダプタ構成 | Total Time | Net Time (Avg/P95) | Parse Time (Avg/P95) | Jank Frames (%) | Max Frame | Peak RSS (Δ) |',
    );
    buffer.writeln(
      '| :--- | :---: | :---: | :---: | :---: | :---: | :---: |',
    );

    // Find the fastest result (minimum total wall time with successCount > 0).
    int? minTotalTime;
    for (final r in results) {
      if (r.successCount > 0) {
        if (minTotalTime == null || r.totalWallTimeMs < minTotalTime) {
          minTotalTime = r.totalWallTimeMs;
        }
      }
    }

    for (final r in results) {
      final isFastest = minTotalTime != null && r.totalWallTimeMs == minTotalTime;

      final totalTimeStr = '${_formatNumber(r.totalWallTimeMs)} ms';
      final netTimeStr =
          '${r.netTimeAvgMs.toStringAsFixed(1)} ms / ${r.netTimeP95Ms.toStringAsFixed(1)} ms';
      final parseTimeStr =
          '${r.parseTimeAvgMs.toStringAsFixed(1)} ms / ${r.parseTimeP95Ms.toStringAsFixed(1)} ms';
      final jankStr =
          '${r.jankFrames} (${r.jankRatePercent.toStringAsFixed(1)}%)';
      final maxFrameStr = '${r.maxFrameDurationMs.toStringAsFixed(1)} ms';
      final rssDeltaStr =
          '${r.rssPeakMiB.toStringAsFixed(1)} MiB (+${r.rssDeltaMiB.toStringAsFixed(1)} MiB)';

      if (isFastest) {
        buffer.writeln(
          '| **${r.preset.shortName}** | **$totalTimeStr** | **$netTimeStr** | **$parseTimeStr** | **$jankStr** | **$maxFrameStr** | **$rssDeltaStr** |',
        );
      } else {
        buffer.writeln(
          '| ${r.preset.shortName} | $totalTimeStr | $netTimeStr | $parseTimeStr | $jankStr | $maxFrameStr | $rssDeltaStr |',
        );
      }
    }

    return buffer.toString();
  }

  /// Formats an integer with comma thousands separators.
  static String _formatNumber(int number) {
    final str = number.toString();
    final regExp = RegExp(r'(\d{1,3})(?=(\d{3})+(?!\d))');
    return str.replaceAllMapped(regExp, (match) => '${match[1]},');
  }
}
