import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

const String kTargetName = 'ESPRelay4';

Guid _uuidFor(String base, String suffix12) {
  return Guid('6E4000$base-B5A3-F393-E0A9-$suffix12');
}

String? _extractSuffixFromService(Guid serviceUuid) {
  final s = serviceUuid.toString().toUpperCase();
  const prefix = '6E400001-B5A3-F393-E0A9-';
  if (!s.startsWith(prefix)) return null;
  final suffix = s.substring(prefix.length);
  return suffix.length == 12 ? suffix : null;
}

void main() {
  runApp(const EspRelayApp());
}

class EspRelayApp extends StatelessWidget {
  const EspRelayApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ESPRelay4 BLE',
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF0C7C59)),
      ),
      home: const BleHomePage(),
    );
  }
}

class BleHomePage extends StatefulWidget {
  const BleHomePage({super.key});

  @override
  State<BleHomePage> createState() => _BleHomePageState();
}

class _BleHomePageState extends State<BleHomePage> {
  final List<ScanResult> _results = [];
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<BluetoothAdapterState>? _adapterSub;
  StreamSubscription<List<int>>? _notifySub;
  Timer? _pollTimer;

  BluetoothDevice? _device;
  BluetoothCharacteristic? _stateChar;
  BluetoothCharacteristic? _stateReadChar;
  Guid? _serviceUuid;
  Guid? _stateCharUuid;
  Guid? _stateReadCharUuid;
  String _stateJson = '--';
  String _stateMeta = '';
  String _status = 'Idle';
  Map<String, dynamic> _state = {};
  String _rxBuffer = '';
  bool _scanning = false;
  BluetoothAdapterState _adapterState = BluetoothAdapterState.unknown;
  String _permStatus = '';
  int _lastFrameMs = 0;

  @override
  void initState() {
    super.initState();
    _adapterSub = FlutterBluePlus.adapterState.listen((s) {
      setState(() => _adapterState = s);
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _startScan();
    });
  }

  @override
  void dispose() {
    _notifySub?.cancel();
    _pollTimer?.cancel();
    _scanSub?.cancel();
    _adapterSub?.cancel();
    _disconnect();
    super.dispose();
  }

  Future<void> _startScan() async {
    if (_scanning) return;
    final permsOk = await _ensurePermissions();
    if (!permsOk) {
      setState(() => _status = 'Permissions required');
      return;
    }
    _results.clear();
    setState(() {
      _scanning = true;
      _status = 'Scanning...';
    });
    await FlutterBluePlus.startScan(timeout: const Duration(seconds: 5));
    _scanSub?.cancel();
    _scanSub = FlutterBluePlus.scanResults.listen((results) {
      for (final r in results) {
        final advName = r.advertisementData.advName;
        final name =
            r.device.platformName.isNotEmpty ? r.device.platformName : advName;
        final isTarget = name.contains(kTargetName);
        final idx =
            _results.indexWhere((e) => e.device.remoteId == r.device.remoteId);
        if (idx >= 0) {
          _results[idx] = r;
        } else {
          // If target name is missing (some devices hide it), still show the device.
          if (isTarget || name.isNotEmpty) _results.add(r);
        }
      }
      setState(() {});
    });
    await Future.delayed(const Duration(seconds: 5));
    await FlutterBluePlus.stopScan();
    setState(() {
      _scanning = false;
      _status = _results.isEmpty ? 'No device found' : 'Scan complete';
    });
  }

  Future<bool> _ensurePermissions() async {
    if (Theme.of(context).platform == TargetPlatform.android) {
      final scan = await Permission.bluetoothScan.request();
      final connect = await Permission.bluetoothConnect.request();
      final loc = await Permission.locationWhenInUse.request();
      final ok = scan.isGranted && connect.isGranted && loc.isGranted;
      setState(() {
        _permStatus =
            'scan=${scan.isGranted ? "ok" : "no"} connect=${connect.isGranted ? "ok" : "no"} location=${loc.isGranted ? "ok" : "no"}';
      });
      return ok;
    }
    return true;
  }

