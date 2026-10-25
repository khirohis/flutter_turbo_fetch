import 'dart:convert';
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';

void main() {
  test('TurboTransformer decodes JSON payload', () async {
    final transformer = TurboTransformer();
    final jsonBytes = Uint8List.fromList(utf8.encode('{"status":"ok"}'));
    final result = await transformer.transformResponse(
      RequestOptions(path: '/test', responseType: ResponseType.json),
      ResponseBody.fromBytes(jsonBytes, 200),
    );
    expect(result, {'status': 'ok'});
  });
}
