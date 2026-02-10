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

  BluetoothDevice? _device;
  BluetoothCharacteristic? _stateChar;
  Guid? _serviceUuid;
  Guid? _stateCharUuid;
  String _stateJson = '--';
  String _stateMeta = '';
  String _status = 'Idle';
  Map<String, dynamic> _state = {};
  String _rxBuffer = '';
  bool _scanning = false;
  BluetoothAdapterState _adapterState = BluetoothAdapterState.unknown;
  String _permStatus = '';

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
        final name = r.device.platformName.isNotEmpty ? r.device.platformName : advName;
        final isTarget = name.contains(kTargetName);
        final idx = _results.indexWhere((e) => e.device.remoteId == r.device.remoteId);
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
    _stateCharUuid = null;
    await _device!.connect(timeout: const Duration(seconds: 10), autoConnect: false);
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
    BluetoothCharacteristic? ch;
    for (final s in services) {
      final suffix = _extractSuffixFromService(s.uuid);
      if (suffix == null) continue;
      _serviceUuid = s.uuid;
      _stateCharUuid = _uuidFor('03', suffix);
      for (final c in s.characteristics) {
        if (_stateCharUuid != null && c.uuid == _stateCharUuid) {
          ch = c;
          break;
        }
      }
      if (ch != null) break;
    }
    if (ch == null) {
      setState(() => _status = 'State characteristic not found');
      return;
    }
    _stateChar = ch;
    await _stateChar!.setNotifyValue(true);
    _notifySub?.cancel();
    _notifySub = _stateChar!.lastValueStream.listen((data) {
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
          _stateMeta = 'len=${payload.length}';
          _state = parsed;
          _status = 'Connected';
        });
      }
      _rxBuffer = _rxBuffer.substring(endIdx + 1);
    });
    final value = await _stateChar!.read();
    if (value.isNotEmpty) {
      final txt = utf8.decode(value, allowMalformed: true);
      final parsed = _tryParseJson(txt);
      if (parsed != null) {
        setState(() {
          _stateJson = txt;
          _stateMeta = 'len=${value.length}';
          _state = parsed;
          _status = 'Connected';
        });
      }
    } else {
      setState(() => _status = 'Connected (no data yet)');
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
    if (d > 0) return '${d}d ${h.toString().padLeft(2, '0')}:${m.toString().padLeft(2, '0')}:${s.toString().padLeft(2, '0')}';
    return '${h.toString().padLeft(2, '0')}:${m.toString().padLeft(2, '0')}:${s.toString().padLeft(2, '0')}';
  }

  Widget _chip(String label, {Color? bg, Color? fg}) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: bg ?? const Color(0xFFF0F0F0),
        borderRadius: BorderRadius.circular(16),
      ),
      child: Text(label, style: TextStyle(color: fg ?? Colors.black87, fontSize: 12)),
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
    final uptimeMs = (_state['uptime_ms'] is num) ? (_state['uptime_ms'] as num).toInt() : null;
    final eth = _state['eth'] is Map ? _state['eth'] as Map : {};
    final ethIp = (eth['ip'] ?? _state['ip'] ?? '--').toString();
    final ethLinkKnown = eth.containsKey('link');
    final ethLink = ethLinkKnown ? ((eth['link'] ?? 0) == 1) : true;

    final inputs = (_state['inputs'] is List) ? List<int>.from(_state['inputs']) : <int>[];
    final relays = (_state['relays'] is List) ? List<int>.from(_state['relays']) : <int>[];
    final overrides = (_state['override'] is List) ? List<int>.from(_state['override']) : <int>[];
    final modules = (_state['modules'] is num) ? (_state['modules'] as num).toInt() : 1;
    final per = (_state['relays_per'] is num) ? (_state['relays_per'] as num).toInt() : 4;
    final inPer = (_state['inputs_per'] is num) ? (_state['inputs_per'] as num).toInt() : 4;

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
                    Text('R${rIdx + 1}', style: const TextStyle(fontWeight: FontWeight.bold)),
                    const SizedBox(height: 4),
                    Text(mode, style: const TextStyle(fontSize: 11, color: Colors.black54)),
                  ],
                ),
                Row(
                  children: [
                    Text(rVal ? 'ON' : 'OFF', style: TextStyle(color: rVal ? Colors.green : Colors.black54)),
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
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12), side: const BorderSide(color: Colors.black12)),
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Module ${m + 1}', style: const TextStyle(fontWeight: FontWeight.bold)),
                const SizedBox(height: 8),
                ...rows.map((w) => Padding(padding: const EdgeInsets.only(bottom: 8), child: w)),
              ],
            ),
          ),
        ),
      );
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            _chip('ETH: ${ethIp == '--' ? '--' : ethIp}${ethLink ? '' : ' (off)'}'),
            _chip('FW: $fw${fwTag.isNotEmpty ? " ($fwTag)" : ""}'),
            if (uptimeMs != null) _chip('Uptime: ${_formatUptime(uptimeMs)}'),
          ],
        ),
        const SizedBox(height: 12),
        ...cards.map((c) => Padding(padding: const EdgeInsets.only(bottom: 12), child: c)),
      ],
    );
  }

  Future<void> _disconnect() async {
    try {
      await _notifySub?.cancel();
      _notifySub = null;
      if (_device != null) {
        await _device!.disconnect();
      }
    } catch (_) {
      // ignore
    }
    setState(() {
      _device = null;
      _stateChar = null;
      _status = 'Disconnected';
    });
  }

  Widget _buildScanList() {
    if (_results.isEmpty) {
      return const Text('No ESPRelay4 found.');
    }
    return Column(
      children: _results.map((r) {
        final name = r.device.platformName.isNotEmpty ? r.device.platformName : 'Unknown';
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
            Text('Bluetooth: ${_adapterState.name}', style: const TextStyle(fontWeight: FontWeight.bold)),
            const SizedBox(height: 8),
            Text('Status: $_status'),
            if (_permStatus.isNotEmpty) ...[
              const SizedBox(height: 4),
              Text('Perms: $_permStatus', style: const TextStyle(fontSize: 12, color: Colors.black54)),
            ],
            const SizedBox(height: 16),
            if (!connected) ...[
              ElevatedButton(
                onPressed: _scanning ? null : _startScan,
                child: Text(_scanning ? 'Scanning...' : 'Scan for ESPRelay4'),
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
                  Expanded(child: Text('Connected to ${_device!.platformName}')),
                  TextButton(onPressed: _disconnect, child: const Text('Disconnect')),
                ],
              ),
              const SizedBox(height: 12),
              _buildStateView(),
              if (_stateMeta.isNotEmpty) ...[
                const SizedBox(height: 8),
                Text(_stateMeta, style: const TextStyle(fontSize: 12, color: Colors.black54)),
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
