import 'dart:io';
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:flutter_turbo_fetch_example/main.dart';
import 'package:integration_test/integration_test.dart';

int _extractItemCountMapper(dynamic data) {
  final map = data as Map<String, dynamic>;
  return (map['items'] as List<dynamic>).length;
}

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  final defaultHost =
      (!Platform.isIOS && Platform.isAndroid) ? '10.0.2.2' : 'localhost';
  final baseUrl = 'https://$defaultHost:8443';

  testWidgets('E2E functional verification over native HTTP/2', (
    WidgetTester tester,
  ) async {
    final dio = Dio(BaseOptions(baseUrl: baseUrl))
      ..httpClientAdapter = TurboHttpAdapter(allowBadCertificates: true)
      ..transformer = TurboTransformer();

    addTearDown(() => dio.close(force: true));

    // 1. Concurrent heterogeneous requests on a single Dio instance (json+mapper, plain, bytes, stream)
    final results = await Future.wait<Object?>(<Future<Object?>>[
      dio
          .get<int>(
            '/json?sizeKb=50',
            options: TurboOptions<int>(
              responseType: ResponseType.json,
              mapper: _extractItemCountMapper,
            ),
          )
          .then((r) => r.data),
      dio
          .get<String>(
            '/health',
            options: TurboOptions<String>(responseType: ResponseType.plain),
          )
          .then((r) => r.data),
      dio
          .get<Uint8List>(
            '/json?sizeKb=10',
            options: TurboOptions<Uint8List>(responseType: ResponseType.bytes),
          )
          .then((r) => r.data),
      dio
          .get<ResponseBody>(
            '/health',
            options: TurboOptions<ResponseBody>(
              responseType: ResponseType.stream,
            ),
          )
          .then((r) => r.data),
    ]);

    expect(results[0] as int, greaterThan(0));
    expect(results[1] as String, contains('"status":"ok"'));
    expect((results[2] as Uint8List).lengthInBytes, greaterThanOrEqualTo(10 * 1024));
    final streamChunks = await (results[3] as ResponseBody).stream.toList();
    expect(streamChunks, hasLength(1));

    // 2. maxResponseBytesExceeded check (with Content-Length and without Content-Length)
    await expectLater(
      dio.get<dynamic>(
        '/json?sizeKb=50',
        options: TurboOptions(maxResponseBytes: 1024),
      ),
      throwsA(
        isA<DioException>()
            .having((e) => e.type, 'type', DioExceptionType.unknown)
            .having((e) => e.message, 'message', contains('1024 bytes')),
      ),
    );

    await expectLater(
      dio.get<dynamic>(
        '/json?sizeKb=50&omitContentLength=1',
        options: TurboOptions(maxResponseBytes: 1024),
      ),
      throwsA(
        isA<DioException>()
            .having((e) => e.type, 'type', DioExceptionType.unknown)
            .having((e) => e.message, 'message', contains('1024 bytes')),
      ),
    );

    // 3. Non-200 status (404) badResponse check
    await expectLater(
      dio.get<dynamic>('/json?status=404&sizeKb=1'),
      throwsA(
        isA<DioException>()
            .having((e) => e.type, 'type', DioExceptionType.badResponse)
            .having((e) => e.response?.statusCode, 'statusCode', 404),
      ),
    );
  });

  testWidgets('Run parallel HTTP/2 benchmarks on device/emulator', (
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
      await waitRealTime(const Duration(seconds: 2));
    }

    // 1. Local 100KB (Android 10.0.2.2) presets ①, ②, ④
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

    // 2. Public DummyJSON for 4-way comparison (including native_dio_adapter)
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
