import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

import 'package:dio/dio.dart';
import 'package:dio/io.dart';
import 'package:dio_http2_adapter/dio_http2_adapter.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';
import 'package:native_dio_adapter/native_dio_adapter.dart';

void main() {
  runApp(const MyApp());
}

enum BenchmarkPreset {
  ioHttp1(
    label: '① IOHttpClientAdapter (HTTP/1.1)',
    shortName: '① IO (HTTP/1.1)',
  ),
  dioHttp2(
    label: '② dio_http2_adapter (Dart HTTP/2)',
    shortName: '② dio_http2',
  ),
  nativeDio(
    label: '③ native_dio_adapter (Stream Bridge)',
    shortName: '③ native_dio',
  ),
  turboFetch(
    label: '④ TurboHttpAdapter + TurboTransformer',
    shortName: '④ TurboFetch',
  );

  const BenchmarkPreset({required this.label, required this.shortName});
  final String label;
  final String shortName;
}

class BenchmarkResult {
  BenchmarkResult({
    required this.preset,
    required this.concurrency,
    required this.responseType,
    required this.totalElapsedMs,
    required this.firstCompletionMs,
    required this.avgLatencyMs,
    required this.maxLatencyMs,
    required this.totalFrames,
    required this.jankFrames,
    required this.maxFrameDurationMs,
    required this.rssBeforeMb,
    required this.rssPeakMb,
    required this.rssAfterMb,
    required this.successCount,
    this.errorMessage,
  });

  final BenchmarkPreset preset;
  final int concurrency;
  final ResponseType responseType;
  final int totalElapsedMs;
  final int firstCompletionMs;
  final double avgLatencyMs;
  final int maxLatencyMs;
  final int totalFrames;
  final int jankFrames;
  final double maxFrameDurationMs;
  final double rssBeforeMb;
  final double rssPeakMb;
  final double rssAfterMb;
  final int successCount;
  final String? errorMessage;
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'flutter_turbo_fetch Benchmark',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
        useMaterial3: true,
      ),
      home: const BenchmarkScreen(),
    );
  }
}

class BenchmarkScreen extends StatefulWidget {
  const BenchmarkScreen({super.key});

  @override
  State<BenchmarkScreen> createState() => _BenchmarkScreenState();
}

