import 'package:pigeon/pigeon.dart';

@ConfigurePigeon(
  PigeonOptions(
    dartOut: 'lib/src/messages.g.dart',
    kotlinOut:
        'android/src/main/kotlin/io/github/khirohis/flutter_turbo_fetch/Messages.g.kt',
    kotlinOptions: KotlinOptions(
      package: 'io.github.khirohis.flutter_turbo_fetch',
    ),
    swiftOut:
        'ios/flutter_turbo_fetch/Sources/flutter_turbo_fetch/Messages.g.swift',
  ),
)
class TurboHttpRequest {
  TurboHttpRequest({
    required this.requestId,
    required this.url,
    required this.method,
    required this.headers,
    this.body,
    this.connectTimeoutMs,
    this.receiveTimeoutMs,
    this.sendTimeoutMs,
    required this.followRedirects,
    required this.maxRedirects,
  });

  final String requestId;
  final String url;
  final String method;
  final Map<String?, String?> headers;
  final Uint8List? body;
  final int? connectTimeoutMs;
  final int? receiveTimeoutMs;
  final int? sendTimeoutMs;
  final bool followRedirects;
  final int maxRedirects;
}

class TurboHttpResponse {
  TurboHttpResponse({
    this.statusCode,
    this.statusMessage,
    this.headers,
    this.body,
    this.errorType,
    this.errorMessage,
  });

  final int? statusCode;
  final String? statusMessage;
  final Map<String?, List<String?>?>? headers;
  final Uint8List? body;
  final String? errorType;
  final String? errorMessage;
}

@HostApi()
abstract class TurboFetchHostApi {
  @async
  TurboHttpResponse executeRequest(TurboHttpRequest request);

  void cancelRequest(String requestId);
}
