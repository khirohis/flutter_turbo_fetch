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

  /// シナリオごとの結果キャッシュ: scenario.id -> Map of preset to BenchmarkScenarioResult
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

  /// 単一プリセットの実行
  Future<void> _runSinglePreset(BenchmarkPreset preset) async {
    if (_isRunning) return;
    await _executeBenchmarks([preset]);
  }

  /// 4つのプリセットを一括連続実行
  Future<void> _runAllPresets() async {
    if (_isRunning) return;
    await _executeBenchmarks(BenchmarkPreset.values);
  }

  /// 指定プリセット群を順次実行
  Future<void> _executeBenchmarks(List<BenchmarkPreset> presets) async {
    final baseUrl = _baseUrlController.text.trim();
    if (baseUrl.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Base URL を入力してください')),
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

      // 完了時に Markdown をコンソール出力
      final markdown = BenchmarkFormatter.formatMarkdownTable(
        scenario: _selectedScenario,
        results: _currentResults.values.toList(),
      );
      debugPrint('\n=== BENCHMARK OUTPUT ===\n$markdown\n========================\n');
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('エラーが発生しました: $e')),
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

  /// 結果 Markdown をクリップボードにコピー
  void _copyMarkdownToClipboard() {
    final results = _currentResults.values.toList();
    if (results.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('コピーする計測結果がありません')),
      );
      return;
    }

    final markdown = BenchmarkFormatter.formatMarkdownTable(
      scenario: _selectedScenario,
      results: results,
    );

    Clipboard.setData(ClipboardData(text: markdown));
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('Markdown をクリップボードにコピーしました')),
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
            tooltip: 'Markdown をコピー',
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
              '1. 接続先モックサーバー設定',
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
                helperText: 'HTTP/2 Mock Server (ポート 8443)',
              ),
            ),
            const SizedBox(height: 8),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('自己署名証明書を許可 (Bad Certificates)'),
              subtitle: const Text('ローカル検証用 (自作オレオレ証明書を許可)'),
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
              '2. 計測シナリオ選択',
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
                    '検証目的: ${_selectedScenario.description}',
                    style: TextStyle(
                      fontSize: 12,
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                  const SizedBox(height: 4),
                  Text(
                    '構成: ${_selectedScenario.sizesKiB.join(", ")} KiB 各サイズ順次 × ${_selectedScenario.rounds} 周 (計 ${_selectedScenario.totalRequests} req / 最大 ${_selectedScenario.concurrency} 並列)',
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
              '3. ベンチマーク実行',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 12),
            SizedBox(
              width: double.infinity,
              height: 48,
              child: FilledButton.icon(
                icon: const Icon(Icons.play_arrow),
                label: const Text(
                  '4プリセット一括連続実行 (推奨)',
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
                      labelText: '個別プリセット',
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
                  child: const Text('個別実行'),
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
                        _currentRunningPreset?.label ?? '計測中...',
                        style: const TextStyle(fontWeight: FontWeight.bold),
                      ),
                      Text(
                        '進捗: $_progressCompleted / $_progressTotal リクエスト (${(progressVal * 100).toStringAsFixed(1)}%)',
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
                  '4. 比較結果サマリー',
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
                ),
                TextButton.icon(
                  icon: const Icon(Icons.copy, size: 16),
                  label: const Text('Markdown コピー'),
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
                  DataColumn(label: Text('構成')),
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
