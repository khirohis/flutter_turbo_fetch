import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:flutter_turbo_fetch/flutter_turbo_fetch.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('TurboHttpAdapter instance test', (WidgetTester tester) async {
    final adapter = TurboHttpAdapter();
    expect(adapter, isNotNull);
  });
}
