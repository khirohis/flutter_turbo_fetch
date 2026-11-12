import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:flutter_turbo_fetch/src/messages.g.dart';

import 'test_helpers.dart';

void main() {
  group('TurboHttpAdapter', () {
    group('Request & response conversion', () {
      test('aggregates multi-chunk request stream and normalizes headers', () async {
        final fakeApi = FakeTurboFetchHostApi(
          onExecute: (req) async => TurboHttpResponse(
            statusCode: 201,
            statusMessage: 'Created',
            headers: <String?, Object?>{
              'X-Custom-Header': <String>['val1', 'val2'],
              'Content-Type': 'application/json',
            },
            body: Uint8List.fromList(utf8.encode('{"created":true}')),
          ),
        );
        final adapter = TurboHttpAdapter(
          maxRequestsPerHost: 48,
          allowBadCertificates: true,
          hostApi: fakeApi,
        );

        final chunks = Stream<Uint8List>.fromIterable(<Uint8List>[
          Uint8List.fromList(<int>[1, 2, 3]),
          Uint8List.fromList(<int>[4, 5]),
        ]);

        final options = RequestOptions(
          path: 'https://example.com/items',
          method: 'post',
          headers: <String, Object?>{
            'Accept': <String>['application/json', 'text/plain'],
            'Authorization': 'Bearer token',
          },
          connectTimeout: const Duration(seconds: 5),
          receiveTimeout: const Duration(seconds: 10),
          sendTimeout: const Duration(seconds: 7),
          followRedirects: false,
        );

        final responseBody = await adapter.fetch(options, chunks, null);

        expect(fakeApi.lastRequest, isNotNull);
        final sent = fakeApi.lastRequest!;
        expect(sent.url, 'https://example.com/items');
        expect(sent.method, 'POST');
        expect(sent.body, Uint8List.fromList(<int>[1, 2, 3, 4, 5]));
        expect(sent.headers['Accept'], 'application/json, text/plain');
        expect(sent.headers['Authorization'], 'Bearer token');
        expect(sent.connectTimeoutMs, 5000);
        expect(sent.receiveTimeoutMs, 10000);
        expect(sent.sendTimeoutMs, 7000);
        expect(sent.followRedirects, isFalse);
        expect(sent.maxRequestsPerHost, 48);
        expect(sent.allowBadCertificates, isTrue);

        expect(responseBody.statusCode, 201);
        expect(responseBody.statusMessage, 'Created');
        expect(responseBody.headers['x-custom-header'], <String>['val1', 'val2']);
        expect(responseBody.headers['content-type'], <String>['application/json']);
      });

      test('handles ResponseType.stream via native batch fetch without fallback', () async {
        final expectedBytes = Uint8List.fromList(<int>[10, 20, 30, 40]);
        final fakeApi = FakeTurboFetchHostApi(
          onExecute: (_) async => TurboHttpResponse(
            statusCode: 200,
            body: expectedBytes,
          ),
        );
        final dio = createTestDio(fakeApi);

        final response = await dio.get<ResponseBody>(
          'https://example.com/stream',
          options: Options(responseType: ResponseType.stream),
        );

        expect(fakeApi.requests, hasLength(1));
        final receivedChunks = await response.data!.stream.toList();
        expect(receivedChunks, hasLength(1));
        expect(receivedChunks.first, expectedBytes);
      });

      test('normalizes single non-iterable response headers to list format', () async {
        final headerFakeApi = FakeTurboFetchHostApi(
          onExecute: (_) async => TurboHttpResponse(
            statusCode: 200,
            headers: <String?, Object?>{
              'Server': 'TurboServer/1.0', // Non-iterable single string
            },
            body: Uint8List(0),
          ),
        );
        final adapter = TurboHttpAdapter(hostApi: headerFakeApi);
        final responseBody = await adapter.fetch(
          RequestOptions(path: 'https://example.com'),
          null,
          null,
        );
        expect(responseBody.headers['server'], <String>['TurboServer/1.0']);
      });

      test('checks platform support (Android, iOS, macOS, Windows, Linux allowed; others throw UnsupportedError) when hostApi is not overridden', () async {
        final adapter = TurboHttpAdapter();
        if (Platform.isAndroid ||
            Platform.isIOS ||
            Platform.isMacOS ||
            Platform.isWindows ||
            Platform.isLinux) {
          await expectLater(
            adapter.fetch(RequestOptions(path: 'https://example.com'), null, null),
            throwsA(isA<DioException>()),
          );
        } else {
          await expectLater(
            adapter.fetch(RequestOptions(path: 'https://example.com'), null, null),
            throwsA(isA<UnsupportedError>()),
          );
        }
      });

      test('generates unique requestId using Isolate hashCode and static counter', () async {
        final fakeApi = FakeTurboFetchHostApi();
        final adapter1 = TurboHttpAdapter(hostApi: fakeApi);
        final adapter2 = TurboHttpAdapter(hostApi: fakeApi);

        await adapter1.fetch(RequestOptions(path: 'https://example.com/1'), null, null);
        await adapter2.fetch(RequestOptions(path: 'https://example.com/2'), null, null);
        await adapter1.fetch(RequestOptions(path: 'https://example.com/3'), null, null);

        expect(fakeApi.requests, hasLength(3));

        final reqId1 = fakeApi.requests[0].requestId;
        final reqId2 = fakeApi.requests[1].requestId;
        final reqId3 = fakeApi.requests[2].requestId;

        final expectedPrefix = 'turbo_${Isolate.current.hashCode}_';
        expect(reqId1.startsWith(expectedPrefix), isTrue);
        expect(reqId2.startsWith(expectedPrefix), isTrue);
        expect(reqId3.startsWith(expectedPrefix), isTrue);

        final counter1 = int.parse(reqId1.substring(expectedPrefix.length));
        final counter2 = int.parse(reqId2.substring(expectedPrefix.length));
        final counter3 = int.parse(reqId3.substring(expectedPrefix.length));

        expect(counter2, equals(counter1 + 1));
        expect(counter3, equals(counter2 + 1));
      });
    });

    group('Lifecycle and closing', () {
      test('throws StateError when fetch is called after close()', () async {
        final adapter = TurboHttpAdapter(hostApi: FakeTurboFetchHostApi());
        adapter.close();

        await expectLater(
          adapter.fetch(RequestOptions(path: 'https://example.com'), null, null),
          throwsA(isA<StateError>()),
        );
      });

      test('close(force: false) allows in-flight requests to complete gracefully', () async {
        final completer = Completer<TurboHttpResponse>();
        final fakeApi = FakeTurboFetchHostApi(
          onExecute: (_) => completer.future,
        );
        final adapter = TurboHttpAdapter(hostApi: fakeApi);

        final future = adapter.fetch(
          RequestOptions(path: 'https://example.com/inflight'),
          null,
          null,
        );

        adapter.close(force: false);

        expect(fakeApi.cancelledRequestIds, isEmpty);

        completer.complete(
          TurboHttpResponse(
            statusCode: 200,
            body: Uint8List(0),
          ),
        );
        final response = await future;
        expect(response.statusCode, 200);
      });

      test('close(force: true) cancels in-flight requests via cancelRequest', () async {
        final completers = <String, Completer<TurboHttpResponse>>{};
        late FakeTurboFetchHostApi fakeApi;
        fakeApi = FakeTurboFetchHostApi(
          onExecute: (req) {
            final c = Completer<TurboHttpResponse>();
            completers[req.requestId] = c;
            return c.future;
          },
          onCancel: (id) {
            final c = completers[id];
            if (c != null && !c.isCompleted) {
              c.complete(
                TurboHttpResponse(
                  errorCode: 'cancel',
                  errorMessage: 'Request cancelled by force close',
                ),
              );
            }
          },
        );
        final adapter = TurboHttpAdapter(hostApi: fakeApi);

        final future1 = adapter.fetch(
          RequestOptions(path: 'https://example.com/1'),
          null,
          null,
        );
        final future2 = adapter.fetch(
          RequestOptions(path: 'https://example.com/2'),
          null,
          null,
        );

        expect(fakeApi.requests, hasLength(2));
        final id1 = fakeApi.requests[0].requestId;
        final id2 = fakeApi.requests[1].requestId;

        adapter.close(force: true);

        expect(fakeApi.cancelledRequestIds, containsAll(<String>[id1, id2]));

        await expectLater(
          future1,
          throwsA(
            isA<DioException>().having(
              (e) => e.type,
              'type',
              DioExceptionType.cancel,
            ),
          ),
        );
        await expectLater(
          future2,
          throwsA(
            isA<DioException>().having(
              (e) => e.type,
              'type',
              DioExceptionType.cancel,
            ),
          ),
        );
      });
    });

    group('Cancellation', () {
      test('CancelToken triggers cancelRequest and throws DioExceptionType.cancel', () async {
        final completer = Completer<TurboHttpResponse>();
        late FakeTurboFetchHostApi fakeApi;
        final cancelToken = CancelToken();

        fakeApi = FakeTurboFetchHostApi(
          onExecute: (_) {
            // Trigger cancellation synchronously upon request arrival without timers
            scheduleMicrotask(() => cancelToken.cancel('User cancelled'));
            return completer.future;
          },
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

        final dio = createTestDio(fakeApi);

        await expectLater(
          dio.get<dynamic>(
            'https://example.com/slow',
            cancelToken: cancelToken,
          ),
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

      test('throws DioExceptionType.cancel when response succeeded but request was cancelled (race condition)', () async {
        final cancelToken = CancelToken();
        final fakeApi = FakeTurboFetchHostApi(
          onExecute: (_) async {
            // Simulate cancellation arriving just before returning response
            cancelToken.cancel('Cancelled during response arrival');
            return TurboHttpResponse(
              statusCode: 200,
              body: Uint8List.fromList(utf8.encode('{"ok":true}')),
            );
          },
        );
        final dio = createTestDio(fakeApi);

        await expectLater(
          dio.get<dynamic>(
            'https://example.com/race-cancel',
            cancelToken: cancelToken,
          ),
          throwsA(
            isA<DioException>().having(
              (e) => e.type,
              'type',
              DioExceptionType.cancel,
            ),
          ),
        );
      });
    });

    group('Error handling & DioException mapping', () {
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
          final dio = createTestDio(fakeApi);

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

      test('maps badCertificate, connectionError, and unknown accurately', () async {
        final cases = <String, DioExceptionType>{
          'badCertificate': DioExceptionType.badCertificate,
          'connectionError': DioExceptionType.connectionError,
          'unknown': DioExceptionType.unknown,
        };

        for (final entry in cases.entries) {
          final fakeApi = FakeTurboFetchHostApi(
            onExecute: (_) async => TurboHttpResponse(
              errorCode: entry.key,
              errorMessage: 'Simulated ${entry.key}',
            ),
          );
          final dio = createTestDio(fakeApi);

          await expectLater(
            dio.get<dynamic>('https://example.com/error'),
            throwsA(
              isA<DioException>()
                  .having((e) => e.type, 'type', entry.value)
                  .having((e) => e.message, 'message', 'Simulated ${entry.key}'),
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
        final dio = createTestDio(fakeApi);

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

      test('uses fallback message when native errorCode has null errorMessage', () async {
        final fakeApi = FakeTurboFetchHostApi(
          onExecute: (_) async => TurboHttpResponse(
            errorCode: 'maxResponseBytesExceeded',
            errorMessage: null, // null message triggers fallback resolver
          ),
        );
        final dio = createTestDio(fakeApi);

        await expectLater(
          dio.get<dynamic>(
            'https://example.com/exceeded-null-msg',
            options: TurboOptions(maxResponseBytes: 8192),
          ),
          throwsA(
            isA<DioException>()
                .having((e) => e.type, 'type', DioExceptionType.unknown)
                .having(
                  (e) => e.message,
                  'fallback message',
                  'Response body exceeded maxResponseBytes limit (8192 bytes)',
                ),
          ),
        );
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
        final dio = createTestDio(fakeApi);

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

        final allowedResponse = await dio.get<Map<String, dynamic>>(
          'https://example.com/missing',
          options: TurboOptions(
            validateStatus: (status) => status != null && status < 500,
          ),
        );
        expect(allowedResponse.statusCode, 404);
        expect(allowedResponse.data, {'error': 'Not Found'});
      });

      test('wraps unexpected hostApi exceptions into DioException (unknown and cancel)', () async {
        final throwingApi = FakeTurboFetchHostApi(
          onExecute: (_) async => throw Exception('Pigeon bridge failure'),
        );
        final dio = createTestDio(throwingApi);

        await expectLater(
          dio.get<dynamic>('https://example.com/bridge-error'),
          throwsA(
            isA<DioException>()
                .having((e) => e.type, 'type', DioExceptionType.unknown)
                .having((e) => e.message, 'message', contains('Pigeon bridge failure')),
          ),
        );

        final cancelToken = CancelToken();
        final cancelCompleter = Completer<TurboHttpResponse>();
        final cancelThrowingApi = FakeTurboFetchHostApi(
          onExecute: (_) async {
            scheduleMicrotask(() => cancelToken.cancel('User aborted'));
            await cancelCompleter.future;
            throw Exception('Pigeon canceled error');
          },
          onCancel: (_) {
            if (!cancelCompleter.isCompleted) {
              cancelCompleter.completeError(Exception('Pigeon canceled error'));
            }
          },
        );
        final cancelDio = createTestDio(cancelThrowingApi);

        await expectLater(
          cancelDio.get<dynamic>(
            'https://example.com/cancel-bridge-error',
            cancelToken: cancelToken,
          ),
          throwsA(
            isA<DioException>().having(
              (e) => e.type,
              'type',
              DioExceptionType.cancel,
            ),
          ),
        );
      });
    });
  });
}
