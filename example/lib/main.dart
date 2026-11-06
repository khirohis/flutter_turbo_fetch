import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'benchmark/benchmark_formatter.dart';
import 'benchmark/benchmark_models.dart';
import 'benchmark/benchmark_runner.dart';

export 'benchmark/benchmark_formatter.dart';
export 'benchmark/benchmark_models.dart';
export 'benchmark/benchmark_runner.dart';

void main() {
  runApp(const MyApp());
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
  late final TextEditingController _baseUrlController;
  late final AnimationController _spinnerController;

  BenchmarkScenario _selectedScenario = BenchmarkScenario.scenarioA;
  BenchmarkPreset _selectedPreset = BenchmarkPreset.turboFetch;
  bool _allowBadCertificates = true;

  bool _isRunning = false;
  BenchmarkPreset? _currentRunningPreset;
  int _progressCompleted = 0;
  int _progressTotal = 0;

  /// Result cache by scenario: scenario.id -> Map of preset to BenchmarkScenarioResult
  final Map<String, Map<BenchmarkPreset, BenchmarkScenarioResult>> _resultsByScenario = {};

  @override
  void initState() {
    super.initState();
    final defaultHost =
        (!Platform.isIOS && Platform.isAndroid) ? '10.0.2.2' : 'localhost';
    _baseUrlController = TextEditingController(
      text: 'https://$defaultHost:8443',
    );
    _spinnerController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1000),
    )..repeat();
  }

  @override
  void dispose() {
    _spinnerController.dispose();
    _baseUrlController.dispose();
    super.dispose();
  }

  Map<BenchmarkPreset, BenchmarkScenarioResult> get _currentResults =>
      _resultsByScenario[_selectedScenario.id] ??= {};

  /// Executes a single preset.
  Future<void> _runSinglePreset(BenchmarkPreset preset) async {
    if (_isRunning) return;
    await _executeBenchmarks([preset]);
  }

  /// Executes all three presets sequentially.
  Future<void> _runAllPresets() async {
    if (_isRunning) return;
    await _executeBenchmarks(BenchmarkPreset.values);
  }

  /// Sequentially executes the specified presets.
  Future<void> _executeBenchmarks(List<BenchmarkPreset> presets) async {
    final baseUrl = _baseUrlController.text.trim();
    if (baseUrl.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Please enter a Base URL')),
      );
      return;
    }

    setState(() {
      _isRunning = true;
      _progressCompleted = 0;
      _progressTotal = _selectedScenario.totalRequests * presets.length;
    });

    int overallCompleted = 0;

    try {
      for (final preset in presets) {
        setState(() {
          _currentRunningPreset = preset;
        });

        final result = await BenchmarkRunner.runScenario(
          scenario: _selectedScenario,
          preset: preset,
          baseUrl: baseUrl,
          allowBadCertificates: _allowBadCertificates,
          onProgress: (completed, total) {
            setState(() {
              _progressCompleted = overallCompleted + completed;
            });
          },
        );

        overallCompleted += _selectedScenario.totalRequests;

        setState(() {
          _currentResults[preset] = result;
        });
      }

      // Print Markdown table to console upon completion.
      final markdown = BenchmarkFormatter.formatMarkdownTable(
        scenario: _selectedScenario,
        results: _currentResults.values.toList(),
      );
      debugPrint('\n=== BENCHMARK OUTPUT ===\n$markdown\n========================\n');
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Error occurred: $e')),
        );
      }
    } finally {
      if (mounted) {
        setState(() {
          _isRunning = false;
          _currentRunningPreset = null;
        });
      }
    }
  }

  /// Copies the resulting Markdown table to the clipboard.
  void _copyMarkdownToClipboard() {
    final results = _currentResults.values.toList();
    if (results.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('No benchmark results to copy')),
      );
      return;
    }

    final markdown = BenchmarkFormatter.formatMarkdownTable(
      scenario: _selectedScenario,
      results: results,
    );

    Clipboard.setData(ClipboardData(text: markdown));
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('Markdown copied to clipboard')),
    );
  }

  @override
  Widget build(BuildContext context) {
    final resultsList = _currentResults.values.toList();

    return Scaffold(
      appBar: AppBar(
        title: const Text('TurboFetch Benchmark'),
        actions: [
          IconButton(
            tooltip: 'Copy Markdown',
            icon: const Icon(Icons.copy_all),
            onPressed: resultsList.isNotEmpty && !_isRunning
                ? _copyMarkdownToClipboard
                : null,
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _buildConnectionCard(),
          const SizedBox(height: 16),
          _buildScenarioSelectionCard(),
          const SizedBox(height: 16),
          _buildActionCard(),
          if (_isRunning) ...[
            const SizedBox(height: 16),
            _buildProgressCard(),
          ],
          if (resultsList.isNotEmpty) ...[
            const SizedBox(height: 16),
            _buildResultsCard(resultsList),
          ],
        ],
      ),
    );
  }

  Widget _buildConnectionCard() {
    return Card(
      elevation: 1,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text(
              '1. Mock Server Connection',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _baseUrlController,
              enabled: !_isRunning,
              decoration: const InputDecoration(
                labelText: 'Mock Server Base URL',
                border: OutlineInputBorder(),
                isDense: true,
                helperText: 'HTTP/2 Mock Server (Port 8443)',
              ),
            ),
            const SizedBox(height: 8),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Allow Self-Signed Certificates'),
              subtitle: const Text('For local testing with self-signed TLS'),
              value: _allowBadCertificates,
              onChanged: _isRunning
                  ? null
                  : (v) => setState(() => _allowBadCertificates = v),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildScenarioSelectionCard() {
    return Card(
      elevation: 1,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text(
              '2. Benchmark Scenario',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 12),
            SegmentedButton<BenchmarkScenario>(
              segments: BenchmarkScenario.values.map((s) {
                return ButtonSegment<BenchmarkScenario>(
                  value: s,
                  label: Text(
                    s.shortLabel,
                    style: const TextStyle(fontSize: 12),
                  ),
                );
              }).toList(),
              selected: {_selectedScenario},
              onSelectionChanged: _isRunning
                  ? null
                  : (set) => setState(() => _selectedScenario = set.first),
            ),
            const SizedBox(height: 12),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Theme.of(context).colorScheme.surfaceContainerHighest,
                borderRadius: BorderRadius.circular(8),
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    _selectedScenario.label,
                    style: const TextStyle(fontWeight: FontWeight.bold),
                  ),
                  const SizedBox(height: 4),
                  Text(
                    'Purpose: ${_selectedScenario.description}',
                    style: TextStyle(
                      fontSize: 12,
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                  const SizedBox(height: 4),
                  Text(
                    'Workload: ${_selectedScenario.sizesKiB.join(", ")} KiB sequentially x ${_selectedScenario.rounds} rounds (${_selectedScenario.totalRequests} reqs / max ${_selectedScenario.concurrency} workers)',
                    style: TextStyle(
                      fontSize: 12,
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildActionCard() {
    return Card(
      elevation: 1,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text(
              '3. Run Benchmark',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 12),
            SizedBox(
              width: double.infinity,
              height: 48,
              child: FilledButton.icon(
                icon: const Icon(Icons.play_arrow),
                label: const Text(
                  'Run All 3 Presets (Recommended)',
                  style: TextStyle(fontSize: 15, fontWeight: FontWeight.bold),
                ),
                onPressed: _isRunning ? null : _runAllPresets,
              ),
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: DropdownButtonFormField<BenchmarkPreset>(
                    initialValue: _selectedPreset,
                    isExpanded: true,
                    decoration: const InputDecoration(
                      labelText: 'Single Preset',
                      border: OutlineInputBorder(),
                      isDense: true,
                    ),
                    items: BenchmarkPreset.values.map((p) {
                      return DropdownMenuItem(
                        value: p,
                        child: Text(
                          p.shortName,
                          overflow: TextOverflow.ellipsis,
                        ),
                      );
                    }).toList(),
                    onChanged: _isRunning
                        ? null
                        : (v) {
                            if (v != null) setState(() => _selectedPreset = v);
                          },
                  ),
                ),
                const SizedBox(width: 12),
                OutlinedButton(
                  onPressed: _isRunning
                      ? null
                      : () => _runSinglePreset(_selectedPreset),
                  child: const Text('Run Single'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildProgressCard() {
    final progressVal = _progressTotal > 0
        ? (_progressCompleted / _progressTotal).clamp(0.0, 1.0)
        : 0.0;

    return Card(
      color: Theme.of(context).colorScheme.primaryContainer,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              children: [
                RotationTransition(
                  turns: _spinnerController,
                  child: Icon(
                    Icons.sync,
                    color: Theme.of(context).colorScheme.primary,
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        _currentRunningPreset?.label ?? 'Running...',
                        style: const TextStyle(fontWeight: FontWeight.bold),
                      ),
                      Text(
                        'Progress: $_progressCompleted / $_progressTotal requests (${(progressVal * 100).toStringAsFixed(1)}%)',
                        style: const TextStyle(fontSize: 12),
                      ),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            LinearProgressIndicator(value: progressVal),
          ],
        ),
      ),
    );
  }

  Widget _buildResultsCard(List<BenchmarkScenarioResult> results) {
    return Card(
      elevation: 2,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                const Text(
                  '4. Benchmark Results Summary',
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
                ),
                TextButton.icon(
                  icon: const Icon(Icons.copy, size: 16),
                  label: const Text('Copy Markdown'),
                  onPressed: _isRunning ? null : _copyMarkdownToClipboard,
                ),
              ],
            ),
            const SizedBox(height: 8),
            SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: DataTable(
                columnSpacing: 16,
                columns: const [
                  DataColumn(label: Text('Configuration')),
                  DataColumn(label: Text('Total Time')),
                  DataColumn(label: Text('Net Time (Avg/P95)')),
                  DataColumn(label: Text('Parse Time (Avg/P95)')),
                  DataColumn(label: Text('Jank (%)')),
                  DataColumn(label: Text('Max Frame')),
                  DataColumn(label: Text('Peak RSS (Δ)')),
                ],
                rows: results.map((r) {
                  final isTurbo = r.preset == BenchmarkPreset.turboFetch;
                  final textStyle = isTurbo
                      ? const TextStyle(fontWeight: FontWeight.bold, color: Colors.indigo)
                      : null;

                  return DataRow(
                    cells: [
                      DataCell(Text(r.preset.shortName, style: textStyle)),
                      DataCell(Text('${r.totalWallTimeMs} ms', style: textStyle)),
                      DataCell(
                        Text(
                          '${r.netTimeAvgMs.toStringAsFixed(1)} / ${r.netTimeP95Ms.toStringAsFixed(1)} ms',
                          style: textStyle,
                        ),
                      ),
                      DataCell(
                        Text(
                          '${r.parseTimeAvgMs.toStringAsFixed(1)} / ${r.parseTimeP95Ms.toStringAsFixed(1)} ms',
                          style: textStyle,
                        ),
                      ),
                      DataCell(
                        Text(
                          '${r.jankFrames} (${r.jankRatePercent.toStringAsFixed(1)}%)',
                          style: textStyle,
                        ),
                      ),
                      DataCell(
                        Text(
                          '${r.maxFrameDurationMs.toStringAsFixed(1)} ms',
                          style: textStyle,
                        ),
                      ),
                      DataCell(
                        Text(
                          '${r.rssPeakMiB.toStringAsFixed(1)} (+${r.rssDeltaMiB.toStringAsFixed(1)}) MiB',
                          style: textStyle,
                        ),
                      ),
                    ],
                  );
                }).toList(),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
