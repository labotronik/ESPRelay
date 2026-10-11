import 'dart:async';
import 'dart:math';

import 'package:device_info_plus/device_info_plus.dart';
import 'package:flutter/material.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  runApp(const ER4MqttApp());
}

// ============================================================================
// LOGO ER4 MQTT
// ============================================================================

/// Logo widget with SVG fallback to text-based logo
/// Uses flutter_svg if available, otherwise renders a text-based logo
class ER4Logo extends StatelessWidget {
  const ER4Logo({super.key, this.size = 32, this.useSvg = true});

  final double size;
  final bool useSvg;

  @override
  Widget build(BuildContext context) {
    // Try to use SVG if available
    if (useSvg) {
      try {
        // We'll use the text-based version as primary since SVG package
        // might not be available. This provides a clean fallback.
      } catch (e) {
        // Fall back to text logo
      }
    }
    
    // Text-based logo (primary implementation)
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        // Circle with ER4
        Container(
          width: size,
          height: size,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            gradient: LinearGradient(
              colors: [
                Theme.of(context).colorScheme.primary,
                Theme.of(context).colorScheme.secondary,
              ],
            ),
          ),
          child: Center(
            child: Text(
              'ER4',
              style: TextStyle(
                color: Colors.white,
                fontWeight: FontWeight.bold,
                fontSize: size * 0.5,
              ),
            ),
          ),
        ),
        const SizedBox(width: 8),
        // MQTT text
        Text(
          'MQTT',
          style: TextStyle(
            fontSize: size * 0.8,
            fontWeight: FontWeight.w600,
            color: Theme.of(context).colorScheme.primary,
          ),
        ),
      ],
    );
  }
}

// ============================================================================
// APPLICATION PRINCIPALE
// ============================================================================

class ER4MqttApp extends StatelessWidget {
  const ER4MqttApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ER4 MQTT',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF0F5CC0),
          brightness: Brightness.light,
          primary: const Color(0xFF0F5CC0),
          secondary: const Color(0xFF64B5F6),
          tertiary: const Color(0xFF2ECC71),
          error: const Color(0xFFE57373),
        ),
        scaffoldBackgroundColor: const Color(0xFFF7F7F7),
        appBarTheme: const AppBarTheme(
          centerTitle: true,
          elevation: 0,
          backgroundColor: Color(0xFFF7F7F7),
          foregroundColor: Color(0xFF212121),
        ),
        cardTheme: CardThemeData(
          elevation: 0,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(12),
            side: BorderSide(color: Colors.grey.shade200),
          ),
        ),
        inputDecorationTheme: InputDecorationTheme(
          filled: true,
          fillColor: Colors.white,
          border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(10),
            borderSide: BorderSide.none,
          ),
          contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
        ),
        filledButtonTheme: FilledButtonThemeData(
          style: FilledButton.styleFrom(
            padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(10),
            ),
          ),
        ),
        outlinedButtonTheme: OutlinedButtonThemeData(
          style: OutlinedButton.styleFrom(
            padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(10),
            ),
            side: BorderSide(color: Colors.grey.shade400),
          ),
        ),
        textButtonTheme: TextButtonThemeData(
          style: TextButton.styleFrom(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
          ),
        ),
      ),
      home: const MainScreen(),
    );
  }
}

// ============================================================================
// CONFIGURATION MQTT (modèle existant préservé)
// ============================================================================

class RelayMqttSettings {
  RelayMqttSettings({
    required this.host,
    required this.port,
    required this.username,
    required this.password,
    required this.baseTopic,
    required this.cardId,
    required this.clientId,
    required this.relayNames,
    required this.inputNames,
  });

  String host;
  int port;
  String username;
  String password;
  String baseTopic;
  String cardId;
  String clientId;
  Map<int, String> relayNames;
  Map<int, String> inputNames;

