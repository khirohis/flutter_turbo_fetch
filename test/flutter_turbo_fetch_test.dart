import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:flutter_turbo_fetch/src/messages.g.dart';

class FakeTurboFetchHostApi extends TurboFetchHostApi {
  FakeTurboFetchHostApi({required this.onExecute, this.onCancel});

  final Future<TurboHttpResponse> Function(TurboHttpRequest request) onExecute;
  final void Function(String requestId)? onCancel;

  TurboHttpRequest? lastRequest;
  final List<String> cancelledRequestIds = [];

  @override
  Future<TurboHttpResponse> executeRequest(TurboHttpRequest request) {
    lastRequest = request;
    return onExecute(request);
  }

  @override
  Future<void> cancelRequest(String requestId) async {
    cancelledRequestIds.add(requestId);
    onCancel?.call(requestId);
  }
}

Dio _createDio(TurboFetchHostApi hostApi) {
  final dio = Dio();
  dio.httpClientAdapter = TurboHttpAdapter(hostApi: hostApi);
  dio.transformer = TurboTransformer();
  return dio;
}

void main() {
  group('TurboTransformer', () {
    test('decodes JSON payload and applies TurboOptions mapper', () async {
      final transformer = TurboTransformer();
      final jsonBytes = Uint8List.fromList(utf8.encode('{"status":"ok"}'));
      final options = TurboOptions<String>(
        mapper: (data) => (data as Map<String, dynamic>)['status'] as String,
      ).compose(BaseOptions(), '/test');

      final result = await transformer.transformResponse(
        options,
        ResponseBody.fromBytes(jsonBytes, 200),
      );
      expect(result, 'ok');
    });
  });

  group('Error handling and DioException mapping (Step 2-5)', () {
    test('CancelToken triggers cancelRequest and throws DioExceptionType.cancel', () async {
      final completer = Completer<TurboHttpResponse>();
      late FakeTurboFetchHostApi fakeApi;
      fakeApi = FakeTurboFetchHostApi(
        onExecute: (_) => completer.future,
        onCancel: (_) {
          if (!completer.isCompleted) {
            completer.complete(
              TurboHttpResponse(
                errorCode: 'cancel',
                errorMessage: 'Request cancelled by user',
              ),
            );
          }
        },
      );

      final dio = _createDio(fakeApi);
      final cancelToken = CancelToken();

      final future = dio.get<dynamic>(
        'https://example.com/slow',
        cancelToken: cancelToken,
      );
      Future<void>.delayed(const Duration(milliseconds: 10), () {
        cancelToken.cancel('User cancelled');
      });

      await expectLater(
        future,
        throwsA(
          isA<DioException>().having(
            (e) => e.type,
            'type',
            DioExceptionType.cancel,
          ),
        ),
      );
      expect(fakeApi.cancelledRequestIds, hasLength(1));
      expect(
        fakeApi.cancelledRequestIds.first,
        equals(fakeApi.lastRequest?.requestId),
      );
    });

    test('maps connectionTimeout, sendTimeout, and receiveTimeout accurately', () async {
      final cases = <String, DioExceptionType>{
        'connectionTimeout': DioExceptionType.connectionTimeout,
        'sendTimeout': DioExceptionType.sendTimeout,
        'receiveTimeout': DioExceptionType.receiveTimeout,
      };

      for (final entry in cases.entries) {
        final fakeApi = FakeTurboFetchHostApi(
          onExecute: (_) async => TurboHttpResponse(
            errorCode: entry.key,
            errorMessage: 'Simulated ${entry.key}',
          ),
        );
        final dio = _createDio(fakeApi);

        await expectLater(
          dio.get<dynamic>('https://example.com/timeout'),
          throwsA(
            isA<DioException>()
                .having((e) => e.type, 'type', entry.value)
                .having((e) => e.message, 'message', 'Simulated ${entry.key}'),
          ),
        );
      }
    });

    test('maps badCertificate and connectionError accurately', () async {
      final cases = <String, DioExceptionType>{
        'badCertificate': DioExceptionType.badCertificate,
        'connectionError': DioExceptionType.connectionError,
      };

      for (final entry in cases.entries) {
        final fakeApi = FakeTurboFetchHostApi(
          onExecute: (_) async => TurboHttpResponse(
            errorCode: entry.key,
            errorMessage: 'Simulated ${entry.key}',
          ),
        );
        final dio = _createDio(fakeApi);

        await expectLater(
          dio.get<dynamic>('https://example.com/error'),
          throwsA(
            isA<DioException>().having((e) => e.type, 'type', entry.value),
          ),
        );
      }
    });

    test('forwards maxResponseBytes and maps maxResponseBytesExceeded to DioExceptionType.unknown with limit in message', () async {
      final fakeApi = FakeTurboFetchHostApi(
        onExecute: (req) async => TurboHttpResponse(
          errorCode: 'maxResponseBytesExceeded',
          errorMessage:
              'Response body exceeded maxResponseBytes limit (${req.maxResponseBytes} bytes)',
        ),
      );
      final dio = _createDio(fakeApi);

      await expectLater(
        dio.get<dynamic>(
          'https://example.com/large',
          options: TurboOptions(maxResponseBytes: 4096),
        ),
        throwsA(
          isA<DioException>()
              .having((e) => e.type, 'type', DioExceptionType.unknown)
              .having((e) => e.message, 'message', contains('4096 bytes')),
        ),
      );
      expect(fakeApi.lastRequest?.maxResponseBytes, 4096);
    });

    test('handles non-200 status codes (404 / 500) via Dio validateStatus as badResponse', () async {
      final errorJson = Uint8List.fromList(
        utf8.encode('{"error":"Not Found"}'),
      );
      final fakeApi = FakeTurboFetchHostApi(
        onExecute: (_) async => TurboHttpResponse(
          statusCode: 404,
          statusMessage: 'Not Found',
          headers: <String?, Object?>{
            'content-type': <String>['application/json'],
          },
          body: errorJson,
        ),
      );
      final dio = _createDio(fakeApi);

      await expectLater(
        dio.get<dynamic>('https://example.com/missing'),
        throwsA(
          isA<DioException>()
              .having((e) => e.type, 'type', DioExceptionType.badResponse)
              .having((e) => e.response?.statusCode, 'statusCode', 404)
              .having(
                (e) => e.response?.data,
                'decoded error body',
                {'error': 'Not Found'},
              ),
        ),
      );

      // When custom validateStatus allows 404, response is returned without throwing
      final allowedResponse = await dio.get<Map<String, dynamic>>(
        'https://example.com/missing',
        options: TurboOptions(
          validateStatus: (status) => status != null && status < 500,
        ),
      );
      expect(allowedResponse.statusCode, 404);
      expect(allowedResponse.data, {'error': 'Not Found'});
    });
  });
}
