import 'dart:convert';
import 'dart:typed_data';

import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:flutter_turbo_fetch/src/messages.g.dart';

import 'test_helpers.dart';

void main() {
  group('TurboOptions', () {
    group('Constructors & extra keys propagation', () {
      test('generic constructor preserves user extra and applies default ResponseType.json', () {
        // When mapper and maxResponseBytes are omitted, extra is not polluted with reserved keys
        final defaultOptions = TurboOptions<dynamic>(
          extra: {'custom_user_key': 'user_value'},
        );
        expect(defaultOptions.responseType, ResponseType.json);
        expect(defaultOptions.extra?['custom_user_key'], 'user_value');
        expect(defaultOptions.extra?.containsKey(turboMapperExtraKey), isFalse);
        expect(defaultOptions.extra?.containsKey(turboMaxResponseBytesExtraKey), isFalse);

        // When mapper and maxResponseBytes are provided, they merge seamlessly into user extra
        final mapperOptions = TurboOptions<String>(
          mapper: (dynamic data) => 'mapped',
          maxResponseBytes: 512,
          extra: {'custom_user_key': 42},
        );
        expect(mapperOptions.extra?['custom_user_key'], 42);
        expect(mapperOptions.extra?[turboMaxResponseBytesExtraKey], 512);
        final mapper = mapperOptions.extra?[turboMapperExtraKey] as dynamic Function(dynamic)?;
        expect(mapper, isNotNull);
        expect(mapper!(null), 'mapped');
      });

      test('TurboOptions.json binds ResponseType.json and configures mapper correctly', () {
        final jsonOptions = TurboOptions<String>.json(
          mapper: (json) => ((json as Map<String, dynamic>)['name'] as String).toUpperCase(),
          maxResponseBytes: 1024,
          headers: {'X-Test': 'json'},
          extra: {'traceId': 'trace_123'},
        );

        expect(jsonOptions.responseType, ResponseType.json);
        expect(jsonOptions.headers?['X-Test'], 'json');
        expect(jsonOptions.extra?['traceId'], 'trace_123');
        expect(jsonOptions.extra?[turboMaxResponseBytesExtraKey], 1024);

        final mapper = jsonOptions.extra?[turboMapperExtraKey] as dynamic Function(dynamic)?;
        expect(mapper, isNotNull);
        expect(mapper!({'name': 'alice'}), 'ALICE');
      });

      test('TurboOptions.bytes binds ResponseType.bytes and configures mapper correctly', () {
        final bytesOptions = TurboOptions<int>.bytes(
          mapper: (Uint8List bytes) => bytes.lengthInBytes,
          maxResponseBytes: 2048,
          headers: {'X-Test': 'bytes'},
        );

        expect(bytesOptions.responseType, ResponseType.bytes);
        expect(bytesOptions.headers?['X-Test'], 'bytes');
        expect(bytesOptions.extra?[turboMaxResponseBytesExtraKey], 2048);

        final mapper = bytesOptions.extra?[turboMapperExtraKey] as dynamic Function(dynamic)?;
        expect(mapper, isNotNull);
        expect(mapper!(Uint8List.fromList([1, 2, 3, 4])), 4);
      });

      test('TurboOptions.plain binds ResponseType.plain and configures mapper correctly', () {
        final plainOptions = TurboOptions<String>.plain(
          mapper: (String text) => text.trim(),
          maxResponseBytes: 4096,
          headers: {'X-Test': 'plain'},
        );

        expect(plainOptions.responseType, ResponseType.plain);
        expect(plainOptions.headers?['X-Test'], 'plain');
        expect(plainOptions.extra?[turboMaxResponseBytesExtraKey], 4096);

        final mapper = plainOptions.extra?[turboMapperExtraKey] as dynamic Function(dynamic)?;
        expect(mapper, isNotNull);
        expect(mapper!('  hello turbo  '), 'hello turbo');
      });
    });

    group('Request isolation', () {
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
        final dio = createTestDio(fakeApi);

        final results = await Future.wait<Object?>(<Future<Object?>>[
          dio
              .get<TestUser>(
                'https://example.com/api/user',
                options: TurboOptions<TestUser>(
                  responseType: ResponseType.json,
                  mapper: (d) => TestUser((d as Map<String, dynamic>)['name'] as String),
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

        expect(results[0], const TestUser('Alice'));
        expect(results[1], 'Plain text content');
        expect(results[2], Uint8List.fromList(<int>[0x89, 0x50, 0x4E, 0x47]));

        expect(fakeApi.requests[0].maxResponseBytes, 1024);
        expect(fakeApi.requests[1].maxResponseBytes, 2048);
        expect(fakeApi.requests[2].maxResponseBytes, 8192);
      });
    });
  });
}
