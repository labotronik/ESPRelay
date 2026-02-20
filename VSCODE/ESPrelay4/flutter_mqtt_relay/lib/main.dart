import 'dart:async';

import 'package:flutter/material.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  runApp(const RelayMqttApp());
}

class RelayMqttApp extends StatelessWidget {
  const RelayMqttApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ESPRELAY MQTT',
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF0F5CC0),
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: const Color(0xFFF7F7F7),
      ),
      home: const RelayDashboardPage(),
    );
  }
}

class RelayMqttSettings {
  RelayMqttSettings({
    required this.host,
    required this.port,
    required this.username,
    required this.password,
    required this.baseTopic,
    required this.clientId,
  });

  String host;
  int port;
  String username;
  String password;
  String baseTopic;
  String clientId;

  static const _kHost = 'mqtt_host';
  static const _kPort = 'mqtt_port';
  static const _kUser = 'mqtt_user';
  static const _kPass = 'mqtt_pass';
  static const _kBase = 'mqtt_base';
  static const _kClient = 'mqtt_client';

  static RelayMqttSettings defaults() {
    return RelayMqttSettings(
      host: '192.168.1.43',
      port: 1883,
      username: '',
      password: '',
      baseTopic: 'esprelay4',
      clientId: 'ESPRELAY_APP',
    );
  }

  static Future<RelayMqttSettings> load() async {
    final prefs = await SharedPreferences.getInstance();
    final d = RelayMqttSettings.defaults();
    return RelayMqttSettings(
      host: prefs.getString(_kHost) ?? d.host,
      port: prefs.getInt(_kPort) ?? d.port,
      username: prefs.getString(_kUser) ?? d.username,
      password: prefs.getString(_kPass) ?? d.password,
      baseTopic: prefs.getString(_kBase) ?? d.baseTopic,
      clientId: prefs.getString(_kClient) ?? d.clientId,
    );
  }

  Future<void> save() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kHost, host.trim());
    await prefs.setInt(_kPort, port);
    await prefs.setString(_kUser, username);
    await prefs.setString(_kPass, password);
    await prefs.setString(_kBase, baseTopic.trim());
    await prefs.setString(_kClient, clientId.trim());
  }
}

class RelayDashboardPage extends StatefulWidget {
  const RelayDashboardPage({super.key});

  @override
  State<RelayDashboardPage> createState() => _RelayDashboardPageState();
}