  Future<void> _connect(ScanResult result) async {
    await FlutterBluePlus.stopScan();
    setState(() {
      _scanning = false;
      _status = 'Connecting...';
    });
    _device = result.device;
    _serviceUuid = null;
    _stateReadCharUuid = null;
    _stateCharUuid = null;
    _lastFrameMs = 0;
    await _device!
        .connect(timeout: const Duration(seconds: 10), autoConnect: false);
    try {
      await _device!.requestMtu(185);
    } catch (_) {
      // ignore
    }
    setState(() => _status = 'Discovering services...');
    await _discover();
  }

  Future<void> _discover() async {
    if (_device == null) return;
    final services = await _device!.discoverServices();
    BluetoothCharacteristic? chNotify;
    BluetoothCharacteristic? chRead;
    for (final s in services) {
      final suffix = _extractSuffixFromService(s.uuid);
      if (suffix == null) continue;
      _serviceUuid = s.uuid;
      _stateCharUuid = _uuidFor('03', suffix);
      _stateReadCharUuid = _uuidFor('04', suffix);
      for (final c in s.characteristics) {
        if (_stateCharUuid != null && c.uuid == _stateCharUuid) {
          chNotify = c;
        } else if (_stateReadCharUuid != null && c.uuid == _stateReadCharUuid) {
          chRead = c;
        }
      }
      if (chNotify != null || chRead != null) break;
    }
    if (chNotify == null && chRead == null) {
      setState(() => _status = 'State characteristic not found');
      return;
    }
    _stateChar = chNotify;
    _stateReadChar = chRead;

    if (_stateChar != null) {
      await _stateChar!.setNotifyValue(true);
    }
    _notifySub?.cancel();
    if (_stateChar != null) {
      _notifySub = _stateChar!.lastValueStream.listen((data) {
        _consumeNotifyFrame(data);
      });
    }

    // Initial read from dedicated READ characteristic (if available)
    if (_stateReadChar != null) {
      await _readStateChar();
    } else if (_stateChar != null) {
      final value = await _stateChar!.read();
      _consumeNotifyFrame(value);
    }

    _pollTimer?.cancel();
    _pollTimer = Timer.periodic(const Duration(seconds: 2), (_) async {
      if (_device == null) return;
      final now = DateTime.now().millisecondsSinceEpoch;
      // If notifications are not flowing, poll the read characteristic.
      if (now - _lastFrameMs > 3000) {
        await _readStateChar();
      }
    });

    if (_state.isEmpty) {
      setState(() => _status = 'Connected (no data yet)');
    }
  }

  void _consumeNotifyFrame(List<int> data) {
    if (data.isEmpty) return;
    _rxBuffer += utf8.decode(data, allowMalformed: true);
    if (_rxBuffer.length > 16384) {
      _rxBuffer = '';
      return;
    }
    final startIdx = _rxBuffer.indexOf('\u001E\u001E');
    if (startIdx < 0) return;
    final endIdx = _rxBuffer.indexOf('\u001F', startIdx + 2);
    if (endIdx < 0) return;
    final payload = _rxBuffer.substring(startIdx + 2, endIdx);
    final parsed = _tryParseJson(payload);
    if (parsed != null) {
      setState(() {
        _stateJson = payload;
        _stateMeta = 'notify len=${payload.length}';
        _state = parsed;
        _status = 'Connected';
      });
      _lastFrameMs = DateTime.now().millisecondsSinceEpoch;
    }
    _rxBuffer = _rxBuffer.substring(endIdx + 1);
  }

  Future<void> _readStateChar() async {
    if (_stateReadChar == null) return;
    try {
      final value = await _stateReadChar!.read();
      if (value.isEmpty) return;
      final txt = utf8.decode(value, allowMalformed: true);
      final parsed = _tryParseJson(txt);
      if (parsed != null) {
        setState(() {
          _stateJson = txt;
          _stateMeta = 'read len=${value.length}';
          _state = parsed;
          _status = 'Connected';
        });
        _lastFrameMs = DateTime.now().millisecondsSinceEpoch;
      }
    } catch (_) {
      // ignore read errors in fallback poll
    }
  }

  Map<String, dynamic>? _tryParseJson(String raw) {
    try {
      final obj = jsonDecode(raw);
      if (obj is Map<String, dynamic>) return obj;
    } catch (_) {}
    return null;
  }

