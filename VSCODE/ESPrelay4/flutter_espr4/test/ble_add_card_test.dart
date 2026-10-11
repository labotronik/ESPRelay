import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_espr4/main.dart';

class NoNetwork extends HttpOverrides {
  int calls = 0;
  @override
  HttpClient createHttpClient(SecurityContext? context) {
    calls++;
    throw StateError('Network must not be used to add a BLE card');
  }
}

void main() {
  for (final ip in ['192.168.88.50', '0.0.0.0']) {
    testWidgets('BLE addition and saving work without access to $ip', (
      tester,
    ) async {
      final network = NoNetwork();
      final previous = HttpOverrides.current;
      HttpOverrides.global = network;
      addTearDown(() => HttpOverrides.global = previous);
      EspCard? saved;
      final scan = BleScanResult(
        cardId: 'BCD5387D78FE',
        suggestedName: 'ESPRelay4-7D78FE',
        device: BluetoothDevice.fromId('BC:D5:38:7D:78:FE'),
      );
      await tester.pumpWidget(
        MaterialApp(
          home: Builder(
            builder: (context) {
              return Scaffold(
                body: ElevatedButton(
                  onPressed: () async {
                    saved = await Navigator.of(context).push<EspCard>(
                      MaterialPageRoute(
                        builder: (_) => CardEditPage(
                          initialBleScan: scan,
                          bleStateReader: (device, id) async => {
                            'device_id': id,
                            'eth': {'link': 1, 'ip': ip},
                            'mqtt': {
                              'eth_connected': 1,
                              'active_transport': 'ethernet',
                            },
                            'total_relays': 8,
                            'total_inputs': 8,
                            'fw': 'test-fw',
                          },
                        ),
                      ),
                    );
                  },
                  child: const Text('Ajouter'),
                ),
              );
            },
          ),
        ),
      );
      await tester.tap(find.text('Ajouter'));
      await tester.pumpAndSettle();
      expect(
        find.text('8 sorties · 8 entrées · Firmware test-fw'),
        findsOneWidget,
      );
      expect(find.byType(AlertDialog), findsNothing);
      expect(
        find.text('Importer la configuration par le réseau'),
        findsNothing,
      );
      expect(
        find.text(
          'Connexion directe en BLE. Aucun accès Ethernet, Wi-Fi ou MQTT requis.',
        ),
        findsOneWidget,
      );
      await tester.tap(find.text('Enregistrer carte'));
      await tester.pumpAndSettle();
      expect(saved, isNotNull);
      expect(saved!.cardId, scan.cardId);
      expect(saved!.policy, TransportPolicy.ble);
      expect(network.calls, 0);
      if (ip != '0.0.0.0') expect(saved!.httpHost, ip);
      expect(tester.takeException(), isNull);
    });
  }
}
