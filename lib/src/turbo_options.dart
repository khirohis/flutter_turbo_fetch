import 'package:dio/dio.dart';

typedef TurboResponseMapper<T> = T Function(dynamic data);

/// TurboTransformer 内部で extra からマッパーを取り出すためのキー
const String turboMapperExtraKey = '_turbo_response_mapper';

/// TurboHttpAdapter 内部で extra から最大レスポンスバイト数を取り出すためのキー
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
}
