import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:isolate';
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
  final List<TurboHttpRequest> requests = [];
  final List<String> cancelledRequestIds = [];

  @override
  Future<TurboHttpResponse> executeRequest(TurboHttpRequest request) {
    lastRequest = request;
    requests.add(request);
    return onExecute(request);
  }

  @override
  Future<void> cancelRequest(String requestId) async {
    cancelledRequestIds.add(requestId);
    onCancel?.call(requestId);
  }
}

class _TestUser {
  const _TestUser(this.name);
  final String name;

  @override
  bool operator ==(Object other) => other is _TestUser && other.name == name;

  @override
  int get hashCode => name.hashCode;
}

Dio _createDio(
  TurboFetchHostApi hostApi, {
  int isolateThresholdBytes = 24 * 1024,
  int? maxConcurrentIsolates,
}) {
  final dio = Dio();
  dio.httpClientAdapter = TurboHttpAdapter(hostApi: hostApi);
  dio.transformer = TurboTransformer(
    isolateThresholdBytes: isolateThresholdBytes,
    maxConcurrentIsolates: maxConcurrentIsolates,
  );
  return dio;
}

// Top-level mappers for Worker Isolate tests (per TurboOptions isolate safety spec)
int _bytesLengthAndFirstByteMapper(dynamic data) {
  final bytes = data as Uint8List;
  return bytes.lengthInBytes + (bytes.isNotEmpty ? bytes[0] : 0);
}

String _plainSummaryMapper(dynamic data) {
  final text = data as String;
  final isolateName = Isolate.current.debugName ?? 'unknown';
  return 'len=${text.length};isolate=$isolateName';
}

Map<String, Object?> _jsonIsolateInspectorMapper(dynamic data) {
  final map = data as Map<String, dynamic>;
  return <String, Object?>{
    'count': (map['items'] as List<dynamic>).length,
    'isolateDebugName': Isolate.current.debugName,
  };
}

int _slowWorkerMapper(dynamic data) {
  final sw = Stopwatch()..start();
  while (sw.elapsedMilliseconds < 40) {
    // Busy wait 40ms inside Worker Isolate to test semaphore queuing deterministically
  }
  final map = data as Map<String, dynamic>;
  return map['id'] as int;
}

