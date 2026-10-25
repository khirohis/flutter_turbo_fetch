import 'package:flutter/material.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    final adapter = TurboHttpAdapter();
    final transformer = TurboTransformer();
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('flutter_turbo_fetch Benchmark')),
        body: Center(
          child: Text(
            'Ready: ${adapter.runtimeType} + ${transformer.runtimeType}',
          ),
        ),
      ),
    );
  }
}