class _BenchmarkScreenState extends State<BenchmarkScreen>
    with SingleTickerProviderStateMixin {
  late final TextEditingController _urlController;
  late final AnimationController _spinnerController;

  BenchmarkPreset _selectedPreset = BenchmarkPreset.turboFetch;
  int _concurrency = 15;
  ResponseType _responseType = ResponseType.json;
  bool _allowBadCertificates = true;
  bool _isRunning = false;
  final List<BenchmarkResult> _results = [];

  // Frame timing collection state during active benchmark run
  bool _collectingFrames = false;
  int _totalFrames = 0;
  int _jankFrames = 0;
  double _maxFrameMs = 0.0;

  @override
  void initState() {
    super.initState();
    final defaultHost =
        (!Platform.isIOS && Platform.isAndroid) ? '10.0.2.2' : 'localhost';
    _urlController = TextEditingController(
      text: 'https://$defaultHost:8443/json?sizeKb=100',
    );
    _spinnerController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1000),
    )..repeat();
    SchedulerBinding.instance.addTimingsCallback(_onFrameTimings);
  }

  @override
  void dispose() {
    SchedulerBinding.instance.removeTimingsCallback(_onFrameTimings);
    _spinnerController.dispose();
    _urlController.dispose();
    super.dispose();
  }

  void _onFrameTimings(List<FrameTiming> timings) {
    if (!_collectingFrames) return;
    for (final timing in timings) {
      _totalFrames++;
      final spanMs = timing.totalSpan.inMicroseconds / 1000.0;
      if (spanMs > 16.67) {
        _jankFrames++;
      }
      if (spanMs > _maxFrameMs) {
        _maxFrameMs = spanMs;
      }
    }
  }

  Dio _createConfiguredDio(BenchmarkPreset preset) {
    final dio = Dio(
      BaseOptions(
        connectTimeout: const Duration(seconds: 15),
        receiveTimeout: const Duration(seconds: 30),
        sendTimeout: const Duration(seconds: 15),
      ),
    );

    switch (preset) {
      case BenchmarkPreset.ioHttp1:
        dio.httpClientAdapter = IOHttpClientAdapter(
          createHttpClient: () {
            final client = HttpClient();
            if (_allowBadCertificates) {
              client.badCertificateCallback = (cert, host, port) => true;
            }
            return client;
          },
        );
        dio.transformer = BackgroundTransformer();
      case BenchmarkPreset.dioHttp2:
        dio.httpClientAdapter = Http2Adapter(
          ConnectionManager(
            idleTimeout: const Duration(seconds: 15),
            onClientCreate: (_, config) {
              if (_allowBadCertificates) {
                config.onBadCertificate = (_) => true;
              }
            },
          ),
        );
        dio.transformer = BackgroundTransformer();
      case BenchmarkPreset.nativeDio:
        dio.httpClientAdapter = NativeAdapter();
        dio.transformer = BackgroundTransformer();
      case BenchmarkPreset.turboFetch:
        dio.httpClientAdapter = TurboHttpAdapter(
          allowBadCertificates: _allowBadCertificates,
        );
        dio.transformer = TurboTransformer();
    }
    return dio;
  }

  double _currentRssMb() {
    try {
      return ProcessInfo.currentRss / (1024 * 1024);
    } catch (_) {
      return 0.0;
    }
  }

  Future<void> _runBenchmark() async {
    if (_isRunning) return;
    final url = _urlController.text.trim();
    if (url.isEmpty) return;

    setState(() {
      _isRunning = true;
      _totalFrames = 0;
      _jankFrames = 0;
      _maxFrameMs = 0.0;
      _collectingFrames = true;
    });

    final dio = _createConfiguredDio(_selectedPreset);
    final rssBefore = _currentRssMb();
    double rssPeak = rssBefore;

    final rssTimer = Timer.periodic(const Duration(milliseconds: 25), (_) {
      final current = _currentRssMb();
      if (current > rssPeak) {
        rssPeak = current;
      }
    });

    final latencies = <int>[];
    int? firstCompletionMs;
    int successCount = 0;
    String? errorMsg;

    final overallStopwatch = Stopwatch()..start();

    try {
      final futures = List.generate(_concurrency, (index) async {
        final reqStopwatch = Stopwatch()..start();
        try {
          final separator = url.contains('?') ? '&' : '?';
          final requestUrl = '$url${separator}seq=$index';
          await dio.get<dynamic>(
            requestUrl,
            options: Options(responseType: _responseType),
          );
          reqStopwatch.stop();
          final elapsed = reqStopwatch.elapsedMilliseconds;
          latencies.add(elapsed);
          firstCompletionMs ??= overallStopwatch.elapsedMilliseconds;
          successCount++;
        } catch (e) {
          reqStopwatch.stop();
          errorMsg ??= e.toString();
        }
      });

      await Future.wait(futures);
    } finally {
      overallStopwatch.stop();
      rssTimer.cancel();
      _collectingFrames = false;
      dio.close(force: true);
    }

    final rssAfter = _currentRssMb();
    if (rssAfter > rssPeak) {
      rssPeak = rssAfter;
    }

    final avgLatency = latencies.isEmpty
        ? 0.0
        : latencies.reduce((a, b) => a + b) / latencies.length;
    final maxLatency = latencies.isEmpty ? 0 : latencies.reduce(math.max);

    final result = BenchmarkResult(
      preset: _selectedPreset,
      concurrency: _concurrency,
      responseType: _responseType,
      totalElapsedMs: overallStopwatch.elapsedMilliseconds,
      firstCompletionMs: firstCompletionMs ?? 0,
      avgLatencyMs: avgLatency,
      maxLatencyMs: maxLatency,
      totalFrames: _totalFrames,
      jankFrames: _jankFrames,
      maxFrameDurationMs: _maxFrameMs,
      rssBeforeMb: rssBefore,
      rssPeakMb: rssPeak,
      rssAfterMb: rssAfter,
      successCount: successCount,
      errorMessage: errorMsg,
    );

    debugPrint(
      '[BenchmarkResult] ${result.preset.shortName} | '
      'OK: ${result.successCount}/${result.concurrency} | '
      'Total: ${result.totalElapsedMs}ms | '
      '1st: ${result.firstCompletionMs}ms | '
      'Avg/Max: ${result.avgLatencyMs.toStringAsFixed(0)}/${result.maxLatencyMs}ms | '
      'Jank: ${result.jankFrames}/${result.totalFrames} (max ${result.maxFrameDurationMs.toStringAsFixed(1)}ms) | '
      'RSS: ${result.rssBeforeMb.toStringAsFixed(1)}->${result.rssPeakMb.toStringAsFixed(1)}->${result.rssAfterMb.toStringAsFixed(1)}MB'
      '${result.errorMessage != null ? " | Error: ${result.errorMessage}" : ""}',
    );

    if (!mounted) return;
    setState(() {
      _isRunning = false;
      _results.insert(0, result);
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('flutter_turbo_fetch Benchmark'),
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 16),
            child: RotationTransition(
              turns: _spinnerController,
              child: const Icon(
                Icons.sync,
                semanticLabel: 'UI Thread Smoothness Indicator',
              ),
            ),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    '1. Adapter / Transformer Preset',
                    style: TextStyle(fontWeight: FontWeight.bold),
                  ),
                  const SizedBox(height: 8),
                  Wrap(
                    spacing: 8,
                    runSpacing: 4,
                    children: BenchmarkPreset.values.map((preset) {
                      return ChoiceChip(
                        label: Text(preset.shortName),
                        selected: _selectedPreset == preset,
                        onSelected: _isRunning
                            ? null
                            : (selected) {
                                if (selected) {
                                  setState(() => _selectedPreset = preset);
                                }
                              },
                      );
                    }).toList(),
                  ),
                  const SizedBox(height: 16),
                  const Text(
                    '2. HTTP/2 Target URL',
                    style: TextStyle(fontWeight: FontWeight.bold),
                  ),
                  const SizedBox(height: 8),
                  TextField(
                    controller: _urlController,
                    enabled: !_isRunning,
                    decoration: const InputDecoration(
                      border: OutlineInputBorder(),
                      isDense: true,
                      hintText: 'https://localhost:8443/json?sizeKb=100',
                    ),
                  ),
                  const SizedBox(height: 8),
                  Wrap(
                    spacing: 8,
                    children: [
                      ActionChip(
                        label: const Text('Local 100KB (Android 10.0.2.2)'),
                        onPressed: _isRunning
                            ? null
                            : () => _urlController.text =
                                'https://10.0.2.2:8443/json?sizeKb=100',
                      ),
                      ActionChip(
                        label: const Text('Local 100KB (iOS localhost)'),
                        onPressed: _isRunning
                            ? null
                            : () => _urlController.text =
                                'https://localhost:8443/json?sizeKb=100',
                      ),
                      ActionChip(
                        label: const Text('Public DummyJSON'),
                        onPressed: _isRunning
                            ? null
                            : () => _urlController.text =
                                'https://dummyjson.com/products?limit=100',
                      ),
                    ],
                  ),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text(
                      'Allow self-signed certificates (for local mock server)',
                      style: TextStyle(fontSize: 13),
                    ),
                    value: _allowBadCertificates,
                    onChanged: _isRunning
                        ? null
                        : (v) => setState(() => _allowBadCertificates = v),
                  ),
                  const SizedBox(height: 8),
                  Row(
                    children: [
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text(
                              '3. Concurrency',
                              style: TextStyle(fontWeight: FontWeight.bold),
                            ),
                            const SizedBox(height: 8),
                            SegmentedButton<int>(
                              showSelectedIcon: false,
                              segments: const [
                                ButtonSegment(value: 10, label: Text('10')),
                                ButtonSegment(value: 15, label: Text('15')),
                                ButtonSegment(value: 20, label: Text('20')),
                              ],
                              selected: {_concurrency},
                              onSelectionChanged: _isRunning
                                  ? null
                                  : (s) => setState(() => _concurrency = s.first),
                            ),
                          ],
                        ),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text(
                              '4. ResponseType',
                              style: TextStyle(fontWeight: FontWeight.bold),
                            ),
                            const SizedBox(height: 8),
                            SegmentedButton<ResponseType>(
                              showSelectedIcon: false,
                              segments: const [
                                ButtonSegment(
                                  value: ResponseType.json,
                                  label: Text('json'),
                                ),
                                ButtonSegment(
                                  value: ResponseType.plain,
                                  label: Text('plain'),
                                ),
                                ButtonSegment(
                                  value: ResponseType.bytes,
                                  label: Text('bytes'),
                                ),
                              ],
                              selected: {_responseType},
                              onSelectionChanged: _isRunning
                                  ? null
                                  : (s) =>
                                      setState(() => _responseType = s.first),
                            ),
                          ],
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 16),
                  SizedBox(
                    width: double.infinity,
                    child: FilledButton.icon(
                      onPressed: _isRunning ? null : _runBenchmark,
                      icon: _isRunning
                          ? const SizedBox(
                              width: 18,
                              height: 18,
                              child: CircularProgressIndicator(
                                strokeWidth: 2,
                                color: Colors.white,
                              ),
                            )
                          : const Icon(Icons.play_arrow),
                      label: Text(
                        _isRunning
                            ? 'Running $_concurrency Parallel Requests...'
                            : 'Run Parallel Benchmark',
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 16),
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(
                'Benchmark Results (${_results.length})',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              if (_results.isNotEmpty)
                TextButton(
                  onPressed: _isRunning
                      ? null
                      : () => setState(() => _results.clear()),
                  child: const Text('Clear'),
                ),
            ],
          ),
          const SizedBox(height: 8),
          if (_results.isEmpty)
            const Card(
              child: Padding(
                padding: EdgeInsets.all(24),
                child: Center(
                  child: Text(
                    'Ready: Select a preset and tap "Run Parallel Benchmark".',
                  ),
                ),
              ),
            )
          else
            ..._results.map((r) => _ResultCard(result: r)),
        ],
      ),
    );
  }
}

