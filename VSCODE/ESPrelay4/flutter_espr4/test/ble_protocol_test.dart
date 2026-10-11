import 'dart:convert';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_espr4/ble_protocol.dart';

void main() {
  test('BLE commands fit the minimum ATT payload and validate actions', () {
    expect(bleControlCommand('I', 0, ''), 'IDENTIFY');
    expect(() => bleControlCommand('I', 1, ''), throwsArgumentError);
    expect(bleControlCommand('R', 1, 'ON'), 'R1:ON');
    expect(bleControlCommand('R', 16, 'AUTO'), 'R16:AUTO');
    expect(bleControlCommand('V', 4, 'TOGGLE'), 'V4:TOGGLE');
    expect(utf8.encode(bleControlCommand('V', 16, 'TOGGLE')).length, lessThanOrEqualTo(20));
    for (final index in [0, 17]) {
      expect(() => bleControlCommand('R', index, 'ON'), throwsArgumentError);
    }
    expect(() => bleControlCommand('V', 1, 'AUTO'), throwsArgumentError);
    expect(() => bleControlCommand('R', 1, 'INVALID'), throwsArgumentError);
    expect(() => bleControlCommand('X', 1, 'ON'), throwsArgumentError);
  });

  test('notifications preserve fragmented UTF-8 and multiple state frames', () {
    final framer = BleStateFramer();
    final first = utf8.encode(jsonEncode({'device_id': 'ABC', 'name': 'Relais été', 'relays': [1, 0]}));
    final split = first.indexOf(0xc3) + 1;
    expect(framer.add([0x00, 0x1e]), isEmpty);
    expect(framer.add([0x1e, ...first.take(split)]), isEmpty);
    final frames = framer.add([...first.skip(split), 0x1f, 0x1e, 0x1e, ...utf8.encode('{"override":[1,-1]}'), 0x1f]);
    expect(frames.length, 2);
    expect(frames.first.state['name'], 'Relais été');
    expect(frames.last.state['override'], [1, -1]);
  });

  test('malformed notifications recover and disconnect clears partial frames', () {
    final framer = BleStateFramer();
    expect(framer.add([0x1e, 0x1e, 0xff, 0x1f]), isEmpty);
    expect(framer.add([0x1e, 0x1e, ...utf8.encode('{"relays":')]), isEmpty);
    framer.clear();
    expect(framer.add([...utf8.encode('[1]}'), 0x1f]), isEmpty);
    expect(framer.add([0x1e, 0x1e, ...utf8.encode('{"relays":[0]}'), 0x1f]).single.state['relays'], [0]);
  });
}
