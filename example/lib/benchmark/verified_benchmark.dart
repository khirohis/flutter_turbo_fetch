import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

import 'package:dio/dio.dart';
import 'package:flutter/services.dart';

import 'benchmark_models.dart';
import 'benchmark_runner.dart';
import 'benchmark_timing_wrapper.dart';

Map<String, double>? summarizeSamples(List<double> values) {
  if (values.isEmpty) return null;
  final sorted = [...values]..sort();
  final mean = sorted.reduce((a, b) => a + b) / sorted.length;
  final middle = sorted.length ~/ 2;
  return {
    'mean': mean,
    'median': sorted.length.isOdd
        ? sorted[middle]
        : (sorted[middle - 1] + sorted[middle]) / 2,
    'p95': sorted[(sorted.length * .95).ceil() - 1],
    'min': sorted.first,
    'max': sorted.last,
    'sample_stddev': sorted.length < 2
        ? 0
        : math.sqrt(
            sorted.map((v) => math.pow(v - mean, 2)).reduce((a, b) => a + b) /
                (sorted.length - 1)),
  };
}

List<BenchmarkPreset> rotatedPresets(int trial) => List.generate(
      BenchmarkPreset.values.length,
      (index) => BenchmarkPreset
          .values[(index + trial) % BenchmarkPreset.values.length],
    );

void validatePayload(dynamic value, int sizeKiB) {
  if (value is! Map<String, dynamic> ||
      value['sizeKb'] != sizeKiB ||
      value['items'] is! List ||
      value['count'] is! int ||
      value['count'] <= 0 ||
      (value['items'] as List).length != value['count']) {
    throw const FormatException('Invalid benchmark payload');
  }
}

int? _rss() {
  try {
    return ProcessInfo.currentRss;
  } catch (_) {
    return null;
  }
}

/// Separates complete-body reception from decoding for all three adapters.
/// This is a controlled split-pipeline experiment, not the stock Dio pipeline.
class VerifiedBenchmarkClient {
  VerifiedBenchmarkClient(
      this.preset, String baseUrl, bool allowLocalCertificate)
      : dio = BenchmarkRunner.createBenchmarkDio(
            preset: preset, allowBadCertificates: allowLocalCertificate) {
    transformer =
        (dio.transformer as BenchmarkTimingTransformer).delegateTransformer;
    dio.httpClientAdapter =
        (dio.httpClientAdapter as BenchmarkTimingAdapter).delegateAdapter;
    dio.transformer = SyncTransformer();
    dio.options.baseUrl = baseUrl;
  }

  final BenchmarkPreset preset;
  final Dio dio;
  late final Transformer transformer;

  Future<Map<String, Object?>> run(
      BenchmarkScenario scenario, int trial, int position) async {
    final rows = <Map<String, Object?>>[];
    final startRss = _rss();
    int? peakRss = startRss;
    var rssSamples = 0;
    void sample() {
      final value = _rss();
      if (value != null) {
        rssSamples++;
        peakRss = math.max(peakRss ?? value, value);
      }
    }

    final timer =
        Timer.periodic(const Duration(milliseconds: 25), (_) => sample());
    final tasks = [
      for (var round = 0; round < scenario.rounds; round++)
        for (final size in scenario.sizesKiB) (round: round, size: size)
    ];
    var next = 0;
    final wall = Stopwatch()..start();
    Future<void> worker() async {
      while (next < tasks.length) {
        final sequence = next++;
        final task = tasks[sequence];
        final total = Stopwatch()..start();
        double? networkMs;
        double? parseMs;
        int? status;
        int? bytes;
        String? protocol;
        String? error;
        try {
          final network = Stopwatch()..start();
          final response = await dio.get<Uint8List>('/json',
              queryParameters: {
                'sizeKb': task.size,
                'trial': trial,
                'scenario': scenario.id,
                'preset': preset.id,
                'seq': sequence
              },
              options: Options(responseType: ResponseType.bytes));
          network.stop();
          networkMs = network.elapsedMicroseconds / 1000;
          status = response.statusCode;
          final body = response.data!;
          bytes = body.length;
          protocol = response.headers.value('x-http-version');
          final expectedProtocol =
              preset == BenchmarkPreset.ioHttp1 ? '1.1' : '2.0';
          if (status != 200 ||
              protocol != expectedProtocol ||
              response.headers.value('content-length') != '$bytes') {
            throw const FormatException('Invalid response metadata');
          }
          final parse = Stopwatch()..start();
          final decoded = await transformer.transformResponse(
              response.requestOptions.copyWith(responseType: ResponseType.json),
              ResponseBody.fromBytes(body, 200, headers: {
                Headers.contentTypeHeader: ['application/json; charset=utf-8'],
              }));
          parse.stop();
          parseMs = parse.elapsedMicroseconds / 1000;
          validatePayload(decoded, task.size);
        } on DioException catch (e) {
          status = e.response?.statusCode;
          error = e.type.name;
        } on FormatException {
          error = 'invalid_payload_or_protocol';
        } catch (_) {
          error = 'unexpected';
        }
        total.stop();
        rows.add({
          'sequence': sequence,
          'round': task.round,
          'size_kib': task.size,
          'status': status,
          'response_bytes': bytes,
          'http_version': protocol,
          'network_ms': networkMs,
          'parse_ms': parseMs,
          'request_ms': total.elapsedMicroseconds / 1000,
          'error': error,
        });
      }
    }

    try {
      await Future.wait(List.generate(scenario.concurrency, (_) => worker()));
    } finally {
      wall.stop();
      timer.cancel();
      sample();
    }
    rows.sort((a, b) => (a['sequence'] as int).compareTo(b['sequence'] as int));
    final successes = rows.where((row) => row['error'] == null).toList();
    final elapsedMs = wall.elapsedMicroseconds / 1000;
    return {
      'schema': 1,
      'mode': 'release',
      'scenario': scenario.id,
      'preset': preset.id,
      'trial': trial,
      'warmup': trial < 0,
      'position': position,
      'total_ms': elapsedMs,
      'success': successes.length,
      'failure': rows.length - successes.length,
      'rps': elapsedMs > 0 ? successes.length * 1000 / elapsedMs : null,
      'network_ms': summarizeSamples(
          successes.map((r) => r['network_ms'] as double).toList()),
      'parse_ms': summarizeSamples(
          successes.map((r) => r['parse_ms'] as double).toList()),
      'rss_start_bytes': startRss,
      'rss_peak_bytes': peakRss,
      'rss_delta_bytes':
          startRss == null || peakRss == null ? null : peakRss! - startRss,
      'rss_samples': rssSamples,
      'jank_frames': null,
      'max_frame_ms': null,
      'requests': rows,
    };
  }

  void close() => dio.close(force: true);
}
