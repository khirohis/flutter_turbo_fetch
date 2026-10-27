import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch_example/main.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  final defaultHost =
      (!Platform.isIOS && Platform.isAndroid) ? '10.0.2.2' : 'localhost';
  final baseUrl = 'https://$defaultHost:8443';

  Future<void> runBenchmarkForScenario(
    WidgetTester tester,
    BenchmarkScenario scenario,
  ) async {
    await tester.pumpWidget(const MyApp());
    await tester.pump(const Duration(milliseconds: 500));

    final presets = BenchmarkPreset.values;
    final results = <BenchmarkScenarioResult>[];

    for (final preset in presets) {
      await Future<void>.delayed(const Duration(milliseconds: 800));
      await tester.pump(const Duration(milliseconds: 50));

      final result = await BenchmarkRunner.runScenario(
        scenario: scenario,
        preset: preset,
        baseUrl: baseUrl,
        allowBadCertificates: true,
        onProgress: (completed, total) {
          tester.pump(const Duration(milliseconds: 1));
        },
      );

      results.add(result);
    }

    final markdown = BenchmarkFormatter.formatMarkdownTable(
      scenario: scenario,
      results: results,
    );

    debugPrint('\n@@@MARKDOWN_START:${scenario.id}@@@\n$markdown\n@@@MARKDOWN_END:${scenario.id}@@@\n');
  }

  testWidgets(
    'Execute Benchmark Scenario A (5-45KiB, 3 parallel, 10 rounds = 90req)',
    (WidgetTester tester) async {
      await runBenchmarkForScenario(tester, BenchmarkScenario.scenarioA);
    },
    timeout: const Timeout(Duration(minutes: 5)),
  );

  testWidgets(
    'Execute Benchmark Scenario B (10-100KiB, 5 parallel, 10 rounds = 100req)',
    (WidgetTester tester) async {
      await runBenchmarkForScenario(tester, BenchmarkScenario.scenarioB);
    },
    timeout: const Timeout(Duration(minutes: 5)),
  );

  testWidgets(
    'Execute Benchmark Scenario C (20-200KiB, 10 parallel, 10 rounds = 100req)',
    (WidgetTester tester) async {
      await runBenchmarkForScenario(tester, BenchmarkScenario.scenarioC);
    },
    timeout: const Timeout(Duration(minutes: 5)),
  );
}
