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
  }) {
    _attachExtras(mapper, maxResponseBytes);
  }

  /// Named constructor for JSON payloads.
  ///
  /// Automatically binds [responseType] to [ResponseType.json] and feeds the
  /// decoded JSON structure (Map or List) into [mapper].
  TurboOptions.json({
    required T Function(dynamic json) mapper,
    int? maxResponseBytes,
    super.method,
    super.sendTimeout,
    super.receiveTimeout,
    super.extra,
    super.headers,
    super.preserveHeaderCase,
    super.contentType,
    super.validateStatus,
    super.receiveDataWhenStatusError,
    super.followRedirects,
    super.maxRedirects,
    super.persistentConnection,
    super.requestEncoder,
    super.responseDecoder,
    super.listFormat,
  }) : super(responseType: ResponseType.json) {
    _attachExtras(mapper, maxResponseBytes);
  }

  /// Named constructor for raw binary payloads (e.g. Protobuf, MessagePack, FlatBuffers).
  ///
  /// Automatically binds [responseType] to [ResponseType.bytes] and statically
  /// types the [mapper] argument as [Uint8List].
  TurboOptions.bytes({
    required T Function(Uint8List bytes) mapper,
    int? maxResponseBytes,
    super.method,
    super.sendTimeout,
    super.receiveTimeout,
    super.extra,
    super.headers,
    super.preserveHeaderCase,
    super.contentType,
    super.validateStatus,
    super.receiveDataWhenStatusError,
    super.followRedirects,
    super.maxRedirects,
    super.persistentConnection,
    super.requestEncoder,
    super.responseDecoder,
    super.listFormat,
  }) : super(responseType: ResponseType.bytes) {
    _attachExtras(_wrapBytesMapper(mapper), maxResponseBytes);
  }

  /// Named constructor for plain text payloads (e.g. CSV, YAML, custom text formats).
  ///
  /// Automatically binds [responseType] to [ResponseType.plain] and statically
  /// types the [mapper] argument as [String].
  TurboOptions.plain({
    required T Function(String text) mapper,
    int? maxResponseBytes,
    super.method,
    super.sendTimeout,
    super.receiveTimeout,
    super.extra,
    super.headers,
    super.preserveHeaderCase,
    super.contentType,
    super.validateStatus,
    super.receiveDataWhenStatusError,
    super.followRedirects,
    super.maxRedirects,
    super.persistentConnection,
    super.requestEncoder,
    super.responseDecoder,
    super.listFormat,
  }) : super(responseType: ResponseType.plain) {
    _attachExtras(_wrapPlainMapper(mapper), maxResponseBytes);
  }

  void _attachExtras(TurboResponseMapper<T>? mapper, int? maxResponseBytes) {
    if (mapper != null || maxResponseBytes != null) {
      extra = <String, Object?>{
        ...?extra,
        if (mapper != null) turboMapperExtraKey: mapper,
        if (maxResponseBytes != null)
          turboMaxResponseBytesExtraKey: maxResponseBytes,
      };
    }
  }

  // Kept static so the wrapping closure captures only [mapper], never `this`.
  static TurboResponseMapper<R> _wrapBytesMapper<R>(
    R Function(Uint8List bytes) mapper,
  ) => (dynamic data) => mapper(data as Uint8List);

  static TurboResponseMapper<R> _wrapPlainMapper<R>(
    R Function(String text) mapper,
  ) => (dynamic data) => mapper(data as String);
}