class _RelayDashboardPageState extends State<RelayDashboardPage>
    with SingleTickerProviderStateMixin {
  static const int _maxIoIndex = 4;
  static const int _maxIncomingPayloadBytes = 128;
  static const int _maxMessagesPerSecond = 10;
  static const Duration _uiFlushInterval = Duration(milliseconds: 120);

  late final TabController _tabController;

  RelayMqttSettings _settings = RelayMqttSettings.defaults();
  final _hostCtrl = TextEditingController();
  final _portCtrl = TextEditingController();
  final _userCtrl = TextEditingController();
  final _passCtrl = TextEditingController();
  final _baseCtrl = TextEditingController();
  final _clientCtrl = TextEditingController();

  MqttServerClient? _client;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage>>>? _mqttSub;
  Timer? _uiFlushTimer;
  bool _subscribed = false;
  int _msgWindowStartMs = 0;
  int _msgWindowCount = 0;

  bool _busy = false;
  bool _connected = false;
  bool _boardOnline = false;
  String _statusText = 'Disconnected';
  String _ethIp = '--';
  String _iccid = '--';

  final Map<int, bool> _relayStates = <int, bool>{};
  final Map<int, String> _relayModes = <int, String>{};
  final Map<int, bool> _inputStates = <int, bool>{};
  final Map<int, bool> _vinStates = <int, bool>{};
  final Map<int, bool> _pendingRelayStates = <int, bool>{};
  final Map<int, String> _pendingRelayModes = <int, String>{};
  final Map<int, bool> _pendingInputStates = <int, bool>{};
  final Map<int, bool> _pendingVinStates = <int, bool>{};

  bool _uiFlushPending = false;
  bool? _pendingBoardOnline;
  String? _pendingEthIp;
  String? _pendingIccid;

  @override
  void initState() {
    super.initState();
    _tabController = TabController(length: 2, vsync: this);
    _initSettings();
  }

  @override
  void dispose() {
    _mqttSub?.cancel();
    _uiFlushTimer?.cancel();
    _client?.disconnect();
    _tabController.dispose();
    _hostCtrl.dispose();
    _portCtrl.dispose();
    _userCtrl.dispose();
    _passCtrl.dispose();
    _baseCtrl.dispose();
    _clientCtrl.dispose();
    super.dispose();
  }

  Future<void> _initSettings() async {
    final loaded = await RelayMqttSettings.load();
    setState(() {
      _settings = loaded;
      _hostCtrl.text = loaded.host;
      _portCtrl.text = loaded.port.toString();
      _userCtrl.text = loaded.username;
      _passCtrl.text = loaded.password;
      _baseCtrl.text = loaded.baseTopic;
      _clientCtrl.text = loaded.clientId;
    });
  }

  Future<void> _connect() async {
    FocusScope.of(context).unfocus();

    final port = int.tryParse(_portCtrl.text.trim());
    if (port == null || port <= 0 || port > 65535) {
      _showSnack('Invalid MQTT port.');
      return;
    }

    final normalizedBase =
        _baseCtrl.text.trim().replaceAll(RegExp(r'^/+|/+$'), '');
    if (_hostCtrl.text.trim().isEmpty ||
        normalizedBase.isEmpty ||
        _clientCtrl.text.trim().isEmpty) {
      _showSnack('Host, Base topic and Client ID are required.');
      return;
    }

    setState(() {
      _busy = true;
      _statusText = 'Connecting...';
    });

    await _disconnect(silent: true);

    _settings
      ..host = _hostCtrl.text.trim()
      ..port = port
      ..username = _userCtrl.text
      ..password = _passCtrl.text
      ..baseTopic = normalizedBase
      ..clientId = _clientCtrl.text.trim();

    await _settings.save();

    final client = MqttServerClient.withPort(
        _settings.host, _settings.clientId, _settings.port);
    client.logging(on: false);
    client.keepAlivePeriod = 45;
    client.autoReconnect = true;
    client.resubscribeOnAutoReconnect = false;
    client.onConnected = () {
      _subscribed = false;
      _ensureSubscriptions();
      setState(() {
        _connected = true;
        _statusText = 'Connected';
      });
    };
    client.onDisconnected = () {
      _subscribed = false;
      if (!mounted) return;
      setState(() {
        _connected = false;
        _statusText = 'Disconnected';
      });
    };
    client.onAutoReconnect = () {
      if (!mounted) return;
      setState(() => _statusText = 'Reconnecting...');
    };
    client.onAutoReconnected = () {
      if (!mounted) return;
      _subscribed = false;
      _ensureSubscriptions();
      setState(() => _statusText = 'Connected (auto)');
    };

    final connMessage = MqttConnectMessage()
        .withClientIdentifier(_settings.clientId)
        .startClean()
        .withWillQos(MqttQos.atMostOnce)
        .withWillTopic('${_settings.baseTopic}/status')
        .withWillMessage('offline');
    client.connectionMessage = connMessage;

    try {
      final result = await client.connect(
        _settings.username.isEmpty ? null : _settings.username,
        _settings.username.isEmpty ? null : _settings.password,
      );
      if (!mounted) return;

      if (result?.state == MqttConnectionState.connected) {
        _client = client;
        _subscribed = false;
        _ensureSubscriptions();
        _listenUpdates(client);
        setState(() {
          _connected = true;
          _statusText = 'Connected';
          _busy = false;
        });
        _showSnack('MQTT connected.');
      } else {
        client.disconnect();
        setState(() {
          _busy = false;
          _connected = false;
          _statusText = 'Connect failed (${result?.state.name ?? 'unknown'})';
        });
      }
    } catch (e) {
      client.disconnect();
      if (!mounted) return;
      setState(() {
        _busy = false;
        _connected = false;
        _statusText = 'Error: $e';
      });
    }
  }

  void _subscribeBase(MqttServerClient client) {
    final b = _settings.baseTopic;
    client.subscribe('$b/status', MqttQos.atLeastOnce);
    client.subscribe('$b/net/ip', MqttQos.atLeastOnce);
    client.subscribe('$b/gsm/iccid', MqttQos.atLeastOnce);
    for (int i = 1; i <= _maxIoIndex; i++) {
      client.subscribe('$b/relay/$i/state', MqttQos.atLeastOnce);
      client.subscribe('$b/relay/$i/mode', MqttQos.atLeastOnce);
      client.subscribe('$b/input/$i/state', MqttQos.atLeastOnce);
      client.subscribe('$b/vin/$i/state', MqttQos.atLeastOnce);
    }
  }

  void _ensureSubscriptions() {
    final client = _client;
    if (client == null || _subscribed != false) return;
    _subscribeBase(client);
    _subscribed = true;
  }

  void _listenUpdates(MqttServerClient client) {
    _mqttSub?.cancel();
    _mqttSub = client.updates?.listen((events) {
      final nowMs = DateTime.now().millisecondsSinceEpoch;
      if (nowMs - _msgWindowStartMs >= 1000) {
        _msgWindowStartMs = nowMs;
        _msgWindowCount = 0;
      }
      for (final event in events) {
        _msgWindowCount++;
        if (_msgWindowCount > _maxMessagesPerSecond) {
          continue;
        }
        final rec = event.payload as MqttPublishMessage;
        final fullTopic = event.topic;
        final subTopic = _extractAllowedSubTopic(fullTopic);
        if (subTopic == null) continue;
        if (rec.payload.message.length > _maxIncomingPayloadBytes) {
          continue;
        }
        final payload =
            MqttPublishPayload.bytesToStringAsString(rec.payload.message);
        _handleMessage(subTopic: subTopic, payload: payload);
      }
    });
  }

  Future<void> _disconnect({bool silent = false}) async {
    await _mqttSub?.cancel();
    _mqttSub = null;
    _client?.disconnect();
    _client = null;
    _subscribed = false;
    if (!mounted) return;
    setState(() {
      _connected = false;
      _busy = false;
      _boardOnline = false;
      _statusText = 'Disconnected';
    });
    if (!silent) _showSnack('MQTT disconnected.');
  }

  String? _extractAllowedSubTopic(String fullTopic) {
    final prefix = '${_settings.baseTopic}/';
    if (!fullTopic.startsWith(prefix)) return null;
    final subTopic = fullTopic.substring(prefix.length);
    if (subTopic == 'status' ||
        subTopic == 'net/ip' ||
        subTopic == 'gsm/iccid') {
      return subTopic;
    }
    if (subTopic.startsWith('relay/') && subTopic.endsWith('/state')) {
      return subTopic;
    }
    if (subTopic.startsWith('relay/') && subTopic.endsWith('/mode')) {
      return subTopic;
    }
    if (subTopic.startsWith('input/') && subTopic.endsWith('/state')) {
      return subTopic;
    }
    if (subTopic.startsWith('vin/') && subTopic.endsWith('/state')) {
      return subTopic;
    }
    return null;
  }

  int _extractIoIndex(String subTopic, String root) {
    final prefix = '$root/';
    if (!subTopic.startsWith(prefix)) return -1;
    bool okSuffix = false;
    int suffixLen = 0;
    if (subTopic.endsWith('/state')) {
      okSuffix = true;
      suffixLen = '/state'.length;
    } else if (subTopic.endsWith('/mode')) {
      okSuffix = true;
      suffixLen = '/mode'.length;
    }
    if (!okSuffix) return -1;
    final start = prefix.length;
    final end = subTopic.length - suffixLen;
    if (end <= start) return -1;
    final idx = int.tryParse(subTopic.substring(start, end)) ?? -1;
    if (idx < 1 || idx > _maxIoIndex) return -1;
    return idx;
  }

  bool? _parseOnOffPayload(String s) {
    if (s.isEmpty) return null;
    if (s.length == 1) {
      if (s == '1') return true;
      if (s == '0') return false;
      return null;
    }
    final lower = s.toLowerCase();
    if (lower == 'on' || lower == 'online') return true;
    if (lower == 'off' || lower == 'offline') return false;
    return null;
  }

  String? _parseRelayModePayload(String s) {
    if (s.isEmpty) return null;
    final up = s.toUpperCase();
    if (up == 'AUTO') return 'AUTO';
    if (up == 'FORCE_ON' || up == 'ON') return 'FORCE_ON';
    if (up == 'FORCE_OFF' || up == 'OFF') return 'FORCE_OFF';
    return null;
  }

  void _handleMessage({
    required String subTopic,
    required String payload,
  }) {
    final normalized = payload.trim();
    final boolValue = _parseOnOffPayload(normalized);
    bool changed = false;

    if (subTopic == 'status') {
      if (boolValue != null) {
        final current = _pendingBoardOnline ?? _boardOnline;
        if (current != boolValue) {
          _pendingBoardOnline = boolValue;
          changed = true;
        }
      }
    } else if (subTopic == 'net/ip') {
      final nextIp = normalized.isEmpty ? '--' : normalized;
      final current = _pendingEthIp ?? _ethIp;
      if (current != nextIp) {
        _pendingEthIp = nextIp;
        changed = true;
      }
    } else if (subTopic == 'gsm/iccid') {
      final nextIccid = normalized.isEmpty ? '--' : normalized;
      final current = _pendingIccid ?? _iccid;
      if (current != nextIccid) {
        _pendingIccid = nextIccid;
        changed = true;
      }
    } else if (subTopic.startsWith('relay/')) {
      final idx = _extractIoIndex(subTopic, 'relay');
      if (idx > 0) {
        if (subTopic.endsWith('/state') && boolValue != null) {
          final current = _pendingRelayStates[idx] ?? _relayStates[idx];
          if (current != boolValue) {
            _pendingRelayStates[idx] = boolValue;
            changed = true;
          }
        } else if (subTopic.endsWith('/mode')) {
          final mode = _parseRelayModePayload(normalized);
          if (mode != null) {
            final current = _pendingRelayModes[idx] ?? _relayModes[idx];
            if (current != mode) {
              _pendingRelayModes[idx] = mode;
              changed = true;
            }
          }
        }
      }
    } else if (subTopic.startsWith('input/')) {
      final idx = _extractIoIndex(subTopic, 'input');
      if (idx > 0 && boolValue != null) {
        final current = _pendingInputStates[idx] ?? _inputStates[idx];
        if (current != boolValue) {
          _pendingInputStates[idx] = boolValue;
          changed = true;
        }
      }
    } else if (subTopic.startsWith('vin/')) {
      final idx = _extractIoIndex(subTopic, 'vin');
      if (idx > 0 && boolValue != null) {
        final current = _pendingVinStates[idx] ?? _vinStates[idx];
        if (current != boolValue) {
          _pendingVinStates[idx] = boolValue;
          changed = true;
        }
      }
    }
    if (changed) _scheduleUiFlush();
  }

  void _scheduleUiFlush() {
    if (_uiFlushPending) return;
    _uiFlushPending = true;
    _uiFlushTimer?.cancel();
    _uiFlushTimer = Timer(_uiFlushInterval, _flushUiUpdates);
  }

  void _flushUiUpdates() {
    _uiFlushPending = false;
    if (!mounted) return;

    final bool hasChanges = _pendingBoardOnline != null ||
        _pendingEthIp != null ||
        _pendingIccid != null ||
        _pendingRelayStates.isNotEmpty ||
        _pendingRelayModes.isNotEmpty ||
        _pendingInputStates.isNotEmpty ||
        _pendingVinStates.isNotEmpty;
    if (!hasChanges) return;

    setState(() {
      if (_pendingBoardOnline != null) {
        _boardOnline = _pendingBoardOnline!;
        _pendingBoardOnline = null;
      }
      if (_pendingEthIp != null) {
        _ethIp = _pendingEthIp!;
        _pendingEthIp = null;
      }
      if (_pendingIccid != null) {
        _iccid = _pendingIccid!;
        _pendingIccid = null;
      }
      if (_pendingRelayStates.isNotEmpty) {
        _relayStates.addAll(_pendingRelayStates);
        _pendingRelayStates.clear();
      }
      if (_pendingRelayModes.isNotEmpty) {
        _relayModes.addAll(_pendingRelayModes);
        _pendingRelayModes.clear();
      }
      if (_pendingInputStates.isNotEmpty) {
        _inputStates.addAll(_pendingInputStates);
        _pendingInputStates.clear();
      }
      if (_pendingVinStates.isNotEmpty) {
        _vinStates.addAll(_pendingVinStates);
        _pendingVinStates.clear();
      }
    });
  }

  Future<void> _publishRelaySet(int relayId, String action) async {
    final client = _client;
    if (client == null || !_connected) {
      _showSnack('Not connected to MQTT.');
      return;
    }

    final topic = '${_settings.baseTopic}/relay/$relayId/set';
    final builder = MqttClientPayloadBuilder();
    builder.addString(action);

    final ok =
        client.publishMessage(topic, MqttQos.atLeastOnce, builder.payload!) > 0;
    if (!ok) {
      _showSnack('Publish failed for relay $relayId.');
    }
  }

  void _showSnack(String text) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text), duration: const Duration(seconds: 2)),
    );
  }

  Widget _pill(String text,
      {required Color bg, required Color border, required Color fg}) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: bg,
        border: Border.all(color: border),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(text,
          style:
              TextStyle(color: fg, fontWeight: FontWeight.w600, fontSize: 12)),
    );
  }

  Widget _statusPills() {
    final mqttChip = _connected
        ? _pill('MQTT: ON',
            bg: const Color(0xFFE8F8F0),
            border: const Color(0xFF66BB6A),
            fg: const Color(0xFF1B5E20))
        : _pill('MQTT: OFF',
            bg: const Color(0xFFFDECEC),
            border: const Color(0xFFE57373),
            fg: const Color(0xFF8E1A1A));

    final boardChip = _boardOnline
        ? _pill('BOARD: ONLINE',
            bg: const Color(0xFFE8F8F0),
            border: const Color(0xFF66BB6A),
            fg: const Color(0xFF1B5E20))
        : _pill('BOARD: OFFLINE',
            bg: const Color(0xFFFFF4E5),
            border: const Color(0xFFFBC02D),
            fg: const Color(0xFF8A6D00));

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            _pill('IP: $_ethIp',
                bg: Colors.white,
                border: const Color(0xFFDADADA),
                fg: Colors.black87),
            _pill('ICCID: $_iccid',
                bg: Colors.white,
                border: const Color(0xFFDADADA),
                fg: Colors.black87),
          ],
        ),
        const SizedBox(height: 8),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            boardChip,
            mqttChip,
          ],
        ),
      ],
    );
  }

  Widget _relayRow(int relayId, int inputId) {
    final relayOn = _relayStates[relayId] ?? false;
    final relayMode = _relayModes[relayId] ?? '--';
    final inputOn = _inputStates[inputId] ?? false;
    final vinOn = _vinStates[inputId] ?? false;

    final ledColor =
        inputOn ? const Color(0xFF27C046) : const Color(0xFFE0E0E0);

    final relayStateChip = relayOn
        ? _pill('ON',
            bg: const Color(0xFFE8F8F0),
            border: const Color(0xFF66BB6A),
            fg: const Color(0xFF1B5E20))
        : _pill('OFF',
            bg: const Color(0xFFF3F3F3),
            border: const Color(0xFFDADADA),
            fg: Colors.black87);

    return Container(
      margin: const EdgeInsets.only(bottom: 8),
      padding: const EdgeInsets.all(8),
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: const Color(0xFFDADADA)),
      ),
      child: Column(
        children: [
          Row(
            children: [
              Text('R$relayId',
                  style: const TextStyle(fontWeight: FontWeight.w700)),
              const SizedBox(width: 8),
              relayStateChip,
              const SizedBox(width: 8),
              Text(relayMode,
                  style: const TextStyle(fontSize: 11, color: Colors.black54)),
              const Spacer(),
              Container(
                width: 14,
                height: 14,
                decoration: BoxDecoration(
                  color: ledColor,
                  shape: BoxShape.circle,
                  border: Border.all(color: const Color(0xFFBDBDBD)),
                ),
              ),
              const SizedBox(width: 6),
              Text('E$inputId',
                  style: const TextStyle(fontWeight: FontWeight.w600)),
              const SizedBox(width: 6),
              Text(vinOn ? 'VIN ON' : 'VIN OFF',
                  style: const TextStyle(fontSize: 12, color: Colors.black54)),
            ],
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(
                child: FilledButton.tonal(
                  onPressed:
                      _connected ? () => _publishRelaySet(relayId, 'ON') : null,
                  child: const Text('ON'),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: FilledButton.tonal(
                  onPressed: _connected
                      ? () => _publishRelaySet(relayId, 'OFF')
                      : null,
                  child: const Text('OFF'),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: OutlinedButton(
                  onPressed: _connected
                      ? () => _publishRelaySet(relayId, 'AUTO')
                      : null,
                  child: const Text('AUTO'),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _moduleCard(int moduleIndex) {
    final baseRelay = moduleIndex * 4;
    final baseInput = moduleIndex * 4;

    return Container(
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: const Color(0xFFDADADA)),
      ),
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('Module ${moduleIndex + 1}',
              style:
                  const TextStyle(fontSize: 16, fontWeight: FontWeight.w700)),
          const SizedBox(height: 8),
          _relayRow(baseRelay + 1, baseInput + 1),
          _relayRow(baseRelay + 2, baseInput + 2),
          _relayRow(baseRelay + 3, baseInput + 3),
          _relayRow(baseRelay + 4, baseInput + 4),
        ],
      ),
    );
  }

  Widget _dashboardTab() {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        _statusPills(),
        const SizedBox(height: 12),
        Container(
          decoration: BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.circular(14),
            border: Border.all(color: const Color(0xFFDADADA)),
          ),
          padding: const EdgeInsets.all(12),
          child: Row(
            children: [
              Expanded(
                child: Text(
                  _statusText,
                  style: TextStyle(
                    fontWeight: FontWeight.w700,
                    color: _connected
                        ? const Color(0xFF1B5E20)
                        : const Color(0xFF8E1A1A),
                  ),
                ),
              ),
              const SizedBox(width: 8),
              FilledButton(
                onPressed: (_busy || _connected) ? null : _connect,
                child: const Text('Connect'),
              ),
              const SizedBox(width: 8),
              OutlinedButton(
                onPressed: (!_busy && _connected) ? () => _disconnect() : null,
                child: const Text('Disconnect'),
              ),
            ],
          ),
        ),
        const SizedBox(height: 12),
        _moduleCard(0),
      ],
    );
  }

  Widget _field(String label, TextEditingController ctrl,
      {bool obscure = false, TextInputType? keyboardType}) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: const TextStyle(fontWeight: FontWeight.w600)),
          const SizedBox(height: 4),
          TextField(
            controller: ctrl,
            obscureText: obscure,
            keyboardType: keyboardType,
            decoration: InputDecoration(
              isDense: true,
              filled: true,
              fillColor: Colors.white,
              border: OutlineInputBorder(
                borderRadius: BorderRadius.circular(10),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _setupTab() {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Container(
          decoration: BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.circular(14),
            border: Border.all(color: const Color(0xFFDADADA)),
          ),
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text('MQTT configuration',
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.w700)),
              const SizedBox(height: 10),
              _field('Broker host', _hostCtrl),
              _field('Broker port', _portCtrl,
                  keyboardType: TextInputType.number),
              _field('Username', _userCtrl),
              _field('Password', _passCtrl, obscure: true),
              _field('Base topic', _baseCtrl),
              _field('Client ID', _clientCtrl),
              Row(
                children: [
                  FilledButton(
                    onPressed: _busy ? null : _connect,
                    child: const Text('Save + Connect'),
                  ),
                  const SizedBox(width: 8),
                  OutlinedButton(
                    onPressed:
                        (_busy || !_connected) ? null : () => _disconnect(),
                    child: const Text('Disconnect'),
                  ),
                ],
              ),
            ],
          ),
        ),
        const SizedBox(height: 12),
        Container(
          decoration: BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.circular(14),
            border: Border.all(color: const Color(0xFFDADADA)),
          ),
          padding: const EdgeInsets.all(12),
          child: const Text(
            'Relay command topics:\n'
            '  <base>/relay/1/set payload ON|OFF|AUTO\n'
            'State topics:\n'
            '  <base>/relay/1/state\n'
            '  <base>/input/1/state\n'
            '  <base>/vin/1/state\n'
            '  <base>/status\n'
            '  <base>/net/ip',
            style: TextStyle(fontFamily: 'monospace', fontSize: 12),
          ),
        ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('ESPRELAY MQTT'),
        bottom: TabBar(
          controller: _tabController,
          tabs: const [
            Tab(text: 'Dashboard'),
            Tab(text: 'Setup'),
          ],
        ),
      ),
      body: TabBarView(
        controller: _tabController,
        children: [
          _dashboardTab(),
          _setupTab(),
        ],
      ),
    );
  }
}
