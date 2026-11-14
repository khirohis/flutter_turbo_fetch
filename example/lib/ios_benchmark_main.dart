import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import 'benchmark/benchmark_models.dart';
import 'benchmark/verified_benchmark.dart';

const baseUrl = String.fromEnvironment('BENCHMARK_BASE_URL');
const allowLocalCertificate = bool.fromEnvironment('BENCHMARK_LOCAL_TLS');

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  if (!kReleaseMode || !Platform.isIOS || baseUrl.isEmpty) {
    throw StateError('Requires iOS Release and BENCHMARK_BASE_URL');
  }
  final uri = Uri.parse(baseUrl);
  if (uri.scheme != 'https' ||
      uri.userInfo.isNotEmpty ||
      uri.query.isNotEmpty ||
      uri.fragment.isNotEmpty ||
      uri.path.isNotEmpty && uri.path != '/') {
    throw StateError('Requires an HTTPS origin');
  }
  if (allowLocalCertificate && !_isPrivateHost(uri.host)) {
    throw StateError('Local TLS bypass requires a private IPv4 or localhost');
  }
  runApp(const MaterialApp(home: BenchmarkSession()));
}

bool _isPrivateHost(String host) {
  if (host == 'localhost' || host == '127.0.0.1') return true;
  final parts = host.split('.').map(int.tryParse).toList();
  if (parts.length != 4 || parts.any((p) => p == null || p < 0 || p > 255)) {
    return false;
  }
  return parts[0] == 10 ||
      parts[0] == 192 && parts[1] == 168 ||
      parts[0] == 172 && parts[1]! >= 16 && parts[1]! <= 31;
}

class BenchmarkSession extends StatefulWidget {
  const BenchmarkSession({super.key});
  @override
  State<BenchmarkSession> createState() => _BenchmarkSessionState();
}

class _BenchmarkSessionState extends State<BenchmarkSession> {
  String status = 'Preparing';
  late File evidence;
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _run());
  }

  Future<void> _run() async {
    evidence = File('${Directory.systemTemp.path}/ios-benchmark.jsonl');
    evidence.writeAsStringSync('');
    try {
      // The host log must retain this marker; absence is not a completed session.
      _emit({
        'type': 'session_start',
        'schema': 1,
        'mode': 'release',
        'utc': DateTime.now().toUtc().toIso8601String(),
        'source_sha': const String.fromEnvironment('BENCHMARK_SOURCE_SHA'),
        'os': Platform.operatingSystemVersion,
        'local_tls_bypass': allowLocalCertificate
      });
      for (final scenario in BenchmarkScenario.values) {
        final clients = {
          for (final p in BenchmarkPreset.values)
            p: VerifiedBenchmarkClient(p, baseUrl, allowLocalCertificate)
        };
        try {
          for (var trial = -1; trial < 10; trial++) {
            final order = rotatedPresets(trial < 0 ? 0 : trial);
            for (var position = 0; position < order.length; position++) {
              final preset = order[position];
              if (mounted) {
                setState(() => status =
                    '${scenario.id} / ${preset.id} / ${trial < 0 ? "warmup" : "trial ${trial + 1}/10"}');
              }
              // Let the same progress UI render before every timed trial.
              await WidgetsBinding.instance.endOfFrame;
              final result =
                  await clients[preset]!.run(scenario, trial, position);
              final requests =
                  result.remove('requests') as List<Map<String, Object?>>;
              _emit({'type': 'trial', ...result});
              for (final request in requests) {
                _emit({
                  'type': 'request',
                  'scenario': scenario.id,
                  'preset': preset.id,
                  'trial': trial,
                  ...request
                });
              }
              if (result['failure'] != 0) {
                throw StateError('Trial failed; results are incomplete');
              }
            }
          }
        } finally {
          for (final client in clients.values) {
            client.close();
          }
        }
      }
      _emit({'type': 'session_complete', 'trials': 90, 'warmups': 9});
      if (mounted) {
        setState(() => status = 'Complete: 90 measured trials, 9 warmups');
      }
    } catch (error, stack) {
      File('${Directory.systemTemp.path}/ios-benchmark-diagnostic.txt')
          .writeAsStringSync('$error\n$stack');
      _emit({'type': 'session_failed'});
      if (mounted) setState(() => status = 'Failed: inspect the host log');
    }
  }

  void _emit(Map<String, Object?> row) {
    // Emit outside timed sections; never include the origin or raw exception text.
    final line = 'IOS_BENCHMARK ${jsonEncode(row)}';
    evidence.writeAsStringSync('$line\n', mode: FileMode.append, flush: true);
    // ignore: avoid_print
    print(line);
  }

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(title: const Text('iOS Release benchmark')),
        body: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const LinearProgressIndicator(),
                const SizedBox(height: 24),
                Text(status),
                const SizedBox(height: 24),
                const Text(
                    'Keep this screen open. Results are sent to the host log.\nFrame metrics: N/A in this Release protocol.')
              ],
            )),
      );
}
