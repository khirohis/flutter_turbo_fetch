import 'benchmark_models.dart';

/// ベンチマーク結果を Markdown テーブル形式に整形するユーティリティ
class BenchmarkFormatter {
  /// 指定シナリオと結果一覧から Markdown テーブル文字列を生成
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

    // 最速 (Total Wall Time 最小かつ成功数 > 0) の結果を特定
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

  /// 整数を3桁カンマ区切りの文字列に変換
  static String _formatNumber(int number) {
    final str = number.toString();
    final regExp = RegExp(r'(\d{1,3})(?=(\d{3})+(?!\d))');
    return str.replaceAllMapped(regExp, (match) => '${match[1]},');
  }
}
