import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_app/main.dart';

void main() {
  test('BLE state rejects incomplete JSON and non-object frames', () {
    expect(parseBleState('{"inputs":[1],"relays":[0]}')?['inputs'], [1]);
    expect(parseBleState('{"inputs":'), isNull);
    expect(parseBleState('[1,2]'), isNull);
  });
  test('BLE framing handles fragmentation, UTF-8 and consecutive frames', () {
    final framer = BleStateFramer();
    final bytes = utf8.encode('\x1e\x1e{"name":"Volet été"}\x1f');
    for (var i = 0; i < bytes.length - 1; i++) {
      expect(framer.add([bytes[i]]), isEmpty);
    }
    expect(framer.add([bytes.last]).single.state['name'], 'Volet été');
    final frames = framer.add(utf8.encode(
        'noise\x1e\x1e{broken\x1f\x1e\x1e{"relay":0}\x1f\x1e\x1e{"relay":1}\x1f'));
    expect(frames.map((f) => f.state['relay']).toList(), [0, 1]);
    framer.add(utf8.encode('\x1e\x1e{"old":'));
    framer.clear();
    expect(framer.add(utf8.encode('1}\x1f')), isEmpty);
  });
  testWidgets(
      'BLE screen displays controls and cleans up its stream on disposal',
      (tester) async {
    final adapter = StreamController<BluetoothAdapterState>();
    await tester.pumpWidget(
        EspRelayApp(autoScan: false, adapterStates: adapter.stream));
    expect(find.text('ESPRelay4 BLE'), findsOneWidget);
    expect(find.text('Scan for ESPRelay4'), findsOneWidget);
    adapter.add(BluetoothAdapterState.on);
    await tester.pump();
    expect(adapter.hasListener, isTrue);
    await tester.pumpWidget(const SizedBox());
    await tester.pump();
    expect(adapter.hasListener, isFalse);
    adapter.add(BluetoothAdapterState.off);
    await tester.pump();
    expect(tester.takeException(), isNull);
    unawaited(adapter.close());
    await tester.pump();
  });
}
