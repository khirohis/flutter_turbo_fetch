import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch_example/main.dart';

void main() {
  testWidgets('Verify example app renders', (WidgetTester tester) async {
    await tester.pumpWidget(const MyApp());
    expect(find.text('TurboFetch Benchmark'), findsOneWidget);
    expect(find.text('Run All 3 Presets (Recommended)', skipOffstage: false), findsOneWidget);
  });
}
