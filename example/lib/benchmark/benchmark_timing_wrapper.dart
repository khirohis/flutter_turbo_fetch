import 'dart:async';
import 'dart:typed_data';
import 'package:dio/dio.dart';

const String kBenchmarkNetTimeMicrosKey = '_benchmark_net_time_micros';
const String kBenchmarkParseTimeMicrosKey = '_benchmark_parse_time_micros';

/// 既存のアダプタをラップし、純粋な通信時間（Net Time）をマイクロ秒精度で計測するアダプタ
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

/// 既存の Transformer をラップし、JSON パース完了待ち時間（Parse Time）を
/// マイクロ秒精度で計測する Transformer（セマフォ待ち時間も包含）
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

/// Response オブジェクトから計測された Net Time (ms) と Parse Time (ms) を抽出する
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
