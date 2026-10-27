import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

import 'package:dio/dio.dart';
import 'package:dio/io.dart';
import 'package:dio_http2_adapter/dio_http2_adapter.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:native_dio_adapter/native_dio_adapter.dart';

import 'benchmark_models.dart';
import 'benchmark_timing_wrapper.dart';

/// ベンチマーク実行エンジン
class BenchmarkRunner {
  /// プロセスの現在 RSS (MiB) を取得
  static double currentRssMiB() {
    try {
      return ProcessInfo.currentRss / (1024 * 1024);
    } catch (_) {
      return 0.0;
    }
  }

  /// 95 パーセンタイル (P95) を計算
  static double calculateP95(List<double> values) {
    if (values.isEmpty) return 0.0;
    final sorted = List<double>.from(values)..sort();
    final index = (sorted.length * 0.95).ceil() - 1;
    return sorted[index.clamp(0, sorted.length - 1)];
  }

  /// 平均値を計算
  static double calculateAvg(List<double> values) {
    if (values.isEmpty) return 0.0;
    final sum = values.fold<double>(0.0, (acc, v) => acc + v);
    return sum / values.length;
  }

  /// 指定プリセットに対応する Dio インスタンスを構築（計測ラッパー付き）
  static Dio createBenchmarkDio({
    required BenchmarkPreset preset,
    required bool allowBadCertificates,
  }) {
    final dio = Dio(
      BaseOptions(
        connectTimeout: const Duration(seconds: 15),
        receiveTimeout: const Duration(seconds: 30),
        sendTimeout: const Duration(seconds: 15),
      ),
    );

    late final HttpClientAdapter rawAdapter;
    late final Transformer rawTransformer;

    switch (preset) {
      case BenchmarkPreset.ioHttp1:
        rawAdapter = IOHttpClientAdapter(
          createHttpClient: () {
            final client = HttpClient();
            if (allowBadCertificates) {
              client.badCertificateCallback = (cert, host, port) => true;
            }
            return client;
          },
        );
        rawTransformer = BackgroundTransformer();

      case BenchmarkPreset.dioHttp2:
        rawAdapter = Http2Adapter(
          ConnectionManager(
            idleTimeout: const Duration(seconds: 15),
            onClientCreate: (_, config) {
              if (allowBadCertificates) {
                config.onBadCertificate = (_) => true;
              }
            },
          ),
        );
        rawTransformer = BackgroundTransformer();

      case BenchmarkPreset.nativeDio:
        rawAdapter = NativeAdapter();
        rawTransformer = BackgroundTransformer();

      case BenchmarkPreset.turboFetch:
        rawAdapter = TurboHttpAdapter(
          allowBadCertificates: allowBadCertificates,
        );
        rawTransformer = TurboTransformer();
    }

    dio.httpClientAdapter = BenchmarkTimingAdapter(rawAdapter);
    dio.transformer = BenchmarkTimingTransformer(rawTransformer);

    return dio;
  }

