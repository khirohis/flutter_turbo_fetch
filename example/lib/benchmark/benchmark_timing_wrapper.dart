import 'dart:async';
import 'dart:typed_data';
import 'package:dio/dio.dart';

const String kBenchmarkNetTimeMicrosKey = '_benchmark_net_time_micros';
const String kBenchmarkParseTimeMicrosKey = '_benchmark_parse_time_micros';

/// Wrapper adapter that measures network transfer latency (Net Time) with microsecond precision.
class BenchmarkTimingAdapter implements HttpClientAdapter {
  BenchmarkTimingAdapter(this.delegateAdapter);

  final HttpClientAdapter delegateAdapter;

  @override
  Future<ResponseBody> fetch(
    RequestOptions options,
    Stream<Uint8List>? requestStream,
    Future<void>? cancelFuture,
  ) async {
    final sw = Stopwatch()..start();
    try {
      final responseBody = await delegateAdapter.fetch(
        options,
        requestStream,
        cancelFuture,
      );
      sw.stop();
      options.extra[kBenchmarkNetTimeMicrosKey] = sw.elapsedMicroseconds;
      return responseBody;
    } catch (_) {
      sw.stop();
      options.extra[kBenchmarkNetTimeMicrosKey] = sw.elapsedMicroseconds;
      rethrow;
    }
  }

  @override
  void close({bool force = false}) {
    delegateAdapter.close(force: force);
  }
}

/// Wrapper transformer that measures response transformation latency (Parse Time)
/// with microsecond precision (including any semaphore queue waiting time).
class BenchmarkTimingTransformer implements Transformer {
  BenchmarkTimingTransformer(this.delegateTransformer);

  final Transformer delegateTransformer;

  @override
  Future<String> transformRequest(RequestOptions options) {
    return delegateTransformer.transformRequest(options);
  }

  @override
  Future transformResponse(
    RequestOptions options,
    ResponseBody responseBody,
  ) async {
    final sw = Stopwatch()..start();
    try {
      final result = await delegateTransformer.transformResponse(
        options,
        responseBody,
      );
      sw.stop();
      options.extra[kBenchmarkParseTimeMicrosKey] = sw.elapsedMicroseconds;
      return result;
    } catch (_) {
      sw.stop();
      options.extra[kBenchmarkParseTimeMicrosKey] = sw.elapsedMicroseconds;
      rethrow;
    }
  }
}

/// Extracts measured Net Time (ms) and Parse Time (ms) from a [Response] object.
({double netTimeMs, double parseTimeMs}) extractBenchmarkTimings(
  Response<dynamic> response,
) {
  final netMicros =
      (response.requestOptions.extra[kBenchmarkNetTimeMicrosKey] as int?) ?? 0;
  final parseMicros =
      (response.requestOptions.extra[kBenchmarkParseTimeMicrosKey] as int?) ??
      0;
  return (
    netTimeMs: netMicros / 1000.0,
    parseTimeMs: parseMicros / 1000.0,
  );
}
