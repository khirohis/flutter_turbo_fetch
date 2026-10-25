import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'package:dio/io.dart';
import 'messages.g.dart';

/// ネイティブ層（Android OkHttp3 / iOS URLSession）の一括受信を利用する Dio 用 HttpClientAdapter。
class TurboHttpAdapter implements HttpClientAdapter {
  final HttpClientAdapter _fallbackAdapter;
  final TurboFetchHostApi _hostApi;
  final bool _overridePlatformCheck;

  int _requestCounter = 0;
  bool _closed = false;

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
    TurboFetchHostApi? hostApi,
  })  : _fallbackAdapter = fallbackAdapter ?? IOHttpClientAdapter(),
        _hostApi = hostApi ?? TurboFetchHostApi(),
        _overridePlatformCheck = hostApi != null;

  @override
  Future<ResponseBody> fetch(
    RequestOptions options,
    Stream<Uint8List>? requestStream,
    Future<void>? cancelFuture,
  ) async {
    if (_closed) {
      throw StateError(
        "Can't establish connection after the TurboHttpAdapter was closed.",
      );
    }

    if (!_overridePlatformCheck && !Platform.isAndroid && !Platform.isIOS) {
      throw UnsupportedError(
        'TurboHttpAdapter is only supported on Android and iOS.',
      );
    }

    // SSE / ストリーミング受信が要求された場合は標準 IOHttpClientAdapter へ自動フォールバック
    if (options.responseType == ResponseType.stream) {
      return _fallbackAdapter.fetch(options, requestStream, cancelFuture);
    }

    // リクエストボディの集約（サイズ制限なし）
    Uint8List? requestBody;
    if (requestStream != null) {
      final builder = BytesBuilder(copy: false);
      await for (final chunk in requestStream) {
        builder.add(chunk);
      }
      requestBody = builder.takeBytes();
    }

    // リクエストヘッダの変換
    final headers = <String?, String?>{};
    options.headers.forEach((key, value) {
      if (value != null) {
        if (value is Iterable) {
          headers[key] = value.map((e) => e.toString()).join(', ');
        } else {
          headers[key] = value.toString();
        }
      }
    });

    final requestId =
        'turbo_${++_requestCounter}_${DateTime.now().microsecondsSinceEpoch}';

    bool isCompleted = false;
    if (cancelFuture != null) {
      cancelFuture.whenComplete(() {
        if (!isCompleted) {
          unawaited(_hostApi.cancelRequest(requestId));
        }
      });
    }

    final turboRequest = TurboHttpRequest(
      requestId: requestId,
      url: options.uri.toString(),
      method: options.method.toUpperCase(),
      headers: headers,
      body: requestBody,
      connectTimeoutMs: options.connectTimeout?.inMilliseconds,
      receiveTimeoutMs: options.receiveTimeout?.inMilliseconds,
      sendTimeoutMs: options.sendTimeout?.inMilliseconds,
      followRedirects: options.followRedirects,
      maxRedirects: options.maxRedirects,
      maxRequestsPerHost: maxRequestsPerHost,
      allowBadCertificates: allowBadCertificates,
    );

    final TurboHttpResponse response;
    try {
      response = await _hostApi.executeRequest(turboRequest);
    } catch (e, stackTrace) {
      isCompleted = true;
      throw DioException(
        requestOptions: options,
        error: e,
        stackTrace: stackTrace,
        type: DioExceptionType.unknown,
        message: e.toString(),
      );
    }
    isCompleted = true;

    if (response.errorType != null) {
      throw DioException(
        requestOptions: options,
        type: _mapErrorType(response.errorType!),
        message: response.errorMessage,
        error: response.errorMessage,
      );
    }

    final responseHeaders = <String, List<String>>{};
    response.headers?.forEach((key, values) {
      if (key != null && values != null) {
        responseHeaders[key.toLowerCase()] =
            values.whereType<String>().toList();
      }
    });

    return ResponseBody.fromBytes(
      response.body ?? Uint8List(0),
      response.statusCode ?? 200,
      statusMessage: response.statusMessage,
      headers: responseHeaders,
    );
  }

  DioExceptionType _mapErrorType(String errorType) {
    switch (errorType) {
      case 'cancel':
        return DioExceptionType.cancel;
      case 'connectionTimeout':
        return DioExceptionType.connectionTimeout;
      case 'sendTimeout':
        return DioExceptionType.sendTimeout;
      case 'receiveTimeout':
        return DioExceptionType.receiveTimeout;
      case 'badCertificate':
        return DioExceptionType.badCertificate;
      case 'connectionError':
        return DioExceptionType.connectionError;
      default:
        return DioExceptionType.unknown;
    }
  }

  @override
  void close({bool force = false}) {
    _closed = true;
    _fallbackAdapter.close(force: force);
  }
}