  /// シナリオを実行し、結果（BenchmarkScenarioResult）を返却する
  static Future<BenchmarkScenarioResult> runScenario({
    required BenchmarkScenario scenario,
    required BenchmarkPreset preset,
    required String baseUrl,
    bool allowBadCertificates = true,
    void Function(int completed, int total)? onProgress,
  }) async {
    final dio = createBenchmarkDio(
      preset: preset,
      allowBadCertificates: allowBadCertificates,
    );

    // フレーム監視の初期化
    int totalFrames = 0;
    int jankFrames = 0;
    double maxFrameDurationMs = 0.0;
    bool collectingFrames = true;

    void onFrameTimings(List<FrameTiming> timings) {
      if (!collectingFrames) return;
      for (final timing in timings) {
        totalFrames++;
        final spanMs = timing.totalSpan.inMicroseconds / 1000.0;
        if (spanMs > 16.67) {
          jankFrames++;
        }
        if (spanMs > maxFrameDurationMs) {
          maxFrameDurationMs = spanMs;
        }
      }
    }

    final scheduler = SchedulerBinding.instance;
    scheduler.addTimingsCallback(onFrameTimings);

    // RSS メモリ監視タイマー（25ms 周期）
    final rssBefore = currentRssMiB();
    double rssPeak = rssBefore;

    final rssTimer = Timer.periodic(const Duration(milliseconds: 25), (_) {
      final current = currentRssMiB();
      if (current > rssPeak) {
        rssPeak = current;
      }
    });

    // タスクリストの生成（全サイズ順次 × rounds）
    final tasks = <({int round, int sizeKiB})>[];
    for (int r = 0; r < scenario.rounds; r++) {
      for (final size in scenario.sizesKiB) {
        tasks.add((round: r, sizeKiB: size));
      }
    }

    final totalTasks = tasks.length;
    final requestMetrics = <SingleRequestMetric>[];
    int completedCount = 0;

    final overallStopwatch = Stopwatch()..start();

    try {
      // 並列プールワーカー（最大 scenario.concurrency 多重）
      int nextTaskIndex = 0;

      Future<void> worker() async {
        while (true) {
          final int taskIndex;
          if (nextTaskIndex >= tasks.length) {
            break;
          }
          taskIndex = nextTaskIndex++;
          final task = tasks[taskIndex];

          final reqStopwatch = Stopwatch()..start();
          try {
            final separator = baseUrl.contains('?') ? '&' : '?';
            final requestUrl =
                '$baseUrl/json?sizeKb=${task.sizeKiB}${separator}seq=$taskIndex&round=${task.round}';

            final response = await dio.get<dynamic>(
              requestUrl,
              options: Options(responseType: ResponseType.json),
            );
            reqStopwatch.stop();

            final timings = extractBenchmarkTimings(response);
            requestMetrics.add(
              SingleRequestMetric(
                sizeKiB: task.sizeKiB,
                roundIndex: task.round,
                netTimeMs: timings.netTimeMs,
                parseTimeMs: timings.parseTimeMs,
                totalTimeMs: reqStopwatch.elapsedMicroseconds / 1000.0,
                statusCode: response.statusCode ?? 200,
              ),
            );
          } catch (e) {
            reqStopwatch.stop();
            int statusCode = 0;
            if (e is DioException && e.response?.statusCode != null) {
              statusCode = e.response!.statusCode!;
            }
            requestMetrics.add(
              SingleRequestMetric(
                sizeKiB: task.sizeKiB,
                roundIndex: task.round,
                netTimeMs: 0.0,
                parseTimeMs: 0.0,
                totalTimeMs: reqStopwatch.elapsedMicroseconds / 1000.0,
                statusCode: statusCode,
                errorMessage: e.toString(),
              ),
            );
          } finally {
            completedCount++;
            onProgress?.call(completedCount, totalTasks);
          }
        }
      }

      final workerFutures = List<Future<void>>.generate(
        math.min(scenario.concurrency, tasks.length),
        (_) => worker(),
      );

      await Future.wait(workerFutures);
    } finally {
      overallStopwatch.stop();
      rssTimer.cancel();
      collectingFrames = false;
      scheduler.removeTimingsCallback(onFrameTimings);
      dio.close();
    }

    final totalWallTimeMs = overallStopwatch.elapsedMilliseconds;
    final rssDelta = math.max(0.0, rssPeak - rssBefore);

    // 成功したリクエストから統計値を算出
    final successMetrics = requestMetrics.where((m) => m.isSuccess).toList();
    final netTimes = successMetrics.map((m) => m.netTimeMs).toList();
    final parseTimes = successMetrics.map((m) => m.parseTimeMs).toList();

    return BenchmarkScenarioResult(
      scenario: scenario,
      preset: preset,
      totalWallTimeMs: totalWallTimeMs,
      netTimeAvgMs: calculateAvg(netTimes),
      netTimeP95Ms: calculateP95(netTimes),
      parseTimeAvgMs: calculateAvg(parseTimes),
      parseTimeP95Ms: calculateP95(parseTimes),
      totalFrames: totalFrames,
      jankFrames: jankFrames,
      maxFrameDurationMs: maxFrameDurationMs,
      rssBeforeMiB: rssBefore,
      rssPeakMiB: rssPeak,
      rssDeltaMiB: rssDelta,
      successCount: successMetrics.length,
      failureCount: requestMetrics.length - successMetrics.length,
      requestMetrics: requestMetrics,
    );
  }
}
