import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:flutter_turbo_fetch_example/main.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('Run parallel HTTP/2 benchmarks on Android Emulator', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const MyApp());
    await tester.pump(const Duration(milliseconds: 500));

    Future<void> waitRealTime(Duration duration) async {
      const step = Duration(milliseconds: 100);
      final steps = duration.inMilliseconds ~/ step.inMilliseconds;
      for (int i = 0; i < steps; i++) {
        await Future<void>.delayed(step);
        await tester.pump();
      }
    }

    Future<void> runPreset(BenchmarkPreset preset) async {
      await tester.tap(find.widgetWithText(ChoiceChip, preset.shortName));
      await waitRealTime(const Duration(milliseconds: 300));

      await tester.tap(find.text('Run Parallel Benchmark'));
      await tester.pump();

      for (int i = 0; i < 150; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 200));
        await tester.pump();
        if (find.text('Run Parallel Benchmark').evaluate().isNotEmpty) {
          break;
        }
      }
      // Pause for 2 seconds so the result card can be clearly seen on the emulator screen
      await waitRealTime(const Duration(seconds: 2));
    }

    // 1. Ensure Local 100KB (Android 10.0.2.2) is selected and run presets ①, ②, ④
    await tester.tap(find.text('Local 100KB (Android 10.0.2.2)'));
    await waitRealTime(const Duration(milliseconds: 300));

    debugPrint('=== Suite 1: Local HTTP/2 Mock Server (100KB JSON x 15) ===');
    for (final preset in [
      BenchmarkPreset.ioHttp1,
      BenchmarkPreset.dioHttp2,
      BenchmarkPreset.turboFetch,
    ]) {
      await runPreset(preset);
    }

    expect(find.text('Benchmark Results (3)'), findsOneWidget);

    // 2. Switch to Public DummyJSON for 4-way comparison (including native_dio_adapter)
    await tester.tap(find.text('Clear'));
    await waitRealTime(const Duration(milliseconds: 500));
    await tester.tap(find.text('Public DummyJSON'));
    await waitRealTime(const Duration(milliseconds: 500));

    debugPrint('=== Suite 2: Public HTTPS HTTP/2 Endpoint (4 Presets x 15) ===');
    for (final preset in BenchmarkPreset.values) {
      await runPreset(preset);
    }

    expect(find.text('Benchmark Results (4)'), findsOneWidget);
  });
}
