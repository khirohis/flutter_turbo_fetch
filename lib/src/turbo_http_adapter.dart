import 'dart:io';
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'package:dio/io.dart';

/// ネイティブ層（Android OkHttp3 / iOS URLSession）の一括受信を利用する Dio 用 HttpClientAdapter。
class TurboHttpAdapter implements HttpClientAdapter {
  final HttpClientAdapter _fallbackAdapter;

  /// 1ホストあたりの最大同時リクエスト数（デフォルト 32）。
  /// Android (OkHttp Dispatcher) の maxRequests (64) を超える値が指定された場合、
  /// ネイティブ側で maxRequests も自動的に同値まで引き上げられる。
  final int maxRequestsPerHost;

  /// ローカル検証環境等で自己署名証明書を許可するかどうかのフラグ（デフォルト false）。
  final bool allowBadCertificates;

  TurboHttpAdapter({
    this.maxRequestsPerHost = 32,
    this.allowBadCertificates = false,
    HttpClientAdapter? fallbackAdapter,
  }) : _fallbackAdapter = fallbackAdapter ?? IOHttpClientAdapter();

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

    // Step 1-5 でネイティブ層（Pigeon ブリッジ）との結合を実装する。
    throw UnimplementedError('Native fetch is not connected yet.');
  }

  @override
  void close({bool force = false}) {
    _fallbackAdapter.close(force: force);
  }
}
