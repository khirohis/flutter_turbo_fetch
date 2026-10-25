import 'dart:io';
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'package:dio/io.dart';

/// ネイティブ層（Android OkHttp3 / iOS URLSession）の一括受信を利用する Dio 用 HttpClientAdapter。
class TurboHttpAdapter implements HttpClientAdapter {
  final HttpClientAdapter _fallbackAdapter;

  TurboHttpAdapter({HttpClientAdapter? fallbackAdapter})
      : _fallbackAdapter = fallbackAdapter ?? IOHttpClientAdapter();

  @override
  Future<ResponseBody> fetch(
    RequestOptions options,
    Stream<Uint8List>? requestStream,
    Future<void>? cancelFuture,
  ) async {
    if (!Platform.isAndroid && !Platform.isIOS) {
      throw UnsupportedError(
        'TurboHttpAdapter is only supported on Android and iOS.',
      );
    }

    if (options.responseType == ResponseType.stream) {
      return _fallbackAdapter.fetch(options, requestStream, cancelFuture);
    }

    // Step 1-3 〜 1-5 でネイティブ層（Pigeon ブリッジ）との連携を実装する。
    throw UnimplementedError('Native fetch is not implemented yet.');
  }

  @override
  void close({bool force = false}) {
    _fallbackAdapter.close(force: force);
  }
}
