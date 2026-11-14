import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/src/messages.g.dart';
import 'package:flutter_turbo_fetch_example/benchmark/benchmark_models.dart';
import 'package:flutter_turbo_fetch_example/benchmark/verified_benchmark.dart';

void main() {
  final binding = TestWidgetsFlutterBinding.ensureInitialized();
  test('Missing observations remain unavailable', () {
    expect(summarizeSamples([]), isNull);
  });

  test('Statistics include the median and sample standard deviation', () {
    final result = summarizeSamples([1, 2, 3, 4])!;
    expect(result['mean'], 2.5);
    expect(result['median'], 2.5);
    expect(result['p95'], 4);
    expect(result['min'], 1);
    expect(result['max'], 4);
    expect(result['sample_stddev'], closeTo(1.2909944487, 0.00001));
  });

  test('The execution schedule rotates each adapter through each position', () {
    expect(rotatedPresets(0).map((p) => p.id),
        ['io_http1', 'dio_http2', 'turbo_fetch']);
    expect(rotatedPresets(1).map((p) => p.id),
        ['dio_http2', 'turbo_fetch', 'io_http1']);
    expect(rotatedPresets(2).map((p) => p.id),
        ['turbo_fetch', 'io_http1', 'dio_http2']);
  });

  test('Payload validation rejects incomplete or mismatched JSON', () {
    expect(() => validatePayload({'sizeKb': 5, 'count': 1, 'items': []}, 5),
        throwsFormatException);
    expect(
        () => validatePayload({
              'sizeKb': 10,
              'count': 1,
              'items': [{}]
            }, 5),
        throwsFormatException);
    expect(
        () => validatePayload({
              'sizeKb': 5,
              'count': 1,
              'items': [{}]
            }, 5),
        returnsNormally);
  });

  test(
      'Trials record complete-body bytes, protocol and successful JSON decoding',
      () async {
    const channel = BasicMessageChannel<Object?>(
      'dev.flutter.pigeon.flutter_turbo_fetch.TurboFetchHostApi.executeRequest',
      TurboFetchHostApi.pigeonChannelCodec,
    );
    binding.defaultBinaryMessenger
        .setMockDecodedMessageHandler<Object?>(channel, (message) async {
      final request = (message as List<Object?>).single! as TurboHttpRequest;
      final size = int.parse(Uri.parse(request.url).queryParameters['sizeKb']!);
      final bytes = Uint8List.fromList(utf8.encode(jsonEncode({
        'sizeKb': size,
        'count': 1,
        'items': [{}]
      })));
      return <Object?>[
        TurboHttpResponse(statusCode: 200, body: bytes, headers: {
          'content-type': ['application/json'],
          'content-length': ['${bytes.length}'],
          'x-http-version': ['2.0'],
        })
      ];
    });
    addTearDown(() => binding.defaultBinaryMessenger
        .setMockDecodedMessageHandler(channel, null));
    final client = VerifiedBenchmarkClient(
        BenchmarkPreset.turboFetch, 'https://localhost:8444', true);
    addTearDown(client.close);
    final result = await client.run(BenchmarkScenario.scenarioA, 0, 2);
    expect(result['success'], 90);
    expect(result['failure'], 0);
    final rows = result['requests'] as List<Map<String, Object?>>;
    expect(rows, hasLength(90));
    for (final row in rows) {
      expect(row['status'], 200);
      expect(row['response_bytes'], greaterThan(0));
      expect(row['http_version'], '2.0');
      expect(row['network_ms'], isNonNegative);
      expect(row['parse_ms'], isNonNegative);
    }
    expect(result['jank_frames'], isNull);
    expect(result['max_frame_ms'], isNull);
  });
}