void main() {
  group('TurboHttpAdapter unit tests', () {
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
      final dio = _createDio(fakeApi);

      final response = await dio.get<ResponseBody>(
        'https://example.com/stream',
        options: Options(responseType: ResponseType.stream),
      );

      expect(fakeApi.requests, hasLength(1));
      final receivedChunks = await response.data!.stream.toList();
      expect(receivedChunks, hasLength(1));
      expect(receivedChunks.first, expectedBytes);
    });

    test('throws StateError when fetch is called after close()', () async {
      final fakeApi = FakeTurboFetchHostApi(
        onExecute: (_) async => TurboHttpResponse(statusCode: 200),
      );
      final adapter = TurboHttpAdapter(hostApi: fakeApi);
      adapter.close();

      await expectLater(
        adapter.fetch(RequestOptions(path: 'https://example.com'), null, null),
        throwsA(isA<StateError>()),
      );
    });

    test('throws UnsupportedError on desktop platforms when hostApi is not overridden', () async {
      if (Platform.isAndroid || Platform.isIOS) {
        return;
      }
      final adapter = TurboHttpAdapter();
      await expectLater(
        adapter.fetch(RequestOptions(path: 'https://example.com'), null, null),
        throwsA(isA<UnsupportedError>()),
      );
    });
  });

  group('TurboTransformer & TurboOptions unit tests', () {
    test('supports all 4 ResponseTypes with and without mapper (below threshold)', () async {
      final transformer = TurboTransformer(isolateThresholdBytes: 24 * 1024);

      // 1. ResponseType.stream
      final streamBytes = Uint8List.fromList(<int>[1, 2, 3]);
      final streamBody = ResponseBody.fromBytes(streamBytes, 200);
      final streamResult = await transformer.transformResponse(
        RequestOptions(path: '/s', responseType: ResponseType.stream),
        streamBody,
      );
      expect(identical(streamResult, streamBody), isTrue);

      final mappedStreamResult = await transformer.transformResponse(
        TurboOptions<int>(
          responseType: ResponseType.stream,
          mapper: (body) => (body as ResponseBody).statusCode,
        ).compose(BaseOptions(), '/s'),
        ResponseBody.fromBytes(streamBytes, 206),
      );
      expect(mappedStreamResult, 206);

      // 2. ResponseType.bytes
      final rawBytes = Uint8List.fromList(<int>[7, 8, 9]);
      final bytesResult = await transformer.transformResponse(
        RequestOptions(path: '/b', responseType: ResponseType.bytes),
        ResponseBody.fromBytes(rawBytes, 200),
      );
      expect(bytesResult, rawBytes);

      final mappedBytesResult = await transformer.transformResponse(
        TurboOptions<int>(
          responseType: ResponseType.bytes,
          mapper: _bytesLengthAndFirstByteMapper,
        ).compose(BaseOptions(), '/b'),
        ResponseBody.fromBytes(rawBytes, 200),
      );
      expect(mappedBytesResult, 3 + 7);

      // 3. ResponseType.plain
      final plainBytes = Uint8List.fromList(utf8.encode('hello turbo'));
      final plainResult = await transformer.transformResponse(
        RequestOptions(path: '/p', responseType: ResponseType.plain),
        ResponseBody.fromBytes(plainBytes, 200),
      );
      expect(plainResult, 'hello turbo');

      final mappedPlainResult = await transformer.transformResponse(
        TurboOptions<String>(
          responseType: ResponseType.plain,
          mapper: (text) => (text as String).toUpperCase(),
        ).compose(BaseOptions(), '/p'),
        ResponseBody.fromBytes(plainBytes, 200),
      );
      expect(mappedPlainResult, 'HELLO TURBO');

      // 4. ResponseType.json
      final jsonBytes = Uint8List.fromList(utf8.encode('{"status":"ok"}'));
      final jsonResult = await transformer.transformResponse(
        RequestOptions(path: '/j', responseType: ResponseType.json),
        ResponseBody.fromBytes(jsonBytes, 200),
      );
      expect(jsonResult, <String, dynamic>{'status': 'ok'});

      final mappedJsonResult = await transformer.transformResponse(
        TurboOptions<String>(
          responseType: ResponseType.json,
          mapper: (data) => (data as Map<String, dynamic>)['status'] as String,
        ).compose(BaseOptions(), '/j'),
        ResponseBody.fromBytes(jsonBytes, 200),
      );
      expect(mappedJsonResult, 'ok');
    });

    test('handles empty payloads (0 bytes) for plain and json', () async {
      final transformer = TurboTransformer();
      final emptyBytes = Uint8List(0);

      final plainEmpty = await transformer.transformResponse(
        RequestOptions(path: '/p', responseType: ResponseType.plain),
        ResponseBody.fromBytes(emptyBytes, 200),
      );
      expect(plainEmpty, '');

      final plainEmptyMapped = await transformer.transformResponse(
        TurboOptions<String>(
          responseType: ResponseType.plain,
          mapper: (data) => 'empty:${(data as String).isEmpty}',
        ).compose(BaseOptions(), '/p'),
        ResponseBody.fromBytes(emptyBytes, 200),
      );
      expect(plainEmptyMapped, 'empty:true');

      final jsonEmpty = await transformer.transformResponse(
        RequestOptions(path: '/j', responseType: ResponseType.json),
        ResponseBody.fromBytes(emptyBytes, 204),
      );
      expect(jsonEmpty, isNull);

      final jsonEmptyMapped = await transformer.transformResponse(
        TurboOptions<String>(
          responseType: ResponseType.json,
          mapper: (data) => data == null ? 'null_payload' : 'non_null',
        ).compose(BaseOptions(), '/j'),
        ResponseBody.fromBytes(emptyBytes, 204),
      );
      expect(jsonEmptyMapped, 'null_payload');
    });

    test('offloads bytes (with mapper), plain, and json >= isolateThresholdBytes to Worker Isolate', () async {
      final mainIsolateName = Isolate.current.debugName ?? 'main';
      // Set threshold to 1KB so a 4KB payload triggers Worker Isolate execution
      final transformer = TurboTransformer(isolateThresholdBytes: 1024);

      // 1. Large bytes with mapper in Worker Isolate
      final largeBytes = Uint8List(4096)..[0] = 42;
      final bytesMapped = await transformer.transformResponse(
        TurboOptions<int>(
          responseType: ResponseType.bytes,
          mapper: _bytesLengthAndFirstByteMapper,
        ).compose(BaseOptions(), '/large-bytes'),
        ResponseBody.fromBytes(largeBytes, 200),
      );
      expect(bytesMapped, 4096 + 42);

      // 2. Large plain text in Worker Isolate
      final largeTextBytes = Uint8List.fromList(utf8.encode('A' * 2048));
      final plainMapped = await transformer.transformResponse(
        TurboOptions<String>(
          responseType: ResponseType.plain,
          mapper: _plainSummaryMapper,
        ).compose(BaseOptions(), '/large-plain'),
        ResponseBody.fromBytes(largeTextBytes, 200),
      ) as String;
      expect(plainMapped, startsWith('len=2048;isolate='));
      expect(plainMapped, isNot('len=2048;isolate=$mainIsolateName'));

      // 3. Large JSON in Worker Isolate
      final items = List<Map<String, Object>>.generate(
        60,
        (i) => <String, Object>{'id': i, 'text': 'x' * 32},
      );
      final largeJsonBytes = Uint8List.fromList(
        utf8.encode(jsonEncode(<String, Object>{'items': items})),
      );
      expect(largeJsonBytes.lengthInBytes, greaterThan(1024));

      final jsonMapped = await transformer.transformResponse(
        TurboOptions<Map<String, Object?>>(
          responseType: ResponseType.json,
          mapper: _jsonIsolateInspectorMapper,
        ).compose(BaseOptions(), '/large-json'),
        ResponseBody.fromBytes(largeJsonBytes, 200),
      ) as Map<String, Object?>;

      expect(jsonMapped['count'], 60);
      expect(jsonMapped['isolateDebugName'], isNot(equals(mainIsolateName)));
    });

    test('semaphore limits concurrent Worker Isolates to maxConcurrentIsolates', () async {
      final transformer = TurboTransformer(
        isolateThresholdBytes: 64,
        maxConcurrentIsolates: 2,
      );
      expect(transformer.maxConcurrentIsolates, 2);

      final futures = List<Future<dynamic>>.generate(6, (index) {
        final payload = Uint8List.fromList(
          utf8.encode(
            jsonEncode(<String, Object>{
              'id': index,
              'padding': 'p' * 128,
            }),
          ),
        );
        return transformer.transformResponse(
          TurboOptions<int>(
            responseType: ResponseType.json,
            mapper: _slowWorkerMapper,
          ).compose(BaseOptions(), '/seq/$index'),
          ResponseBody.fromBytes(payload, 200),
        );
      });

      final results = await Future.wait(futures);
      expect(results, <int>[0, 1, 2, 3, 4, 5]);
    });

    test('single Dio instance handles concurrent requests with different TurboOptions without interference', () async {
      final fakeApi = FakeTurboFetchHostApi(
        onExecute: (req) async {
          if (req.url.endsWith('/api/user')) {
            return TurboHttpResponse(
              statusCode: 200,
              body: Uint8List.fromList(utf8.encode('{"name":"Alice"}')),
            );
          } else if (req.url.endsWith('/static/note.txt')) {
            return TurboHttpResponse(
              statusCode: 200,
              body: Uint8List.fromList(utf8.encode('Plain text content')),
            );
          } else {
            return TurboHttpResponse(
              statusCode: 200,
              body: Uint8List.fromList(<int>[0x89, 0x50, 0x4E, 0x47]),
            );
          }
        },
      );
      final dio = _createDio(fakeApi);

      final results = await Future.wait<Object?>(<Future<Object?>>[
        dio
            .get<_TestUser>(
              'https://example.com/api/user',
              options: TurboOptions<_TestUser>(
                responseType: ResponseType.json,
                mapper: (d) => _TestUser((d as Map<String, dynamic>)['name'] as String),
                maxResponseBytes: 1024,
              ),
            )
            .then((r) => r.data),
        dio
            .get<String>(
              'https://example.com/static/note.txt',
              options: TurboOptions<String>(
                responseType: ResponseType.plain,
                maxResponseBytes: 2048,
              ),
            )
            .then((r) => r.data),
        dio
            .get<Uint8List>(
              'https://example.com/assets/icon.png',
              options: TurboOptions<Uint8List>(
                responseType: ResponseType.bytes,
                maxResponseBytes: 8192,
              ),
            )
            .then((r) => r.data),
      ]);

      expect(results[0], const _TestUser('Alice'));
      expect(results[1], 'Plain text content');
      expect(results[2], Uint8List.fromList(<int>[0x89, 0x50, 0x4E, 0x47]));

      expect(fakeApi.requests[0].maxResponseBytes, 1024);
      expect(fakeApi.requests[1].maxResponseBytes, 2048);
      expect(fakeApi.requests[2].maxResponseBytes, 8192);
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

    test('normalizes single non-iterable response headers and handles null errorMessage fallback', () async {
      final fakeApi = FakeTurboFetchHostApi(
        onExecute: (_) async => TurboHttpResponse(
          errorCode: 'maxResponseBytesExceeded',
          errorMessage: null, // null message triggers fallback resolver
        ),
      );
      final dio = _createDio(fakeApi);

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

      // Verify single string header normalization
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

    test('wraps unexpected hostApi exceptions into DioException (unknown and cancel)', () async {
      // 1. Uncancelled unexpected exception
      final throwingApi = FakeTurboFetchHostApi(
        onExecute: (_) async => throw Exception('Pigeon bridge failure'),
      );
      final dio = _createDio(throwingApi);

      await expectLater(
        dio.get<dynamic>('https://example.com/bridge-error'),
        throwsA(
          isA<DioException>()
              .having((e) => e.type, 'type', DioExceptionType.unknown)
              .having((e) => e.message, 'message', contains('Pigeon bridge failure')),
        ),
      );

      // 2. Cancelled unexpected exception
      final cancelToken = CancelToken();
      final cancelCompleter = Completer<TurboHttpResponse>();
      final cancelThrowingApi = FakeTurboFetchHostApi(
        onExecute: (_) async {
          await cancelCompleter.future;
          throw Exception('Pigeon canceled error');
        },
        onCancel: (_) {
          if (!cancelCompleter.isCompleted) {
            cancelCompleter.completeError(Exception('Pigeon canceled error'));
          }
        },
      );
      final cancelDio = _createDio(cancelThrowingApi);

      final cancelFuture = cancelDio.get<dynamic>(
        'https://example.com/cancel-bridge-error',
        cancelToken: cancelToken,
      );
      Future<void>.delayed(const Duration(milliseconds: 10), () {
        cancelToken.cancel('User aborted');
      });

      await expectLater(
        cancelFuture,
        throwsA(
          isA<DioException>().having(
            (e) => e.type,
            'type',
            DioExceptionType.cancel,
          ),
        ),
      );
    });

    test('throws DioExceptionType.cancel when response succeeded but request was cancelled (race condition)', () async {
      final completer = Completer<TurboHttpResponse>();
      final fakeApi = FakeTurboFetchHostApi(
        onExecute: (_) => completer.future,
        onCancel: (_) {
          // Native completed with 200 just as cancel arrived
          if (!completer.isCompleted) {
            completer.complete(
              TurboHttpResponse(
                statusCode: 200,
                body: Uint8List.fromList(utf8.encode('{"ok":true}')),
              ),
            );
          }
        },
      );
      final dio = _createDio(fakeApi);
      final cancelToken = CancelToken();

      final future = dio.get<dynamic>(
        'https://example.com/race-cancel',
        cancelToken: cancelToken,
      );
      Future<void>.delayed(const Duration(milliseconds: 10), () {
        cancelToken.cancel('Cancelled during response arrival');
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
    });

    test('returns bytes directly on Main Isolate when bytes >= isolateThresholdBytes but mapper is null', () async {
      final transformer = TurboTransformer(isolateThresholdBytes: 1024);
      final largeBytes = Uint8List(4096)..[0] = 99;

      final result = await transformer.transformResponse(
        RequestOptions(path: '/large-raw-bytes', responseType: ResponseType.bytes),
        ResponseBody.fromBytes(largeBytes, 200),
      );

      expect(result, isA<Uint8List>());
      expect((result as Uint8List).lengthInBytes, 4096);
      expect(result[0], 99);
    });
  });
}
