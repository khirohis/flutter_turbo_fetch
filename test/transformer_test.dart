import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';

import 'test_helpers.dart';

void main() {
  group('TurboTransformer', () {
    group('Default configurations', () {
      test('defines expected default constants and respects them when omitted', () {
        expect(TurboTransformer.defaultIsolateThresholdBytes, 24 * 1024);
        expect(TurboTransformer.defaultMinConcurrentIsolates, 2);

        final defaultTransformer = TurboTransformer();
        expect(
          defaultTransformer.isolateThresholdBytes,
          TurboTransformer.defaultIsolateThresholdBytes,
        );
        final expectedConcurrent = Platform.numberOfProcessors - 2 > TurboTransformer.defaultMinConcurrentIsolates
            ? Platform.numberOfProcessors - 2
            : TurboTransformer.defaultMinConcurrentIsolates;
        expect(defaultTransformer.maxConcurrentIsolates, expectedConcurrent);
      });
    });

    group('ResponseType decoding (below threshold & streaming)', () {
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
            mapper: bytesLengthAndFirstByteMapper,
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

      test('consolidates multi-chunk streams for bytes, plain, and json', () async {
        final transformer = TurboTransformer();

        // 1. Multi-chunk bytes
        final chunk1 = Uint8List.fromList(<int>[1, 2]);
        final chunk2 = Uint8List.fromList(<int>[3, 4]);
        final multiChunkBytes = ResponseBody(
          Stream.fromIterable([chunk1, chunk2]),
          200,
        );
        final bytesResult = await transformer.transformResponse(
          RequestOptions(path: '/b', responseType: ResponseType.bytes),
          multiChunkBytes,
        );
        expect(bytesResult, Uint8List.fromList(<int>[1, 2, 3, 4]));

        // 2. Multi-chunk plain
        final chunkText1 = Uint8List.fromList(utf8.encode('hello '));
        final chunkText2 = Uint8List.fromList(utf8.encode('world'));
        final multiChunkPlain = ResponseBody(
          Stream.fromIterable([chunkText1, chunkText2]),
          200,
        );
        final plainResult = await transformer.transformResponse(
          RequestOptions(path: '/p', responseType: ResponseType.plain),
          multiChunkPlain,
        );
        expect(plainResult, 'hello world');

        // 3. Multi-chunk json
        final chunkJson1 = Uint8List.fromList(utf8.encode('{"part":'));
        final chunkJson2 = Uint8List.fromList(utf8.encode('123}'));
        final multiChunkJson = ResponseBody(
          Stream.fromIterable([chunkJson1, chunkJson2]),
          200,
        );
        final jsonResult = await transformer.transformResponse(
          RequestOptions(path: '/j', responseType: ResponseType.json),
          multiChunkJson,
        );
        expect(jsonResult, <String, dynamic>{'part': 123});
      });
    });

    group('Worker Isolate offloading & zero-copy', () {
      test('offloads bytes (with mapper), plain, and json >= isolateThresholdBytes to Worker Isolate', () async {
        final mainIsolateName = Isolate.current.debugName ?? 'main';
        // Set threshold to 1KB so a 4KB payload triggers Worker Isolate execution
        final transformer = TurboTransformer(isolateThresholdBytes: 1024);

        // 1. Large bytes with mapper in Worker Isolate
        final largeBytes = Uint8List(4096)..[0] = 42;
        final bytesMapped = await transformer.transformResponse(
          TurboOptions<int>(
            responseType: ResponseType.bytes,
            mapper: bytesLengthAndFirstByteMapper,
          ).compose(BaseOptions(), '/large-bytes'),
          ResponseBody.fromBytes(largeBytes, 200),
        );
        expect(bytesMapped, 4096 + 42);

        // 2. Large plain text in Worker Isolate
        final largeTextBytes = Uint8List.fromList(utf8.encode('A' * 2048));
        final plainMapped = await transformer.transformResponse(
          TurboOptions<String>(
            responseType: ResponseType.plain,
            mapper: plainSummaryMapper,
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
            mapper: jsonIsolateInspectorMapper,
          ).compose(BaseOptions(), '/large-json'),
          ResponseBody.fromBytes(largeJsonBytes, 200),
        ) as Map<String, Object?>;

        expect(jsonMapped['count'], 60);
        expect(jsonMapped['isolateDebugName'], isNot(equals(mainIsolateName)));
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

    group('Concurrency & resource protection', () {
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
              mapper: slowWorkerMapper,
            ).compose(BaseOptions(), '/seq/$index'),
            ResponseBody.fromBytes(payload, 200),
          );
        });

        final results = await Future.wait(futures);
        expect(results, <int>[0, 1, 2, 3, 4, 5]);
      });
    });
  });
}
