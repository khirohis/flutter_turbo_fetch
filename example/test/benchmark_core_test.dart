import 'dart:convert';
import 'dart:typed_data';

import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch_example/benchmark/benchmark_models.dart';
import 'package:flutter_turbo_fetch_example/benchmark/benchmark_runner.dart';
import 'package:flutter_turbo_fetch_example/benchmark/benchmark_timing_wrapper.dart';

class _MockAdapter implements HttpClientAdapter {
  _MockAdapter({required this.delayMs, required this.responseBytes});

  final int delayMs;
  final Uint8List responseBytes;

  @override
  Future<ResponseBody> fetch(
    RequestOptions options,
    Stream<Uint8List>? requestStream,
    Future<void>? cancelFuture,
  ) async {
    if (delayMs > 0) {
      await Future<void>.delayed(Duration(milliseconds: delayMs));
    }
    return ResponseBody.fromBytes(
      responseBytes,
      200,
      headers: {
        Headers.contentTypeHeader: [Headers.jsonContentType],
      },
    );
  }

  @override
  void close({bool force = false}) {}
}

class _MockTransformer implements Transformer {
  _MockTransformer({required this.delayMs});

  final int delayMs;

  @override
  Future<String> transformRequest(RequestOptions options) async => '';

  @override
  Future transformResponse(
    RequestOptions options,
    ResponseBody responseBody,
  ) async {
    if (delayMs > 0) {
      await Future<void>.delayed(Duration(milliseconds: delayMs));
    }
    final builder = BytesBuilder(copy: false);
    await for (final chunk in responseBody.stream) {
      builder.add(chunk);
    }
    final bytes = builder.takeBytes();
    return jsonDecode(utf8.decode(bytes));
  }
}

void main() {
  group('BenchmarkTimingWrapper', () {
    test('extractBenchmarkTimings correctly measures net and parse time', () async {
      final payloadJson = jsonEncode({'message': 'hello'});
      final payloadBytes = Uint8List.fromList(utf8.encode(payloadJson));

      final dio = Dio();
      dio.httpClientAdapter = BenchmarkTimingAdapter(
        _MockAdapter(delayMs: 30, responseBytes: payloadBytes),
      );
      dio.transformer = BenchmarkTimingTransformer(
        _MockTransformer(delayMs: 20),
      );

      final response = await dio.get<dynamic>('https://example.com/test');
      final timings = extractBenchmarkTimings(response);

      // Verify Net Time is around 30ms (>= 25ms)
      expect(timings.netTimeMs, greaterThanOrEqualTo(25.0));
      // Verify Parse Time is around 20ms (>= 15ms)
      expect(timings.parseTimeMs, greaterThanOrEqualTo(15.0));
      expect(response.data, equals({'message': 'hello'}));
    });
  });

  group('BenchmarkRunner Statistics', () {
    test('calculateAvg returns correct mean', () {
      expect(BenchmarkRunner.calculateAvg([]), equals(0.0));
      expect(BenchmarkRunner.calculateAvg([10.0, 20.0, 30.0]), equals(20.0));
      expect(BenchmarkRunner.calculateAvg([5.0, 15.0]), equals(10.0));
    });

    test('calculateP95 returns correct 95th percentile', () {
      expect(BenchmarkRunner.calculateP95([]), equals(0.0));

      // 100 elements: 1.0 to 100.0
      final values = List.generate(100, (i) => (i + 1).toDouble());
      final p95 = BenchmarkRunner.calculateP95(values);
      expect(p95, equals(95.0));
    });
  });

  group('BenchmarkScenario Models', () {
    test('scenario configurations match requirements', () {
      expect(BenchmarkScenario.scenarioA.concurrency, equals(3));
      expect(BenchmarkScenario.scenarioA.sizesKiB, equals([5, 10, 15, 20, 25, 30, 35, 40, 45]));
      expect(BenchmarkScenario.scenarioA.totalRequests, equals(90));

      expect(BenchmarkScenario.scenarioB.concurrency, equals(5));
      expect(BenchmarkScenario.scenarioB.sizesKiB, equals([10, 20, 30, 40, 50, 60, 70, 80, 90, 100]));
      expect(BenchmarkScenario.scenarioB.totalRequests, equals(100));

      expect(BenchmarkScenario.scenarioC.concurrency, equals(10));
      expect(BenchmarkScenario.scenarioC.sizesKiB, equals([20, 40, 60, 80, 100, 120, 140, 160, 180, 200]));
      expect(BenchmarkScenario.scenarioC.totalRequests, equals(100));
    });
  });
}