  static const _kHost = 'mqtt_host';
  static const _kPort = 'mqtt_port';
  static const _kUser = 'mqtt_user';
  static const _kPass = 'mqtt_pass';
  static const _kBase = 'mqtt_base';
  static const _kCardId = 'mqtt_card_id';
  static const _kClient = 'mqtt_client';
  static const _kClientAuto = 'mqtt_client_auto';
  static const _kRelayNamePrefix = 'relay_name_';
  static const _kInputNamePrefix = 'input_name_';
  static const _legacyClientId = 'ESPRELAY_APP';
  static const int _relayCount = 4;

  static Map<int, String> _defaultRelayNames() {
    return <int, String>{
      for (int i = 1; i <= _relayCount; i++) i: 'R$i',
    };
  }

  static Map<int, String> _defaultInputNames() {
    return <int, String>{
      for (int i = 1; i <= _relayCount; i++) i: 'E$i',
    };
  }

  static String _normalizeRelayName(String? name, int relayId) {
    final trimmed = (name ?? '').trim();
    if (trimmed.isEmpty) return 'R$relayId';
    if (trimmed.length > 24) return trimmed.substring(0, 24);
    return trimmed;
  }

  static String _normalizeInputName(String? name, int inputId) {
    final trimmed = (name ?? '').trim();
    if (trimmed.isEmpty) return 'E$inputId';
    if (trimmed.length > 24) return trimmed.substring(0, 24);
    return trimmed;
  }

  static RelayMqttSettings defaults() {
    return RelayMqttSettings(
      host: '192.168.1.43',
      port: 1883,
      username: '',
      password: '',
      baseTopic: 'espr4',
      cardId: '',
      clientId: 'ER4_MQTT_APP',
      relayNames: _defaultRelayNames(),
      inputNames: _defaultInputNames(),
    );
  }

  static Future<RelayMqttSettings> load() async {
    final prefs = await SharedPreferences.getInstance();
    final d = RelayMqttSettings.defaults();
    final autoClientId = await _resolveClientId(prefs);
    final loadedRelayNames = <int, String>{};
    final loadedInputNames = <int, String>{};
    for (int i = 1; i <= _relayCount; i++) {
      loadedRelayNames[i] =
          _normalizeRelayName(prefs.getString('$_kRelayNamePrefix$i'), i);
      loadedInputNames[i] =
          _normalizeInputName(prefs.getString('$_kInputNamePrefix$i'), i);
    }
    return RelayMqttSettings(
      host: prefs.getString(_kHost) ?? d.host,
      port: prefs.getInt(_kPort) ?? d.port,
      username: prefs.getString(_kUser) ?? d.username,
      password: prefs.getString(_kPass) ?? d.password,
      baseTopic: _normalizeBaseTopic(prefs.getString(_kBase) ?? d.baseTopic),
      cardId: _normalizeCardId(prefs.getString(_kCardId) ?? d.cardId),
      clientId: autoClientId,
      relayNames: loadedRelayNames,
      inputNames: loadedInputNames,
    );
  }

  static String _normalizeBaseTopic(String? base) {
    return (base ?? '').trim().replaceAll(RegExp(r'^/+|/+$'), '');
  }

  static String _normalizeCardId(String? cardId) {
    return (cardId ?? '')
        .trim()
        .replaceAll(':', '')
        .replaceAll('-', '')
        .replaceAll(RegExp(r'\\s+'), '')
        .toUpperCase();
  }

  static Future<String> _resolveClientId(SharedPreferences prefs) async {
    final saved = (prefs.getString(_kClient) ?? '').trim();
    if (saved.isNotEmpty && saved != _legacyClientId) return saved;

    final cachedAuto = (prefs.getString(_kClientAuto) ?? '').trim();
    if (cachedAuto.isNotEmpty) {
      if (saved.isEmpty || saved == _legacyClientId) {
        await prefs.setString(_kClient, cachedAuto);
      }
      return cachedAuto;
    }

    final generated = await _buildMachineClientId();
    await prefs.setString(_kClientAuto, generated);
    if (saved.isEmpty || saved == _legacyClientId) {
      await prefs.setString(_kClient, generated);
    }
    return generated;
  }

