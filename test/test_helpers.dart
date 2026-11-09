import 'dart:async';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:dio/dio.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:flutter_turbo_fetch/src/messages.g.dart';

/// Test double for [TurboFetchHostApi].
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

/// Simple model for testing typed mapper conversions.
class TestUser {
  const TestUser(this.name);
  final String name;

  @override
  bool operator ==(Object other) => other is TestUser && other.name == name;

  @override
  int get hashCode => name.hashCode;
}

/// Helper to configure a standard [Dio] instance wired with turbo components.
Dio createTestDio(
  TurboFetchHostApi hostApi, {
  int isolateThresholdBytes = TurboTransformer.defaultIsolateThresholdBytes,
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
int bytesLengthAndFirstByteMapper(dynamic data) {
  final bytes = data as Uint8List;
  return bytes.lengthInBytes + (bytes.isNotEmpty ? bytes[0] : 0);
}

String plainSummaryMapper(dynamic data) {
  final text = data as String;
  final isolateName = Isolate.current.debugName ?? 'unknown';
  return 'len=${text.length};isolate=$isolateName';
}

Map<String, Object?> jsonIsolateInspectorMapper(dynamic data) {
  final map = data as Map<String, dynamic>;
  return <String, Object?>{
    'count': (map['items'] as List<dynamic>).length,
    'isolateDebugName': Isolate.current.debugName,
  };
}

int slowWorkerMapper(dynamic data) {
  final sw = Stopwatch()..start();
  while (sw.elapsedMilliseconds < 40) {
    // Busy wait 40ms inside Worker Isolate to test semaphore queuing deterministically
  }
  final map = data as Map<String, dynamic>;
  return map['id'] as int;
}