  String _formatUptime(int ms) {
    final total = (ms / 1000).floor();
    final d = total ~/ 86400;
    final h = (total % 86400) ~/ 3600;
    final m = (total % 3600) ~/ 60;
    final s = total % 60;
    if (d > 0)
      return '${d}d ${h.toString().padLeft(2, '0')}:${m.toString().padLeft(2, '0')}:${s.toString().padLeft(2, '0')}';
    return '${h.toString().padLeft(2, '0')}:${m.toString().padLeft(2, '0')}:${s.toString().padLeft(2, '0')}';
  }

  int _toInt(dynamic v, [int fallback = 0]) {
    if (v is num) return v.toInt();
    if (v is String) return int.tryParse(v) ?? fallback;
    return fallback;
  }

  String _toStr(dynamic v, [String fallback = '--']) {
    if (v == null) return fallback;
    final s = v.toString();
    return s.isEmpty ? fallback : s;
  }

  Widget _chip(String label, {int level = 0}) {
    Color bg;
    Color fg;
    Color border;
    switch (level) {
      case 1: // OK
        bg = const Color(0xFFE8F8F0);
        fg = const Color(0xFF1B5E20);
        border = const Color(0xFF66BB6A);
        break;
      case 2: // Warning
        bg = const Color(0xFFFFF4E5);
        fg = const Color(0xFF8A6D00);
        border = const Color(0xFFFBC02D);
        break;
      case 3: // Error
        bg = const Color(0xFFFDECEC);
        fg = const Color(0xFF8E1A1A);
        border = const Color(0xFFE57373);
        break;
      case 4: // Info
        bg = const Color(0xFFEAF4FF);
        fg = const Color(0xFF0D47A1);
        border = const Color(0xFF64B5F6);
        break;
      default: // Neutral
        bg = const Color(0xFFF3F3F3);
        fg = Colors.black87;
        border = const Color(0xFFE0E0E0);
    }
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: bg,
        border: Border.all(color: border),
        borderRadius: BorderRadius.circular(16),
      ),
      child: Text(label,
          style:
              TextStyle(color: fg, fontSize: 12, fontWeight: FontWeight.w600)),
    );
  }

  Widget _statusCard(String title, List<Widget> chips) {
    return Card(
      elevation: 0,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: const BorderSide(color: Colors.black12),
      ),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(title, style: const TextStyle(fontWeight: FontWeight.bold)),
            const SizedBox(height: 8),
            Wrap(spacing: 8, runSpacing: 8, children: chips),
          ],
        ),
      ),
    );
  }

  Widget _led(bool on) {
    return Container(
      width: 12,
      height: 12,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: on ? const Color(0xFF2ECC71) : const Color(0xFFBDBDBD),
        border: Border.all(color: Colors.black12),
      ),
    );
  }

  Widget _buildStateView() {
    final fw = (_state['fw'] ?? '--').toString();
    final fwTag = (_state['fw_tag'] ?? '').toString();
    final uptimeMs = (_state['uptime_ms'] is num)
        ? (_state['uptime_ms'] as num).toInt()
        : null;
    final eth = _state['eth'] is Map ? _state['eth'] as Map : {};
    final ethIp = (eth['ip'] ?? _state['ip'] ?? '--').toString();
    final ethLinkKnown = eth.containsKey('link');
    final ethLink = ethLinkKnown ? ((eth['link'] ?? 0) == 1) : true;
    final mqtt = _state['mqtt'] is Map ? _state['mqtt'] as Map : {};
    final gsm = _state['gsm'] is Map ? _state['gsm'] as Map : {};
    final mqttConnected = _toInt(mqtt['connected']) == 1;
    final activeTransport = _toStr(mqtt['active_transport'], '--');
    final onGsm = _toInt(mqtt['on_gsm']) == 1;
    final gsmModemReady = _toInt(gsm['modem_ready'], _toInt(gsm['ready'])) == 1;
    final gsmReady = _toInt(
            gsm['ready'],
            (_toInt(gsm['network']) == 1 && _toInt(gsm['data']) == 1)
                ? 1
                : 0) ==
        1;
    final gsmNetwork = _toInt(gsm['network']) == 1;
    final gsmData = _toInt(gsm['data']) == 1;
    final gsmCsq = _toInt(gsm['csq'], -1);
    final gsmDbm = _toInt(gsm['dbm'], 0);
    final gsmOp = _toStr(gsm['op'], '-');
    final gsmApn = _toStr(gsm['apn'], '-');
    final gsmIp = _toStr(gsm['ip'], '-');
    final bleError = _toStr(_state['error'], '');
    final activeTransportLower = activeTransport.toLowerCase();
    final mqttOnGsmNow = onGsm || activeTransportLower == 'gsm';
    final signalLevel =
        gsmCsq < 0 ? 0 : (gsmCsq >= 20 ? 1 : (gsmCsq >= 10 ? 2 : 3));

    final inputs =
        (_state['inputs'] is List) ? List<int>.from(_state['inputs']) : <int>[];
    final relays =
        (_state['relays'] is List) ? List<int>.from(_state['relays']) : <int>[];
    final overrides = (_state['override'] is List)
        ? List<int>.from(_state['override'])
        : <int>[];
    final modules =
        (_state['modules'] is num) ? (_state['modules'] as num).toInt() : 1;
    final per = (_state['relays_per'] is num)
        ? (_state['relays_per'] as num).toInt()
        : 4;
    final inPer = (_state['inputs_per'] is num)
        ? (_state['inputs_per'] as num).toInt()
        : 4;

    final cards = <Widget>[];
    for (int m = 0; m < modules; m++) {
      final base = m * per;
      final rows = <Widget>[];
      for (int i = 0; i < per; i++) {
        final rIdx = base + i;
        final inIdx = m * inPer + i;
        final rVal = (rIdx < relays.length) ? relays[rIdx] == 1 : false;
        final inVal = (inIdx < inputs.length) ? inputs[inIdx] == 1 : false;
        final ov = (rIdx < overrides.length) ? overrides[rIdx] : -1;
        final mode = (ov == -1) ? 'AUTO' : (ov == 1 ? 'FORCE ON' : 'FORCE OFF');
        rows.add(
          Container(
            padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 8),
            decoration: BoxDecoration(
              border: Border.all(color: Colors.black12),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text('R${rIdx + 1}',
                        style: const TextStyle(fontWeight: FontWeight.bold)),
                    const SizedBox(height: 4),
                    Text(mode,
                        style: const TextStyle(
                            fontSize: 11, color: Colors.black54)),
                  ],
                ),
                Row(
                  children: [
                    Text(rVal ? 'ON' : 'OFF',
                        style: TextStyle(
                            color: rVal ? Colors.green : Colors.black54)),
                    const SizedBox(width: 12),
                    _led(inVal),
                    const SizedBox(width: 6),
                    Text('E${inIdx + 1}', style: const TextStyle(fontSize: 12)),
                  ],
                ),
              ],
            ),
          ),
        );
      }
      cards.add(
        Card(
          elevation: 0,
          shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(12),
              side: const BorderSide(color: Colors.black12)),
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Module ${m + 1}',
                    style: const TextStyle(fontWeight: FontWeight.bold)),
                const SizedBox(height: 8),
                ...rows.map((w) => Padding(
                    padding: const EdgeInsets.only(bottom: 8), child: w)),
              ],
            ),
          ),
        ),
      );
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _statusCard('Connectivite', [
          _chip('ETH ${ethLink ? "UP" : "DOWN"}', level: ethLink ? 1 : 3),
          _chip('ETH IP $ethIp', level: ethIp == '--' ? 2 : 4),
          _chip('MQTT ${mqttConnected ? "ON" : "OFF"}',
              level: mqttConnected ? 1 : 3),
          _chip('Transport ${activeTransport.toUpperCase()}',
              level: mqttOnGsmNow ? 4 : 0),
        ]),
        const SizedBox(height: 12),
        _statusCard('Systeme', [
          _chip('FW $fw${fwTag.isNotEmpty ? " ($fwTag)" : ""}', level: 4),
          if (uptimeMs != null)
            _chip('Uptime ${_formatUptime(uptimeMs)}', level: 0),
          if (bleError.isNotEmpty) _chip('BLE erreur $bleError', level: 3),
        ]),
        if (mqttOnGsmNow ||
            gsmModemReady ||
            gsmReady ||
            gsmNetwork ||
            gsmData) ...[
          const SizedBox(height: 12),
          _statusCard('GSM', [
            _chip('1NCE ${gsmReady ? "READY" : "OFF"}',
                level: gsmReady ? 1 : 3),
            _chip('Modem AT ${gsmModemReady ? "READY" : "OFF"}',
                level: gsmModemReady ? 1 : 3),
            _chip('Reseau ${gsmNetwork ? "ON" : "OFF"}',
                level: gsmNetwork ? 1 : 3),
            _chip('Data ${gsmData ? "ON" : "OFF"}', level: gsmData ? 1 : 3),
            _chip('Signal CSQ ${gsmCsq >= 0 ? gsmCsq : "--"}',
                level: signalLevel),
            _chip('Signal dBm ${gsmCsq >= 0 ? gsmDbm.toString() : "--"}',
                level: signalLevel),
            _chip('Operateur $gsmOp', level: 4),
            _chip('APN $gsmApn', level: 0),
            _chip('IP $gsmIp', level: 0),
          ]),
        ],
        const SizedBox(height: 12),
        ...cards.map((c) =>
            Padding(padding: const EdgeInsets.only(bottom: 12), child: c)),
      ],
    );
  }

  Future<void> _disconnect() async {
    try {
      await _notifySub?.cancel();
      _notifySub = null;
      _pollTimer?.cancel();
      _pollTimer = null;
      if (_device != null) {
        await _device!.disconnect();
      }
    } catch (_) {
      // ignore
    }
    setState(() {
      _device = null;
      _stateChar = null;
      _stateReadChar = null;
      _status = 'Disconnected';
    });
  }

  Widget _buildScanList() {
    if (_results.isEmpty) {
      return const Text('No ESPRelay4 found.');
    }
    return Column(
      children: _results.map((r) {
        final name = r.device.platformName.isNotEmpty
            ? r.device.platformName
            : 'Unknown';
        final rssi = r.rssi;
        return ListTile(
          title: Text(name),
          subtitle: Text('RSSI: $rssi'),
          trailing: ElevatedButton(
            onPressed: () => _connect(r),
            child: const Text('Connect'),
          ),
        );
      }).toList(),
    );
  }

  @override
  Widget build(BuildContext context) {
    final connected = _device != null;
    return Scaffold(
      appBar: AppBar(
        title: const Text('ESPRelay4 BLE'),
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: LayoutBuilder(
          builder: (context, constraints) {
            return SingleChildScrollView(
              child: ConstrainedBox(
                constraints: BoxConstraints(minHeight: constraints.maxHeight),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Text('Bluetooth: ${_adapterState.name}',
                        style: const TextStyle(fontWeight: FontWeight.bold)),
                    const SizedBox(height: 8),
                    Text('Status: $_status'),
                    const SizedBox(height: 16),
                    if (!connected) ...[
                      ElevatedButton(
                        onPressed: _scanning ? null : _startScan,
                        child: Text(
                            _scanning ? 'Scanning...' : 'Scan for ESPRelay4'),
                      ),
                      const SizedBox(height: 8),
                      OutlinedButton(
                        onPressed: () async {
                          await openAppSettings();
                        },
                        child: const Text('Open App Settings'),
                      ),
                      const SizedBox(height: 12),
                      _buildScanList(),
                    ] else ...[
                      Row(
                        children: [
                          Expanded(
                              child: Text(
                                  'Connected to ${_device!.platformName}')),
                          TextButton(
                              onPressed: _disconnect,
                              child: const Text('Disconnect')),
                        ],
                      ),
                      const SizedBox(height: 12),
                      _buildStateView(),
                      if (_stateMeta.isNotEmpty) ...[
                        const SizedBox(height: 8),
                        Text(_stateMeta,
                            style: const TextStyle(
                                fontSize: 12, color: Colors.black54)),
                      ],
                    ],
                  ],
                ),
              ),
            );
          },
        ),
      ),
    );
  }
}
