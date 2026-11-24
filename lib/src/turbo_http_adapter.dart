import 'dart:async';
import 'dart:isolate';
import 'dart:io';
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'messages.g.dart';
import 'turbo_options.dart';

/// A high-performance Dio [HttpClientAdapter] that leverages platform-native
/// batch HTTP/2 reception (Android OkHttp3 / iOS & macOS URLSession / Windows WinHTTP / Linux libcurl).
class TurboHttpAdapter implements HttpClientAdapter {
  final TurboFetchHostApi _hostApi;
  final bool _overridePlatformCheck;

  static final String _isolatePrefix = 'turbo_${Isolate.current.hashCode}_';
  static final bool _isSupportedPlatform =
      Platform.isAndroid ||
      Platform.isIOS ||
      Platform.isMacOS ||
      Platform.isWindows ||
      Platform.isLinux;
  static final Uint8List _emptyBytes = Uint8List(0);
  static int _requestCounter = 0;
  final _activeRequests = <String, bool>{};
  bool _closed = false;

  /// Whether to allow self-signed or invalid certificates for local testing (default: false).
  final bool allowBadCertificates;

  TurboHttpAdapter({
    this.allowBadCertificates = false,
    TurboFetchHostApi? hostApi,
  })  : _hostApi = hostApi ?? TurboFetchHostApi(),
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

    if (!_overridePlatformCheck && !_isSupportedPlatform) {
      throw UnsupportedError(
        'TurboHttpAdapter is only supported on Android, iOS, macOS, Windows, and Linux.',
      );
    }

    // Aggregate request body stream into a single byte array (unbounded size)
    Uint8List? requestBody;
    if (requestStream != null) {
      final builder = BytesBuilder(copy: false);
      await for (final chunk in requestStream) {
        builder.add(chunk);
      }
      requestBody = builder.takeBytes();
    }

    final maxResponseBytes =
        options.extra[turboMaxResponseBytesExtraKey] as int?;
    final requestId = '$_isolatePrefix${++_requestCounter}';
    final turboRequest = _buildRequest(
      requestId: requestId,
      options: options,
      requestBody: requestBody,
      maxResponseBytes: maxResponseBytes,
    );

    _activeRequests[requestId] = false;
    final TurboHttpResponse response;
    bool isCancelled = false;
    try {
      cancelFuture?.whenComplete(() => _cancelRequest(requestId));
      response = await _hostApi.executeRequest(turboRequest);
    } catch (e, stackTrace) {
      final cancelled = _activeRequests[requestId] == true;
      throw DioException(
        requestOptions: options,
        error: e,
        stackTrace: stackTrace,
        type: cancelled ? DioExceptionType.cancel : DioExceptionType.unknown,
        message: e.toString(),
      );
    } finally {
      isCancelled = _activeRequests.remove(requestId) == true;
    }

    _validateResponse(
      response,
      options: options,
      isCancelled: isCancelled,
      maxResponseBytes: maxResponseBytes,
    );

    return _buildResponseBody(response);
  }

  TurboHttpRequest _buildRequest({
    required String requestId,
    required RequestOptions options,
    required Uint8List? requestBody,
    required int? maxResponseBytes,
  }) {
    final headers = <String?, String?>{};
    for (final MapEntry(:key, :value) in options.headers.entries) {
      if (value != null) {
        if (value is Iterable) {
          headers[key] = value.join(', ');
        } else {
          headers[key] = value is String ? value : value.toString();
        }
      }
    }

    return TurboHttpRequest(
      requestId: requestId,
      url: options.uri.toString(),
      method: options.method.toUpperCase(),
      headers: headers,
      body: requestBody,
      connectTimeoutMs: options.connectTimeout?.inMilliseconds,
      receiveTimeoutMs: options.receiveTimeout?.inMilliseconds,
      sendTimeoutMs: options.sendTimeout?.inMilliseconds,
      followRedirects: options.followRedirects,
      allowBadCertificates: allowBadCertificates,
      maxResponseBytes: maxResponseBytes,
    );
  }

  void _cancelRequest(String requestId) {
    if (_activeRequests[requestId] == false) {
      _activeRequests[requestId] = true;
      unawaited(_hostApi.cancelRequest(requestId));
    }
  }

  void _validateResponse(
    TurboHttpResponse response, {
    required RequestOptions options,
    required bool isCancelled,
    required int? maxResponseBytes,
  }) {
    if (response.errorCode != null) {
      final errorCode = isCancelled ? 'cancel' : response.errorCode!;
      final errorMessage = _resolveErrorMessage(
        errorCode: errorCode,
        rawMessage: response.errorMessage,
        maxResponseBytes: maxResponseBytes,
      );
      throw DioException(
        requestOptions: options,
        type: _mapErrorCode(errorCode),
        message: errorMessage,
        error: errorMessage,
      );
    }

    if (isCancelled) {
      throw DioException(
        requestOptions: options,
        type: DioExceptionType.cancel,
        message: 'Request was cancelled.',
      );
    }
  }

  ResponseBody _buildResponseBody(TurboHttpResponse response) {
    final responseHeaders = <String, List<String>>{};
    final rawHeaders = response.headers;
    if (rawHeaders != null) {
      for (final MapEntry(:key, value: rawValues) in rawHeaders.entries) {
        if (key != null && rawValues != null) {
          final normalizedKey = key.toLowerCase();
          if (rawValues is Iterable) {
            responseHeaders[normalizedKey] = <String>[
              for (final e in rawValues)
                if (e != null) e is String ? e : e.toString(),
            ];
          } else {
            responseHeaders[normalizedKey] = <String>[
              rawValues is String ? rawValues : rawValues.toString(),
            ];
          }
        }
      }
    }

    return ResponseBody.fromBytes(
      response.body ?? _emptyBytes,
      response.statusCode ?? HttpStatus.ok,
      statusMessage: response.statusMessage,
      headers: responseHeaders,
    );
  }

  String? _resolveErrorMessage({
    required String errorCode,
    required String? rawMessage,
    required int? maxResponseBytes,
  }) {
    if (errorCode == 'maxResponseBytesExceeded') {
      if (rawMessage != null && rawMessage.isNotEmpty) {
        return rawMessage;
      }
      return 'Response body exceeded maxResponseBytes limit (${maxResponseBytes ?? 0} bytes)';
    }
    return rawMessage;
  }

  DioExceptionType _mapErrorCode(String errorCode) {
    switch (errorCode) {
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
      case 'maxResponseBytesExceeded':
      default:
        return DioExceptionType.unknown;
    }
  }

  @override
  void close({bool force = false}) {
    _closed = true;
    if (force) {
      for (final requestId in _activeRequests.keys.toList()) {
        _cancelRequest(requestId);
      }
    }
  }
}
