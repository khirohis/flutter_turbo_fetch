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
}
