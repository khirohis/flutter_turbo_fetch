import 'dart:async';
import 'dart:io' show sleep;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:dio/dio.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:flutter_turbo_fetch/src/messages.g.dart';

/// Test double for [TurboFetchHostApi].
///
/// If [onExecute] is omitted, defaults to returning HTTP 200 OK with an empty body.
class FakeTurboFetchHostApi extends TurboFetchHostApi {
  FakeTurboFetchHostApi({this.onExecute, this.onCancel});

  final Future<TurboHttpResponse> Function(TurboHttpRequest request)? onExecute;
  final void Function(String requestId)? onCancel;

  TurboHttpRequest? lastRequest;
  final List<TurboHttpRequest> requests = [];
  final List<String> cancelledRequestIds = [];

  @override
  Future<TurboHttpResponse> executeRequest(TurboHttpRequest request) {
    lastRequest = request;
    requests.add(request);
    return onExecute?.call(request) ??
        Future.value(TurboHttpResponse(statusCode: 200, body: Uint8List(0)));
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

  @override
  String toString() => 'TestUser(name: $name)';
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

// Top-level mappers for Worker Isolate tests (per TurboOptions isolate safety spec).
// Must remain top-level or static functions to ensure cross-isolate transferability via Isolate.run.

/// Computes length and first byte offset in Worker Isolate.
int bytesLengthAndFirstByteMapper(dynamic data) {
  final bytes = data as Uint8List;
  return bytes.lengthInBytes + (bytes.isNotEmpty ? bytes[0] : 0);
}

/// Inspects string length and Worker Isolate debug name.
String plainSummaryMapper(dynamic data) {
  final text = data as String;
  final isolateName = Isolate.current.debugName ?? 'unknown';
  return 'len=${text.length};isolate=$isolateName';
}

/// Inspects JSON array count and Worker Isolate debug name.
Map<String, Object?> jsonIsolateInspectorMapper(dynamic data) {
  final map = data as Map<String, dynamic>;
  return <String, Object?>{
    'count': (map['items'] as List<dynamic>).length,
    'isolateDebugName': Isolate.current.debugName,
  };
}

/// Simulates a short Worker Isolate task with sleep to test semaphore queuing.
int slowWorkerMapper(dynamic data) {
  sleep(const Duration(milliseconds: 40));
  final map = data as Map<String, dynamic>;
  return map['id'] as int;
}
