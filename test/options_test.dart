import 'dart:convert';
import 'dart:typed_data';

import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:flutter_turbo_fetch/src/messages.g.dart';

import 'test_helpers.dart';

void main() {
  group('TurboOptions', () {
    test('typed factory constructors configure responseType and execute mapper correctly', () async {
      // 1. TurboOptions.json
      final jsonOptions = TurboOptions<String>.json(
        mapper: (json) => ((json as Map<String, dynamic>)['name'] as String).toUpperCase(),
        maxResponseBytes: 1024,
        headers: {'X-Test': 'json'},
      );
      expect(jsonOptions.responseType, ResponseType.json);
      expect(jsonOptions.headers?['X-Test'], 'json');
      expect(jsonOptions.extra?[turboMaxResponseBytesExtraKey], 1024);
      final jsonMapper = jsonOptions.extra?[turboMapperExtraKey] as dynamic Function(dynamic)?;
      expect(jsonMapper, isNotNull);
      expect(jsonMapper!({'name': 'alice'}), 'ALICE');

      // 2. TurboOptions.bytes
      final bytesOptions = TurboOptions<int>.bytes(
        mapper: (Uint8List bytes) => bytes.lengthInBytes,
        maxResponseBytes: 2048,
        headers: {'X-Test': 'bytes'},
      );
      expect(bytesOptions.responseType, ResponseType.bytes);
      expect(bytesOptions.headers?['X-Test'], 'bytes');
      expect(bytesOptions.extra?[turboMaxResponseBytesExtraKey], 2048);
      final bytesMapper = bytesOptions.extra?[turboMapperExtraKey] as dynamic Function(dynamic)?;
      expect(bytesMapper, isNotNull);
      expect(bytesMapper!(Uint8List.fromList([1, 2, 3])), 3);

      // 3. TurboOptions.plain
      final plainOptions = TurboOptions<String>.plain(
        mapper: (String text) => text.trim(),
        maxResponseBytes: 4096,
        headers: {'X-Test': 'plain'},
      );
      expect(plainOptions.responseType, ResponseType.plain);
      expect(plainOptions.headers?['X-Test'], 'plain');
      expect(plainOptions.extra?[turboMaxResponseBytesExtraKey], 4096);
      final plainMapper = plainOptions.extra?[turboMapperExtraKey] as dynamic Function(dynamic)?;
      expect(plainMapper, isNotNull);
      expect(plainMapper!('  hello  '), 'hello');

      // 4. End-to-end transformResponse with TurboTransformer
      final transformer = TurboTransformer();
      final jsonResult = await transformer.transformResponse(
        jsonOptions.compose(BaseOptions(), '/json'),
        ResponseBody.fromBytes(Uint8List.fromList(utf8.encode('{"name":"bob"}')), 200),
      );
      expect(jsonResult, 'BOB');

      final bytesResult = await transformer.transformResponse(
        bytesOptions.compose(BaseOptions(), '/bytes'),
        ResponseBody.fromBytes(Uint8List.fromList([10, 20, 30, 40]), 200),
      );
      expect(bytesResult, 4);

      final plainResult = await transformer.transformResponse(
        plainOptions.compose(BaseOptions(), '/plain'),
        ResponseBody.fromBytes(Uint8List.fromList(utf8.encode('  world  ')), 200),
      );
      expect(plainResult, 'world');
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
}