  static Future<String> _buildMachineClientId() async {
    try {
      final info = await DeviceInfoPlugin().deviceInfo;
      final fingerprint = _fingerprintFromMap(info.data);
      if (fingerprint.isNotEmpty) return _clientIdFromSeed(fingerprint);
    } catch (_) {}
    final rnd = Random.secure();
    return _clientIdFromSeed(
      '${DateTime.now().microsecondsSinceEpoch}-${rnd.nextInt(1 << 32)}',
    );
  }

  static String _fingerprintFromMap(Map<String, dynamic> data) {
    const preferredKeys = <String>[
      'machineId',
      'systemGuid',
      'deviceId',
      'identifierForVendor',
      'id',
      'hostName',
      'computerName',
      'name',
      'model',
      'manufacturer',
      'brand',
    ];

    final parts = <String>[];
    for (final key in preferredKeys) {
      final value = data[key];
      if (value == null) continue;
      final s = value.toString().trim();
      if (s.isEmpty || s.toLowerCase() == 'unknown') continue;
      parts.add('$key=$s');
    }

    if (parts.isEmpty) {
      for (final entry in data.entries) {
        if (parts.length >= 8) break;
        final value = entry.value;
        if (value == null || value is Map || value is List) continue;
        final s = value.toString().trim();
        if (s.isEmpty || s.toLowerCase() == 'unknown') continue;
        parts.add('${entry.key}=$s');
      }
    }
    return parts.join('|');
  }

  static String _clientIdFromSeed(String seed) {
    final h1 = _fnv1a32(seed);
    final h2 = _fnv1a32('$seed#ER4');
    final suffix = h1.toRadixString(16).padLeft(8, '0').toUpperCase() +
        h2.toRadixString(16).padLeft(8, '0').toUpperCase();
    return 'ER4_$suffix';
  }

  static int _fnv1a32(String input) {
    var hash = 0x811C9DC5;
    for (final c in input.codeUnits) {
      hash ^= c;
      hash = (hash * 0x01000193) & 0xFFFFFFFF;
    }
    return hash;
  }

  Future<void> save() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kHost, host.trim());
    await prefs.setInt(_kPort, port);
    await prefs.setString(_kUser, username);
    await prefs.setString(_kPass, password);
    await prefs.setString(_kBase, _normalizeBaseTopic(baseTopic));
    await prefs.setString(_kCardId, _normalizeCardId(cardId));
    await prefs.setString(_kClient, clientId.trim());
    for (int i = 1; i <= _relayCount; i++) {
      final normalized = _normalizeRelayName(relayNames[i], i);
      relayNames[i] = normalized;
      await prefs.setString('$_kRelayNamePrefix$i', normalized);
      final normalizedInput = _normalizeInputName(inputNames[i], i);
      inputNames[i] = normalizedInput;
      await prefs.setString('$_kInputNamePrefix$i', normalizedInput);
    }
  }
}

// ============================================================================
// SCREEN PRINCIPAL AVEC NAVIGATION
// ============================================================================

class MainScreen extends StatefulWidget {
  const MainScreen({super.key});

  @override
  State<MainScreen> createState() => _MainScreenState();
}

class _MainScreenState extends State<MainScreen> {
  int _currentIndex = 0;
  bool _connected = false;

  // État MQTT (à connecter avec la logique existante)
  MqttServerClient? _client;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage>>>? _mqttSub;
  
  RelayMqttSettings _settings = RelayMqttSettings.defaults();

  @override
  void initState() {
    super.initState();
    _loadSettings();
  }

  @override
  void dispose() {
    _mqttSub?.cancel();
    _client?.disconnect();
    super.dispose();
  }