class _ResultCard extends StatelessWidget {
  const _ResultCard({required this.result});

  final BenchmarkResult result;

  @override
  Widget build(BuildContext context) {
    final hasError = result.errorMessage != null;
    return Card(
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text(
                  result.preset.shortName,
                  style: const TextStyle(
                    fontWeight: FontWeight.bold,
                    fontSize: 16,
                  ),
                ),
                Text(
                  '${result.concurrency}x (${result.responseType.name}) • '
                  'OK: ${result.successCount}/${result.concurrency}',
                  style: Theme.of(context).textTheme.bodySmall,
                ),
              ],
            ),
            const Divider(),
            Wrap(
              spacing: 16,
              runSpacing: 8,
              children: [
                _MetricItem(
                  label: 'Total Time',
                  value: '${result.totalElapsedMs} ms',
                ),
                _MetricItem(
                  label: '1st Complete',
                  value: '${result.firstCompletionMs} ms',
                ),
                _MetricItem(
                  label: 'Avg / Max Req',
                  value:
                      '${result.avgLatencyMs.toStringAsFixed(0)} / ${result.maxLatencyMs} ms',
                ),
                _MetricItem(
                  label: 'UI Jank (>16ms)',
                  value: '${result.jankFrames}/${result.totalFrames} frames '
                      '(max ${result.maxFrameDurationMs.toStringAsFixed(1)} ms)',
                ),
                _MetricItem(
                  label: 'RSS Memory (Before → Peak → After)',
                  value: '${result.rssBeforeMb.toStringAsFixed(1)} → '
                      '${result.rssPeakMb.toStringAsFixed(1)} → '
                      '${result.rssAfterMb.toStringAsFixed(1)} MB',
                ),
              ],
            ),
            if (hasError) ...[
              const SizedBox(height: 8),
              Text(
                'Error: ${result.errorMessage}',
                style: TextStyle(
                  color: Theme.of(context).colorScheme.error,
                  fontSize: 12,
                ),
                maxLines: 3,
                overflow: TextOverflow.ellipsis,
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class _MetricItem extends StatelessWidget {
  const _MetricItem({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(label, style: Theme.of(context).textTheme.labelSmall),
        Text(value, style: const TextStyle(fontWeight: FontWeight.w600)),
      ],
    );
  }
}
