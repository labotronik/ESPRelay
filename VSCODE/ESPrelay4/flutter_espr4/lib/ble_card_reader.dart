import 'dart:async';
import 'dart:convert';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'ble_protocol.dart';

Future<Map<String, dynamic>> readBleCardState(
  BluetoothDevice device,
  String cardId,
) async {
  final suffix = cardId.toUpperCase();
  final serviceUuid = Guid('6E400001-B5A3-F393-E0A9-$suffix');
  final readUuid = Guid('6E400004-B5A3-F393-E0A9-$suffix');
  final notifyUuid = Guid('6E400003-B5A3-F393-E0A9-$suffix');
  var connectedHere = false;
  var notificationsEnabledHere = false;
  StreamSubscription<List<int>>? subscription;
  BluetoothCharacteristic? notify;
  bool matches(Map<String, dynamic>? state) =>
      state?['device_id']?.toString().toUpperCase() == suffix;
  try {
    await FlutterBluePlus.stopScan();
    if (!device.isConnected) {
      await device.connect(
        timeout: const Duration(seconds: 10),
        autoConnect: false,
      );
      connectedHere = true;
    }
    BluetoothCharacteristic? read;
    for (final service in await device.discoverServices()) {
      if (service.uuid != serviceUuid) continue;
      for (final characteristic in service.characteristics) {
        if (characteristic.uuid == readUuid) read = characteristic;
        if (characteristic.uuid == notifyUuid) notify = characteristic;
      }
    }
    if (read != null) {
      for (var attempt = 0; attempt < 6; attempt++) {
        try {
          final state = parseBleState(utf8.decode(await read.read()));
          if (matches(state)) return state!;
        } catch (_) {
          // Some devices cannot read the full JSON; use framed notifications.
        }
        await Future<void>.delayed(const Duration(milliseconds: 650));
      }
    }
    if (notify == null) throw StateError('État BLE indisponible');
    final completer = Completer<Map<String, dynamic>>();
    final framer = BleStateFramer();
    subscription = notify.onValueReceived.listen((bytes) {
      for (final frame in framer.add(bytes)) {
        if (matches(frame.state) && !completer.isCompleted) {
          completer.complete(frame.state);
        }
      }
    });
    if (!notify.isNotifying) {
      await notify.setNotifyValue(true);
      notificationsEnabledHere = true;
    }
    return await completer.future.timeout(const Duration(seconds: 5));
  } finally {
    await subscription?.cancel();
    if (notificationsEnabledHere) {
      try {
        await notify?.setNotifyValue(false);
      } catch (_) {}
    }
    if (connectedHere) {
      try {
        await device.disconnect();
      } catch (_) {}
    }
  }
}