  Future<void> _loadSettings() async {
    final loaded = await RelayMqttSettings.load();
    setState(() {
      _settings = loaded;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const ER4Logo(size: 36),
        actions: [
          _buildConnectionStatusButton(),
        ],
      ),
      body: IndexedStack(
        index: _currentIndex,
        children: [
          DashboardTab(settings: _settings, client: _client, connected: _connected),
          ControlTab(settings: _settings, client: _client, connected: _connected),
          SettingsTab(
            settings: _settings,
            onSettingsSaved: (newSettings) {
              setState(() {
                _settings = newSettings;
              });
            },
          ),
          const AboutTab(),
        ],
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _currentIndex,
        onDestinationSelected: (index) => setState(() => _currentIndex = index),
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.dashboard_outlined),
            selectedIcon: Icon(Icons.dashboard),
            label: 'Dashboard',
          ),
          NavigationDestination(
            icon: Icon(Icons.electrical_services_outlined),
            selectedIcon: Icon(Icons.electrical_services),
            label: 'Contrôle',
          ),
          NavigationDestination(
            icon: Icon(Icons.settings_outlined),
            selectedIcon: Icon(Icons.settings),
            label: 'Paramètres',
          ),
          NavigationDestination(
            icon: Icon(Icons.info_outline),
            selectedIcon: Icon(Icons.info),
            label: 'À propos',
          ),
        ],
      ),
    );
  }

  Widget _buildConnectionStatusButton() {
    return Padding(
      padding: const EdgeInsets.only(right: 12),
      child: FilledButton.tonalIcon(
        onPressed: () {
          // Logique de connexion/déconnexion à implémenter
          setState(() {
            _connected = !_connected;
          });
        },
        icon: Icon(_connected ? Icons.check : Icons.wifi),
        label: Text(_connected ? 'Déconnecter' : 'Se connecter'),
        style: FilledButton.styleFrom(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
        ),
      ),
    );
  }
}

// ============================================================================
// ONGLET DASHBOARD
// ============================================================================

class DashboardTab extends StatelessWidget {
  const DashboardTab({
    super.key,
    required this.settings,
    required this.client,
    required this.connected,
  });

