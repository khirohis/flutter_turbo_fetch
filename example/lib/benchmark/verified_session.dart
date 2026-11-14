import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import 'benchmark_models.dart';
import 'verified_benchmark.dart';

/// Standalone runner widget for headless or automated split-pipeline benchmark execution.
class VerifiedBenchmarkApp extends StatelessWidget {
  const VerifiedBenchmarkApp({
    super.key,
    required this.baseUrl,
    this.allowLocalCertificate = false,
  });

  final String baseUrl;
  final bool allowLocalCertificate;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'flutter_turbo_fetch Verified Benchmark',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal),
        useMaterial3: true,
      ),
      home: VerifiedBenchmarkSession(
        baseUrl: baseUrl,
        allowLocalCertificate: allowLocalCertificate,
      ),
    );
  }
}

class VerifiedBenchmarkSession extends StatefulWidget {
  const VerifiedBenchmarkSession({
    super.key,
    required this.baseUrl,
    this.allowLocalCertificate = false,
  });

  final String baseUrl;
  final bool allowLocalCertificate;

  @override
  State<VerifiedBenchmarkSession> createState() =>
      _VerifiedBenchmarkSessionState();
}

class _VerifiedBenchmarkSessionState extends State<VerifiedBenchmarkSession> {
  String status = 'Preparing';
  late File evidence;

  @override
  void initState() {
    super.initState();
    _validateConfiguration();
    WidgetsBinding.instance.addPostFrameCallback((_) => _run());
  }

  void _validateConfiguration() {
    final uri = Uri.parse(widget.baseUrl);
    if (uri.scheme != 'https' ||
        uri.userInfo.isNotEmpty ||
        uri.query.isNotEmpty ||
        uri.fragment.isNotEmpty ||
        (uri.path.isNotEmpty && uri.path != '/')) {
      throw StateError('Requires an HTTPS origin without paths or query parameters');
    }
    if (widget.allowLocalCertificate && !_isPrivateHost(uri.host)) {
      throw StateError('Local TLS bypass requires a private IPv4 or localhost');
    }
  }

  static bool _isPrivateHost(String host) {
    if (host == 'localhost' || host == '127.0.0.1') return true;
    final parts = host.split('.').map(int.tryParse).toList();
    if (parts.length != 4 || parts.any((p) => p == null || p < 0 || p > 255)) {
      return false;
    }
    return parts[0] == 10 ||
        (parts[0] == 192 && parts[1] == 168) ||
        (parts[0] == 172 && parts[1]! >= 16 && parts[1]! <= 31);
  }

  Future<void> _run() async {
    evidence = File('${Directory.systemTemp.path}/ios-benchmark.jsonl');
    evidence.writeAsStringSync('');
    try {
      _emit({
        'type': 'session_start',
        'schema': 1,
        'mode': kReleaseMode ? 'release' : 'debug',
        'utc': DateTime.now().toUtc().toIso8601String(),
        'source_sha': const String.fromEnvironment('BENCHMARK_SOURCE_SHA'),
        'os': Platform.operatingSystemVersion,
        'local_tls_bypass': widget.allowLocalCertificate,
      });

      for (final scenario in BenchmarkScenario.values) {
        final clients = {
          for (final p in BenchmarkPreset.values)
            p: VerifiedBenchmarkClient(p, widget.baseUrl, widget.allowLocalCertificate)
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
                  ...request,
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
      if (mounted) setState(() => status = 'Failed: inspect host log');
    }
  }

  void _emit(Map<String, Object?> row) {
    final line = 'IOS_BENCHMARK ${jsonEncode(row)}';
    evidence.writeAsStringSync('$line\n', mode: FileMode.append, flush: true);
    // ignore: avoid_print
    print(line);
  }

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(title: const Text('Verified Release Benchmark')),
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
                'Keep this screen open. Results are streamed to stdout/host log.\n'
                'Frame metrics: N/A in this Release protocol.',
              ),
            ],
          ),
        ),
      );
}
