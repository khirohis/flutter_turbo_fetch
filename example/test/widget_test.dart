import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch_example/main.dart';

void main() {
  testWidgets('Verify example app renders', (WidgetTester tester) async {
    await tester.pumpWidget(const MyApp());
    expect(find.text('TurboFetch Benchmark'), findsOneWidget);
    expect(find.text('4プリセット一括連続実行 (推奨)'), findsOneWidget);
  });
}
