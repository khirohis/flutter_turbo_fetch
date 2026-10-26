import 'dart:async';
import 'dart:convert';
import 'dart:io' show ProcessInfo;
import 'dart:math' as math;
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'package:flutter/foundation.dart' show debugPrint;
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';

class BenchmarkPayloadModel {
  const BenchmarkPayloadModel({
    required this.sizeKb,
    required this.itemCount,
    required this.totalScore,
  });

  final int sizeKb;
  final int itemCount;
  final int totalScore;

  static BenchmarkPayloadModel fromJson(dynamic raw) {
    final map = raw as Map<String, dynamic>;
    final items = map['items'] as List<dynamic>;
    int scoreSum = 0;
    for (final item in items) {
      final metrics = (item as Map<String, dynamic>)['metrics'] as Map<String, dynamic>;
      scoreSum += metrics['score'] as int;
    }
    return BenchmarkPayloadModel(
      sizeKb: map['sizeKb'] as int,
      itemCount: items.length,
      totalScore: scoreSum,
    );
  }
}

Uint8List _generatePayloadBytes(int targetKb) {
  final targetBytes = targetKb * 1024;
  final items = <Map<String, Object>>[];
  int index = 0;

  while (true) {
    items.add(<String, Object>{
      'id': index,
      'uuid': 'item-$index-${'x' * 24}',
      'title': 'Benchmark Payload Record #$index',
      'timestamp': '2026-10-27T00:00:00.000Z',
      'metrics': <String, Object>{
        'score': (index * 17) % 100,
        'active': index.isEven,
        'tags': <String>['http2', 'zero-copy', 'isolate', 'flutter_turbo_fetch'],
      },
      'description':
          'High-concurrency HTTP/2 batch reception and zero-copy Isolate JSON decoding benchmark item.',
    });
    index++;
    if (index % 10 == 0) {
      final currentBytes = utf8.encode(
        jsonEncode(<String, Object>{
          'sizeKb': targetKb,
          'count': items.length,
          'items': items,
        }),
      );
      if (currentBytes.length >= targetBytes) {
        return Uint8List.fromList(currentBytes);
      }
    }
  }
}

class _MicroBenchmarkMetrics {
  const _MicroBenchmarkMetrics({
    required this.workloadLabel,
    required this.strategyLabel,
    required this.totalElapsedMs,
    required this.avgLatencyMs,
    required this.maxLatencyMs,
    required this.maxEventLoopLagMs,
    required this.rssPeakDeltaMb,
  });

  final String workloadLabel;
  final String strategyLabel;
  final int totalElapsedMs;
  final double avgLatencyMs;
  final int maxLatencyMs;
  final double maxEventLoopLagMs;
  final double rssPeakDeltaMb;
}

double _currentRssMb() {
  try {
    return ProcessInfo.currentRss / (1024 * 1024);
  } catch (_) {
    return 0.0;
  }
}

Future<_MicroBenchmarkMetrics> _measureWorkload({
  required String workloadLabel,
  required String strategyLabel,
  required Uint8List payloadBytes,
  required int concurrency,
  required Future<BenchmarkPayloadModel> Function(Uint8List bytes) runner,
}) async {
  // Warmup single run
  await runner(payloadBytes);

  final rssBefore = _currentRssMb();
  double rssPeak = rssBefore;

  double maxLagMs = 0.0;
  final lagStopwatch = Stopwatch()..start();
  int lastTickMicros = lagStopwatch.elapsedMicroseconds;
  const pollIntervalMicros = 2000; // 2ms polling

  final timer = Timer.periodic(const Duration(milliseconds: 2), (_) {
    final nowMicros = lagStopwatch.elapsedMicroseconds;
    final deltaMicros = nowMicros - lastTickMicros;
    lastTickMicros = nowMicros;
    final lagMs = math.max(0.0, (deltaMicros - pollIntervalMicros) / 1000.0);
    if (lagMs > maxLagMs) {
      maxLagMs = lagMs;
    }
    final currentRss = _currentRssMb();
    if (currentRss > rssPeak) {
      rssPeak = currentRss;
    }
  });

  final latencies = <int>[];
  final totalSw = Stopwatch()..start();

  try {
    final futures = List<Future<BenchmarkPayloadModel>>.generate(
      concurrency,
      (_) async {
        // Yield once so the event-loop lag timer can observe Main Isolate stalls
        await Future<void>.delayed(Duration.zero);
        final reqSw = Stopwatch()..start();
        final model = await runner(payloadBytes);
        reqSw.stop();
        latencies.add(reqSw.elapsedMilliseconds);
        return model;
      },
    );
    final models = await Future.wait(futures);
    expect(models, hasLength(concurrency));
    expect(models.first.itemCount, greaterThan(0));
  } finally {
    totalSw.stop();
    timer.cancel();
    lagStopwatch.stop();
  }

  final rssAfter = _currentRssMb();
  if (rssAfter > rssPeak) {
    rssPeak = rssAfter;
  }

  final avgLatency =
      latencies.reduce((a, b) => a + b) / latencies.length;
  final maxLatency = latencies.reduce(math.max);

  return _MicroBenchmarkMetrics(
    workloadLabel: workloadLabel,
    strategyLabel: strategyLabel,
    totalElapsedMs: totalSw.elapsedMilliseconds,
    avgLatencyMs: avgLatency,
    maxLatencyMs: maxLatency,
    maxEventLoopLagMs: maxLagMs,
    rssPeakDeltaMb: math.max(0.0, rssPeak - rssBefore),
  );
}