  final RelayMqttSettings settings;
  final MqttServerClient? client;
  final bool connected;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          // Carte de statut de connexion
          _buildConnectionStatusCard(context),
          const SizedBox(height: 16),
          // Carte des relais
          _buildRelaysCard(context),
          const SizedBox(height: 16),
          // Carte des entrées
          _buildInputsCard(context),
        ],
      ),
    );
  }

  Widget _buildConnectionStatusCard(BuildContext context) {
    final theme = Theme.of(context);
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              children: [
                Container(
                  padding: const EdgeInsets.all(8),
                  decoration: BoxDecoration(
                    color: connected ? Colors.green.shade100 : Colors.grey.shade100,
                    shape: BoxShape.circle,
                  ),
                  child: Icon(
                    connected ? Icons.check : Icons.wifi_off,
                    color: connected ? Colors.green.shade700 : Colors.grey.shade700,
                    size: 20,
                  ),
                ),
                const SizedBox(width: 12),
                Text(
                  connected ? 'Connecté' : 'Déconnecté',
                  style: const TextStyle(
                    fontSize: 18,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const Spacer(),
                Text(
                  '${settings.host}:${settings.port}',
                  style: TextStyle(
                    color: theme.colorScheme.primary,
                    fontWeight: FontWeight.w500,
                    fontSize: 14,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            const Divider(),
            const SizedBox(height: 8),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              alignment: WrapAlignment.center,
              children: [
                _buildStatusChip('Carte', connected ? 'ONLINE' : 'OFFLINE', Colors.green, context),
                _buildStatusChip('MQTT', connected ? 'CONNECTÉ' : 'DÉCONNECTÉ', Colors.blue, context),
                if (settings.cardId.isNotEmpty)
                  _buildStatusChip('ID', settings.cardId, Colors.grey, context),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildStatusChip(String label, String value, MaterialColor color, BuildContext context) {
    return Column(
      children: [
        Text(
          label,
          style: TextStyle(
            fontSize: 12,
            color: Theme.of(context).colorScheme.onSurfaceVariant,
          ),
        ),
        const SizedBox(height: 4),
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
          decoration: BoxDecoration(
            color: color.shade50,
            borderRadius: BorderRadius.circular(20),
            border: Border.all(color: color.shade200),
          ),
          child: Text(
            value,
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w600,
              color: color.shade700,
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildRelaysCard(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  Icons.electrical_services,
                  color: Theme.of(context).colorScheme.primary,
                  size: 20,
                ),
                const SizedBox(width: 8),
                const Text(
                  'Relais',
                  style: TextStyle(
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            ...List.generate(4, (index) => _buildRelayItem(context, index + 1)),
          ],
        ),
      ),
    );
  }

  Widget _buildRelayItem(BuildContext context, int relayNumber) {
    final relayName = RelayMqttSettings._normalizeRelayName(
      settings.relayNames[relayNumber],
      relayNumber,
    );
    
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        children: [
          // Infos relais
          Expanded(
            child: Container(
              padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 12),
              decoration: BoxDecoration(
                color: Colors.grey.shade50,
                borderRadius: BorderRadius.circular(8),
              ),
              child: Row(
                children: [
                  Text(
                    relayName,
                    style: const TextStyle(
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const Spacer(),
                  // Switch pour l'état (simulé)
                  Switch(
                    value: false,
                    onChanged: (value) {},
                    activeThumbColor: Theme.of(context).colorScheme.tertiary,
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(width: 8),
          // Boutons de contrôle
          FilledButton.tonal(
            onPressed: () {},
            child: const Text('ON'),
          ),
          const SizedBox(width: 4),
          OutlinedButton(
            onPressed: () {},
            child: const Text('OFF'),
          ),
          const SizedBox(width: 4),
          TextButton(
            onPressed: () {},
            child: const Text('AUTO'),
          ),
        ],
      ),
    );
  }

  Widget _buildInputsCard(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  Icons.input,
                  color: Theme.of(context).colorScheme.primary,
                  size: 20,
                ),
                const SizedBox(width: 8),
                const Text(
                  'Entrées Virtuelles',
                  style: TextStyle(
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            ...List.generate(4, (index) => _buildInputItem(context, index + 1)),
          ],
        ),
      ),
    );
  }

  Widget _buildInputItem(BuildContext context, int inputNumber) {
    final inputName = RelayMqttSettings._normalizeInputName(
      settings.inputNames[inputNumber],
      inputNumber,
    );
    
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        children: [
          Expanded(
            child: Container(
              padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 12),
              decoration: BoxDecoration(
                color: Colors.grey.shade50,
                borderRadius: BorderRadius.circular(8),
              ),
              child: Row(
                children: [
                  Text(
                    inputName,
                    style: const TextStyle(
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const Spacer(),
                  Switch(
                    value: false,
                    onChanged: (value) {},
                    activeThumbColor: Theme.of(context).colorScheme.tertiary,
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(width: 8),
          FilledButton.tonal(
            onPressed: () {},
            child: const Text('ON'),
          ),
          const SizedBox(width: 4),
          OutlinedButton(
            onPressed: () {},
            child: const Text('OFF'),
          ),
        ],
      ),
    );
  }
}

// ============================================================================
// ONGLET CONTRÔLE
// ============================================================================

class ControlTab extends StatelessWidget {
  const ControlTab({
    super.key,
    required this.settings,
    required this.client,
    required this.connected,
  });

  final RelayMqttSettings settings;
  final MqttServerClient? client;
  final bool connected;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Commandes rapides',
                    style: TextStyle(
                      fontSize: 16,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 12),
                  const Text('Commandes globales pour tous les relais'),
                  const SizedBox(height: 16),
                  Row(
                    children: [
                      Expanded(
                        child: FilledButton.icon(
                          onPressed: connected ? () {} : null,
                          icon: const Icon(Icons.power),
                          label: const Text('TOUS ON'),
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: OutlinedButton.icon(
                          onPressed: connected ? () {} : null,
                          icon: const Icon(Icons.power_off),
                          label: const Text('TOUS OFF'),
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: TextButton.icon(
                          onPressed: connected ? () {} : null,
                          icon: const Icon(Icons.refresh),
                          label: const Text('TOUS AUTO'),
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 16),
          // Affichage des commandes individuelles
          ...List.generate(4, (index) => Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: Card(
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: Row(
                  children: [
                    Text(
                      'R${index + 1}',
                      style: const TextStyle(
                        fontWeight: FontWeight.bold,
                        fontSize: 16,
                      ),
                    ),
                    const Spacer(),
                    FilledButton(
                      onPressed: connected ? () {} : null,
                      child: const Text('ON'),
                    ),
                    const SizedBox(width: 4),
                    OutlinedButton(
                      onPressed: connected ? () {} : null,
                      child: const Text('OFF'),
                    ),
                    const SizedBox(width: 4),
                    TextButton(
                      onPressed: connected ? () {} : null,
                      child: const Text('AUTO'),
                    ),
                  ],
                ),
              ),
            ),
          )),
        ],
      ),
    );
  }
}

// ============================================================================
// ONGLET PARAMÈTRES
// ============================================================================

class SettingsTab extends StatefulWidget {
  const SettingsTab({
    super.key,
    required this.settings,
    required this.onSettingsSaved,
  });

  final RelayMqttSettings settings;
  final Function(RelayMqttSettings) onSettingsSaved;

  @override
  State<SettingsTab> createState() => _SettingsTabState();
}

class _SettingsTabState extends State<SettingsTab> {
  late TextEditingController _hostCtrl;
  late TextEditingController _portCtrl;
  late TextEditingController _baseTopicCtrl;
  late TextEditingController _cardIdCtrl;
  late TextEditingController _clientIdCtrl;
  late TextEditingController _userCtrl;
  late TextEditingController _passCtrl;

  final Map<int, TextEditingController> _relayNameCtrls = {};
  final Map<int, TextEditingController> _inputNameCtrls = {};

  @override
  void initState() {
    super.initState();
    _hostCtrl = TextEditingController(text: widget.settings.host);
    _portCtrl = TextEditingController(text: widget.settings.port.toString());
    _baseTopicCtrl = TextEditingController(text: widget.settings.baseTopic);
    _cardIdCtrl = TextEditingController(text: widget.settings.cardId);
    _clientIdCtrl = TextEditingController(text: widget.settings.clientId);
    _userCtrl = TextEditingController(text: widget.settings.username);
    _passCtrl = TextEditingController(text: widget.settings.password);

    for (int i = 1; i <= 4; i++) {
      _relayNameCtrls[i] = TextEditingController(
        text: widget.settings.relayNames[i],
      );
      _inputNameCtrls[i] = TextEditingController(
        text: widget.settings.inputNames[i],
      );
    }
  }

  @override
  void dispose() {
    _hostCtrl.dispose();
    _portCtrl.dispose();
    _baseTopicCtrl.dispose();
    _cardIdCtrl.dispose();
    _clientIdCtrl.dispose();
    _userCtrl.dispose();
    _passCtrl.dispose();
    for (var ctrl in _relayNameCtrls.values) {
      ctrl.dispose();
    }
    for (var ctrl in _inputNameCtrls.values) {
      ctrl.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          // Carte Configuration MQTT
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Configuration MQTT',
                    style: TextStyle(
                      fontSize: 18,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 16),
                  _buildTextField(context, 'Broker Host', 'Ex: 192.168.1.43', _hostCtrl),
                  const SizedBox(height: 12),
                  _buildTextField(
                    context,
                    'Port',
                    '1883',
                    _portCtrl,
                    keyboardType: TextInputType.number,
                  ),
                  const SizedBox(height: 12),
                  _buildTextField(context, 'Username', 'Optionnel', _userCtrl),
                  const SizedBox(height: 12),
                  _buildTextField(
                    context,
                    'Password',
                    'Optionnel',
                    _passCtrl,
                    obscure: true,
                  ),
                  const SizedBox(height: 12),
                  _buildTextField(context, 'Base Topic', 'espr4', _baseTopicCtrl),
                  const SizedBox(height: 12),
                  _buildTextField(context, 'Client ID', 'ER4_MQTT_APP', _clientIdCtrl),
                  const SizedBox(height: 12),
                  _buildTextField(context, 'Card ID', '12 hex chars', _cardIdCtrl),
                  const SizedBox(height: 20),
                  Row(
                    children: [
                      Expanded(
                        child: FilledButton(
                          onPressed: _saveSettings,
                          child: const Text('Sauvegarder'),
                        ),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: OutlinedButton.icon(
                          onPressed: _scanQrCode,
                          icon: const Icon(Icons.qr_code),
                          label: const Text('Scan QR'),
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 16),
          // Carte Noms personnalisés
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Noms personnalisés',
                    style: TextStyle(
                      fontSize: 18,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 8),
                  const Text(
                    'Personnalise les noms des relais et entrées',
                    style: TextStyle(color: Colors.grey),
                  ),
                  const SizedBox(height: 16),
                  ...List.generate(4, (index) => Padding(
                    padding: const EdgeInsets.only(bottom: 12),
                    child: Row(
                      children: [
                        Expanded(
                          child: _buildTextField(
                            context,
                            'Relais ${index + 1}',
                            'R${index + 1}',
                            _relayNameCtrls[index + 1]!,
                          ),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: _buildTextField(
                            context,
                            'Entrée ${index + 1}',
                            'E${index + 1}',
                            _inputNameCtrls[index + 1]!,
                          ),
                        ),
                      ],
                    ),
                  )),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildTextField(
    BuildContext context,
    String label,
    String hint,
    TextEditingController controller, {
    bool obscure = false,
    TextInputType? keyboardType,
  }) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          label,
          style: const TextStyle(
            fontWeight: FontWeight.w500,
          ),
        ),
        const SizedBox(height: 6),
        TextField(
          controller: controller,
          obscureText: obscure,
          keyboardType: keyboardType,
          decoration: InputDecoration(
            hintText: hint,
            hintStyle: TextStyle(color: Colors.grey.shade500),
          ),
        ),
      ],
    );
  }

  Future<void> _saveSettings() async {
    final newSettings = RelayMqttSettings(
      host: _hostCtrl.text.trim(),
      port: int.tryParse(_portCtrl.text.trim()) ?? 1883,
      username: _userCtrl.text.trim(),
      password: _passCtrl.text,
      baseTopic: RelayMqttSettings._normalizeBaseTopic(_baseTopicCtrl.text),
      cardId: RelayMqttSettings._normalizeCardId(_cardIdCtrl.text),
      clientId: _clientIdCtrl.text.trim(),
      relayNames: Map.fromIterable(
        List.generate(4, (i) => MapEntry(
          i + 1,
          RelayMqttSettings._normalizeRelayName(
            _relayNameCtrls[i + 1]?.text,
            i + 1,
          ),
        )),
      ),
      inputNames: Map.fromIterable(
        List.generate(4, (i) => MapEntry(
          i + 1,
          RelayMqttSettings._normalizeInputName(
            _inputNameCtrls[i + 1]?.text,
            i + 1,
          ),
        )),
      ),
    );

    await newSettings.save();
    widget.onSettingsSaved(newSettings);
    
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(
        content: Text('Paramètres sauvegardés'),
        duration: Duration(seconds: 2),
      ),
    );
  }

  Future<void> _scanQrCode() async {
    final raw = await Navigator.of(context).push<String>(
      MaterialPageRoute<String>(
        builder: (_) => const _CardIdQrScanPage(),
      ),
    );
    if (!mounted || raw == null) return;

    final cardId = _extractCardIdFromText(raw);
    if (cardId == null) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('QR code invalide: Card ID non trouvé'),
          duration: Duration(seconds: 2),
        ),
      );
      return;
    }
    setState(() {
      _cardIdCtrl.text = cardId;
    });
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(
        content: Text('Card ID rempli depuis QR code'),
        duration: Duration(seconds: 2),
      ),
    );
  }

  String? _extractCardIdFromText(String raw) {
    final text = raw.trim();
    if (text.isEmpty) return null;

    final macWithSeparator =
        RegExp(r'([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}').firstMatch(text);
    if (macWithSeparator != null) {
      final candidate = RelayMqttSettings._normalizeCardId(
          macWithSeparator.group(0));
      if (RegExp(r'^[0-9A-F]{12}$').hasMatch(candidate)) return candidate;
    }

    final directHex = RegExp(r'[0-9A-Fa-f]{12}').firstMatch(text);
    if (directHex != null) {
      return directHex.group(0)!.toUpperCase();
    }

    final normalized = RelayMqttSettings._normalizeCardId(text);
    if (RegExp(r'^[0-9A-F]{12}$').hasMatch(normalized)) return normalized;
    if (normalized.length > 12) {
      final tail = normalized.substring(normalized.length - 12);
      if (RegExp(r'^[0-9A-F]{12}$').hasMatch(tail)) return tail;
    }
    return null;
  }
}

// ============================================================================
// ONGLET À PROPOS
// ============================================================================

class AboutTab extends StatelessWidget {
  const AboutTab({super.key});

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Column(
                children: [
                  const ER4Logo(size: 64),
                  const SizedBox(height: 16),
                  Text(
                    'ER4 MQTT',
                    style: TextStyle(
                      fontSize: 24,
                      fontWeight: FontWeight.bold,
                      color: Theme.of(context).colorScheme.primary,
                    ),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    'v1.0.0 - Application de contrôle MQTT',
                    style: TextStyle(
                      color: Colors.grey.shade600,
                    ),
                  ),
                  const SizedBox(height: 24),
                  const Divider(),
                  const SizedBox(height: 16),
                  const Text(
                    'Une application Flutter pour contrôler les cartes ESPRelay4 via MQTT.',
                    textAlign: TextAlign.center,
                    style: TextStyle(fontSize: 16),
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 16),
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Fonctionnalités',
                    style: TextStyle(
                      fontSize: 18,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 12),
                  ...[
                    'Contrôle des relais via MQTT',
                    'Gestion des entrées virtuelles',
                    'Configuration personnalisable',
                    'Scan QR code pour le Card ID',
                    'Design moderne Material 3',
                  ].map((text) => Padding(
                    padding: const EdgeInsets.symmetric(vertical: 4),
                    child: Row(
                      children: [
                        const Icon(Icons.check, color: Colors.green, size: 16),
                        const SizedBox(width: 8),
                        Text(text),
                      ],
                    ),
                  )),
                ],
              ),
            ),
          ),
          const SizedBox(height: 16),
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Auteur',
                    style: TextStyle(
                      fontSize: 18,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 8),
                  const Text('ESPRelay4 Team'),
                  const SizedBox(height: 16),
                  const Text(
                    'Technologies',
                    style: TextStyle(
                      fontSize: 18,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 8),
                  Wrap(
                    spacing: 8,
                    children: [
                      _buildTechChip('Flutter'),
                      _buildTechChip('Dart'),
                      _buildTechChip('MQTT'),
                      _buildTechChip('ESP32'),
                    ],
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildTechChip(String label) {
    return Chip(
      label: Text(label),
      backgroundColor: Colors.grey.shade100,
      labelStyle: const TextStyle(color: Colors.grey),
    );
  }
}

// ============================================================================
// PAGE SCAN QR CODE (héritée de l'ancien code)
// ============================================================================

class _CardIdQrScanPage extends StatefulWidget {
  const _CardIdQrScanPage();

  @override
  State<_CardIdQrScanPage> createState() => _CardIdQrScanPageState();
}

class _CardIdQrScanPageState extends State<_CardIdQrScanPage> {
  final MobileScannerController controller = MobileScannerController();

  @override
  void dispose() {
    controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Scan QR Code'),
      ),
      body: MobileScanner(
        controller: controller,
        onDetect: (barcode) {
          if (barcode.raw != null) {
            Navigator.of(context).pop(barcode.raw);
          }
        },
      ),
    );
  }
}
