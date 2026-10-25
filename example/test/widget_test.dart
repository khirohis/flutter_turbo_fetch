import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_turbo_fetch_example/main.dart';

void main() {
  testWidgets('Verify example app renders', (WidgetTester tester) async {
    await tester.pumpWidget(const MyApp());
    expect(
      find.byWidgetPredicate(
        (Widget widget) =>
            widget is Text && widget.data!.startsWith('Ready:'),
      ),
      findsOneWidget,
    );
  });
}
