import 'dart:typed_data';
import 'package:dio/dio.dart';

typedef TurboResponseMapper<T> = T Function(dynamic data);

/// Key used by TurboTransformer to extract the custom response mapper from extra.
const String turboMapperExtraKey = '_turbo_response_mapper';

/// Key used by TurboHttpAdapter to extract the maximum response bytes limit from extra.
const String turboMaxResponseBytesExtraKey = '_turbo_max_response_bytes';

class TurboOptions<T> extends Options {
  TurboOptions({
    TurboResponseMapper<T>? mapper,
    int? maxResponseBytes,
    super.method,
    super.sendTimeout,
    super.receiveTimeout,
    super.extra,
    super.headers,
    super.preserveHeaderCase,
    super.responseType = ResponseType.json,
    super.contentType,
    super.validateStatus,
    super.receiveDataWhenStatusError,
    super.followRedirects,
    super.maxRedirects,
    super.persistentConnection,
    super.requestEncoder,
    super.responseDecoder,
    super.listFormat,
  }) : super() {
    if (mapper != null || maxResponseBytes != null) {
      extra = <String, Object?>{
        ...?extra,
        if (mapper != null) turboMapperExtraKey: mapper,
        if (maxResponseBytes != null)
          turboMaxResponseBytesExtraKey: maxResponseBytes,
      };
    }
  }

  /// Factory constructor for JSON payloads.
  ///
  /// Automatically binds [responseType] to [ResponseType.json] and feeds the
  /// decoded JSON structure (Map or List) into [mapper].
  factory TurboOptions.json({
    required T Function(dynamic json) mapper,
    int? maxResponseBytes,
    String? method,
    Duration? sendTimeout,
    Duration? receiveTimeout,
    Map<String, dynamic>? extra,
    Map<String, dynamic>? headers,
    bool? preserveHeaderCase,
    String? contentType,
    ValidateStatus? validateStatus,
    bool? receiveDataWhenStatusError,
    bool? followRedirects,
    int? maxRedirects,
    bool? persistentConnection,
    RequestEncoder? requestEncoder,
    ResponseDecoder? responseDecoder,
    ListFormat? listFormat,
  }) {
    return TurboOptions<T>(
      mapper: mapper,
      maxResponseBytes: maxResponseBytes,
      method: method,
      sendTimeout: sendTimeout,
      receiveTimeout: receiveTimeout,
      extra: extra,
      headers: headers,
      preserveHeaderCase: preserveHeaderCase,
      responseType: ResponseType.json,
      contentType: contentType,
      validateStatus: validateStatus,
      receiveDataWhenStatusError: receiveDataWhenStatusError,
      followRedirects: followRedirects,
      maxRedirects: maxRedirects,
      persistentConnection: persistentConnection,
      requestEncoder: requestEncoder,
      responseDecoder: responseDecoder,
      listFormat: listFormat,
    );
  }

  /// Factory constructor for raw binary payloads (e.g. Protobuf, MessagePack, FlatBuffers).
  ///
  /// Automatically binds [responseType] to [ResponseType.bytes] and statically
  /// types the [mapper] argument as [Uint8List].
  factory TurboOptions.bytes({
    required T Function(Uint8List bytes) mapper,
    int? maxResponseBytes,
    String? method,
    Duration? sendTimeout,
    Duration? receiveTimeout,
    Map<String, dynamic>? extra,
    Map<String, dynamic>? headers,
    bool? preserveHeaderCase,
    String? contentType,
    ValidateStatus? validateStatus,
    bool? receiveDataWhenStatusError,
    bool? followRedirects,
    int? maxRedirects,
    bool? persistentConnection,
    RequestEncoder? requestEncoder,
    ResponseDecoder? responseDecoder,
    ListFormat? listFormat,
  }) {
    return TurboOptions<T>(
      mapper: (dynamic data) => mapper(data as Uint8List),
      maxResponseBytes: maxResponseBytes,
      method: method,
      sendTimeout: sendTimeout,
      receiveTimeout: receiveTimeout,
      extra: extra,
      headers: headers,
      preserveHeaderCase: preserveHeaderCase,
      responseType: ResponseType.bytes,
      contentType: contentType,
      validateStatus: validateStatus,
      receiveDataWhenStatusError: receiveDataWhenStatusError,
      followRedirects: followRedirects,
      maxRedirects: maxRedirects,
      persistentConnection: persistentConnection,
      requestEncoder: requestEncoder,
      responseDecoder: responseDecoder,
      listFormat: listFormat,
    );
  }

  /// Factory constructor for plain text payloads (e.g. CSV, YAML, custom text formats).
  ///
  /// Automatically binds [responseType] to [ResponseType.plain] and statically
  /// types the [mapper] argument as [String].
  factory TurboOptions.plain({
    required T Function(String text) mapper,
    int? maxResponseBytes,
    String? method,
    Duration? sendTimeout,
    Duration? receiveTimeout,
    Map<String, dynamic>? extra,
    Map<String, dynamic>? headers,
    bool? preserveHeaderCase,
    String? contentType,
    ValidateStatus? validateStatus,
    bool? receiveDataWhenStatusError,
    bool? followRedirects,
    int? maxRedirects,
    bool? persistentConnection,
    RequestEncoder? requestEncoder,
    ResponseDecoder? responseDecoder,
    ListFormat? listFormat,
  }) {
    return TurboOptions<T>(
      mapper: (dynamic data) => mapper(data as String),
      maxResponseBytes: maxResponseBytes,
      method: method,
      sendTimeout: sendTimeout,
      receiveTimeout: receiveTimeout,
      extra: extra,
      headers: headers,
      preserveHeaderCase: preserveHeaderCase,
      responseType: ResponseType.plain,
      contentType: contentType,
      validateStatus: validateStatus,
      receiveDataWhenStatusError: receiveDataWhenStatusError,
      followRedirects: followRedirects,
      maxRedirects: maxRedirects,
      persistentConnection: persistentConnection,
      requestEncoder: requestEncoder,
      responseDecoder: responseDecoder,
      listFormat: listFormat,
    );
  }
}
