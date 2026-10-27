import 'dart:io';
import 'dart:typed_data';
import 'package:dio/dio.dart';
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

    // 1. Basic JSON fetch & TurboOptions(mapper) execution
    final res = await dio.get<dynamic>(
      '/json?sizeKb=10',
      options: TurboOptions(
        responseType: ResponseType.json,
        mapper: _extractItemCountMapper,
      ),
    );
    expect(res.statusCode, equals(200));
    expect(res.data, isA<int>());
    expect(res.data as int, greaterThan(0));

    // ResponseType.bytes
    final bytesRes = await dio.get<dynamic>(
      '/json?sizeKb=5',
      options: Options(responseType: ResponseType.bytes),
    );
    expect(bytesRes.statusCode, equals(200));
    expect(bytesRes.data, isA<Uint8List>());

    // ResponseType.plain
    final plainRes = await dio.get<dynamic>(
      '/json?sizeKb=5',
      options: Options(responseType: ResponseType.plain),
    );
    expect(plainRes.statusCode, equals(200));
    expect(plainRes.data, isA<String>());

    // ResponseType.stream
    final streamRes = await dio.get<dynamic>(
      '/json?sizeKb=5',
      options: Options(responseType: ResponseType.stream),
    );
    expect(streamRes.statusCode, equals(200));
    expect(streamRes.data, isA<ResponseBody>());

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

  testWidgets('Verify benchmark scenario UI controls on device/emulator', (
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

    // Verify UI components are rendered
    expect(find.text('TurboFetch Benchmark'), findsOneWidget);
    expect(find.text('1. 接続先モックサーバー設定'), findsOneWidget);
    expect(find.text('2. 計測シナリオ選択'), findsOneWidget);
    expect(find.text('3. ベンチマーク実行'), findsOneWidget);
    expect(find.text('4プリセット一括連続実行 (推奨)'), findsOneWidget);
    expect(find.text('個別実行'), findsOneWidget);

    // Verify scenario segments can be switched
    await tester.tap(find.text('計測B (10〜100KiB)'));
    await waitRealTime(const Duration(milliseconds: 300));
    expect(find.textContaining('実運用標準負荷の総合性能'), findsOneWidget);

    await tester.tap(find.text('計測C (20〜200KiB)'));
    await waitRealTime(const Duration(milliseconds: 300));
    expect(find.textContaining('高多重・セマフォ上限時の耐性'), findsOneWidget);

    await tester.tap(find.text('計測A (5〜45KiB)'));
    await waitRealTime(const Duration(milliseconds: 300));
    expect(find.textContaining('24KiB閾値の妥当性評価'), findsOneWidget);
  });
}