void main() {
  test('Dart-layer Transform & Mapper Performance Benchmark', () async {
    final backgroundTransformer = BackgroundTransformer();
    final turboTransformer = TurboTransformer();

    final workloads = <({String label, int sizeKb, int concurrency})>[
      (label: '10KB x 20 (Small / <24KB)', sizeKb: 10, concurrency: 20),
      (label: '100KB x 15 (Medium / >=24KB)', sizeKb: 100, concurrency: 15),
      (label: '500KB x 10 (Large / >=24KB)', sizeKb: 500, concurrency: 10),
    ];

    final allResults = <_MicroBenchmarkMetrics>[];

    for (final w in workloads) {
      final payloadBytes = _generatePayloadBytes(w.sizeKb);

      // ① Dio BackgroundTransformer + Main Isolate model mapping
      final bgMetrics = await _measureWorkload(
        workloadLabel: w.label,
        strategyLabel: '1. Dio BackgroundTransformer + Main Mapper',
        payloadBytes: payloadBytes,
        concurrency: w.concurrency,
        runner: (bytes) async {
          final raw = await backgroundTransformer.transformResponse(
            RequestOptions(path: '/bench', responseType: ResponseType.json),
            ResponseBody.fromBytes(
              bytes,
              200,
              headers: <String, List<String>>{
                Headers.contentTypeHeader: <String>[Headers.jsonContentType],
              },
            ),
          );
          return BenchmarkPayloadModel.fromJson(raw);
        },
      );
      allResults.add(bgMetrics);

      // ② TurboTransformer + Main Isolate model mapping
      final turboMetrics = await _measureWorkload(
        workloadLabel: w.label,
        strategyLabel: '2. TurboTransformer (Zero-Copy JSON)',
        payloadBytes: payloadBytes,
        concurrency: w.concurrency,
        runner: (bytes) async {
          final raw = await turboTransformer.transformResponse(
            RequestOptions(path: '/bench', responseType: ResponseType.json),
            ResponseBody.fromBytes(bytes, 200),
          );
          return BenchmarkPayloadModel.fromJson(raw);
        },
      );
      allResults.add(turboMetrics);

      // ③ TurboTransformer + TurboOptions(mapper) (end-to-end inside Worker Isolate)
      final turboMapperOptions = TurboOptions<BenchmarkPayloadModel>(
        responseType: ResponseType.json,
        mapper: BenchmarkPayloadModel.fromJson,
      ).compose(BaseOptions(), '/bench');

      final turboMapperMetrics = await _measureWorkload(
        workloadLabel: w.label,
        strategyLabel: '3. TurboTransformer + TurboOptions(mapper)',
        payloadBytes: payloadBytes,
        concurrency: w.concurrency,
        runner: (bytes) async {
          final model = await turboTransformer.transformResponse(
            turboMapperOptions,
            ResponseBody.fromBytes(bytes, 200),
          );
          return model as BenchmarkPayloadModel;
        },
      );
      allResults.add(turboMapperMetrics);
    }

    final buffer = StringBuffer()
      ..writeln()
      ..writeln('=== Dart-Layer Transform & Mapper Performance Benchmark ===')
      ..writeln(
        '| Workload | Strategy | Total (ms) | Avg / Max Req (ms) | Max Main-Isolate Lag (ms) | RSS Peak +MB |',
      )
      ..writeln(
        '| :--- | :--- | ---: | ---: | ---: | ---: |',
      );

    for (final m in allResults) {
      buffer.writeln(
        '| ${m.workloadLabel} | ${m.strategyLabel} | ${m.totalElapsedMs} | '
        '${m.avgLatencyMs.toStringAsFixed(1)} / ${m.maxLatencyMs} | '
        '${m.maxEventLoopLagMs.toStringAsFixed(1)} | '
        '+${m.rssPeakDeltaMb.toStringAsFixed(1)} |',
      );
    }
    debugPrint(buffer.toString());
  });
}
