import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:http/http.dart' as http;
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final repo = await CardRepository.load();
  runApp(EspR4App(repository: repo));
}

class EspR4App extends StatelessWidget {
  const EspR4App({super.key, required this.repository});

  final CardRepository repository;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ESP R4 Manager',
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF1565C0),
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: const Color(0xFFF3F7FF),
      ),
      home: CardsHomePage(repository: repository),
    );
  }
}

enum TransportPolicy { auto, local, gsm, ble }

enum ActiveTransport { none, local, gsm, ble }

String transportPolicyLabel(TransportPolicy p) {
  switch (p) {
    case TransportPolicy.auto:
      return 'AUTO (Local -> GSM -> BLE)';
    case TransportPolicy.local:
      return 'MQTT Local';
    case TransportPolicy.gsm:
      return 'MQTT GSM';
    case TransportPolicy.ble:
      return 'BLE';
  }
}

String transportPolicyCompactLabel(TransportPolicy p) {
  switch (p) {
    case TransportPolicy.auto:
      return 'AUTO';
    case TransportPolicy.local:
      return 'MQTT Local';
    case TransportPolicy.gsm:
      return 'MQTT GSM';
    case TransportPolicy.ble:
      return 'BLE';
  }
}

String activeTransportLabel(ActiveTransport t) {
  switch (t) {
    case ActiveTransport.none:
      return 'none';
    case ActiveTransport.local:
      return 'local';
    case ActiveTransport.gsm:
      return 'gsm';
    case ActiveTransport.ble:
      return 'ble';
  }
}

Guid bleUuidFor(String base, String suffix12) {
  return Guid('6E4000$base-B5A3-F393-E0A9-$suffix12');
}

class EspCard {
  EspCard({
    required this.cardId,
    required this.name,
    required this.policy,
    required this.httpHost,
    required this.httpUser,
    required this.httpPass,
    required this.localHost,
    required this.localPort,
    required this.localUser,
    required this.localPass,
    required this.gsmHost,
    required this.gsmPort,
    required this.gsmUser,
    required this.gsmPass,
    required this.retain,
    required this.updatedAtMs,
  });

  String cardId;
  String name;
  TransportPolicy policy;
  String httpHost;
  String httpUser;
  String httpPass;
  String localHost;
  int localPort;
  String localUser;
  String localPass;
  String gsmHost;
  int gsmPort;
  String gsmUser;
  String gsmPass;
  bool retain;
  int updatedAtMs;

  static String normalizeCardId(String raw) {
    return raw
        .trim()
        .replaceAll(':', '')
        .replaceAll('-', '')
        .replaceAll(RegExp(r'\s+'), '')
        .toUpperCase();
  }

  static bool isValidCardId(String raw) {
    return RegExp(r'^[0-9A-F]{12}$').hasMatch(normalizeCardId(raw));
  }

  String get baseTopic => 'espr4/$cardId';

  EspCard copy() {
    return EspCard(
      cardId: cardId,
      name: name,
      policy: policy,
      httpHost: httpHost,
      httpUser: httpUser,
      httpPass: httpPass,
      localHost: localHost,
      localPort: localPort,
      localUser: localUser,
      localPass: localPass,
      gsmHost: gsmHost,
      gsmPort: gsmPort,
      gsmUser: gsmUser,
      gsmPass: gsmPass,
      retain: retain,
      updatedAtMs: updatedAtMs,
    );
  }

  Map<String, dynamic> toJson() {
    return {
      'card_id': cardId,
      'name': name,
      'policy': policy.name,
      'http_host': httpHost,
      'http_user': httpUser,
      'http_pass': httpPass,
      'local_host': localHost,
      'local_port': localPort,
      'local_user': localUser,
      'local_pass': localPass,
      'gsm_host': gsmHost,
      'gsm_port': gsmPort,
      'gsm_user': gsmUser,
      'gsm_pass': gsmPass,
      'retain': retain ? 1 : 0,
      'updated_at_ms': updatedAtMs,
    };
  }

  static EspCard fromJson(Map<String, dynamic> map) {
    final parsedPolicy = switch ((map['policy'] ?? 'auto').toString()) {
      'local' => TransportPolicy.local,
      'gsm' => TransportPolicy.gsm,
      'ble' => TransportPolicy.ble,
      _ => TransportPolicy.auto,
    };

    return EspCard(
      cardId: normalizeCardId((map['card_id'] ?? '').toString()),
      name: (map['name'] ?? 'ESPRelay4').toString(),
      policy: parsedPolicy,
      httpHost: (map['http_host'] ?? '').toString(),
      httpUser: (map['http_user'] ?? '').toString(),
      httpPass: (map['http_pass'] ?? '').toString(),
      localHost: (map['local_host'] ?? '').toString(),
      localPort: _intOr(map['local_port'], 1883),
      localUser: (map['local_user'] ?? '').toString(),
      localPass: (map['local_pass'] ?? '').toString(),
      gsmHost: (map['gsm_host'] ?? '').toString(),
      gsmPort: _intOr(map['gsm_port'], 1883),
      gsmUser: (map['gsm_user'] ?? '').toString(),
      gsmPass: (map['gsm_pass'] ?? '').toString(),
      retain: _intOr(map['retain'], 1) == 1,
      updatedAtMs: _intOr(
        map['updated_at_ms'],
        DateTime.now().millisecondsSinceEpoch,
      ),
    );
  }

  static int _intOr(dynamic v, int fallback) {
    if (v is int) return v;
    if (v is num) return v.toInt();
    return int.tryParse(v?.toString() ?? '') ?? fallback;
  }

  static EspCard defaults() {
    return EspCard(
      cardId: '',
      name: 'Nouvelle carte',
      policy: TransportPolicy.auto,
      httpHost: '',
      httpUser: '',
      httpPass: '',
      localHost: '192.168.1.43',
      localPort: 1883,
      localUser: '',
      localPass: '',
      gsmHost: '',
      gsmPort: 1883,
      gsmUser: '',
      gsmPass: '',
      retain: true,
      updatedAtMs: DateTime.now().millisecondsSinceEpoch,
    );
  }
}

class CardRepository {
  CardRepository(this.cards);

  static const _kCards = 'espr4_cards_v1';
  final List<EspCard> cards;

  static Future<CardRepository> load() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_kCards) ?? '[]';
    try {
      final decoded = jsonDecode(raw);
      if (decoded is List) {
        final loaded = decoded
            .whereType<Map>()
            .map((m) => EspCard.fromJson(Map<String, dynamic>.from(m)))
            .where((c) => c.cardId.isNotEmpty)
            .toList();
        loaded.sort((a, b) => b.updatedAtMs.compareTo(a.updatedAtMs));
        return CardRepository(loaded);
      }
    } catch (_) {
      // fallback below
    }
    return CardRepository(<EspCard>[]);
  }

  Future<void> save() async {
    final prefs = await SharedPreferences.getInstance();
    final payload = jsonEncode(cards.map((c) => c.toJson()).toList());
    await prefs.setString(_kCards, payload);
  }

  Future<void> upsert(EspCard card) async {
    card.updatedAtMs = DateTime.now().millisecondsSinceEpoch;
    final idx = cards.indexWhere((c) => c.cardId == card.cardId);
    if (idx >= 0) {
      cards[idx] = card.copy();
    } else {
      cards.add(card.copy());
    }
    cards.sort((a, b) => b.updatedAtMs.compareTo(a.updatedAtMs));
    await save();
  }

  Future<void> delete(String cardId) async {
    cards.removeWhere((c) => c.cardId == cardId);
    await save();
  }
}

class CardsHomePage extends StatefulWidget {
  const CardsHomePage({super.key, required this.repository});

  final CardRepository repository;

  @override
  State<CardsHomePage> createState() => _CardsHomePageState();
}

class _CardsHomePageState extends State<CardsHomePage> {
  bool _busyBle = false;

  Future<void> _openCardEditor({EspCard? existing}) async {
    final updated = await Navigator.of(context).push<EspCard>(
      MaterialPageRoute(
        builder: (_) => CardEditPage(initialCard: existing),
      ),
    );
    if (updated == null) return;
    await widget.repository.upsert(updated);
    if (!mounted) return;
    setState(() {});
  }

  Future<void> _openCardDetail(EspCard card) async {
    await Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => CardDetailPage(
          card: card.copy(),
          repository: widget.repository,
        ),
      ),
    );
    if (!mounted) return;
    setState(() {});
  }

  Future<void> _addCardFromBle() async {
    setState(() => _busyBle = true);
    final scanned = await Navigator.of(context).push<BleScanResult>(
      MaterialPageRoute(builder: (_) => const BleScannerPage()),
    );
    if (!mounted) return;
    setState(() => _busyBle = false);
    if (scanned == null) return;

    final card = EspCard.defaults()
      ..cardId = scanned.cardId
      ..name = scanned.suggestedName;

    final edited = await Navigator.of(context).push<EspCard>(
      MaterialPageRoute(
        builder: (_) => CardEditPage(
          initialCard: card,
          initialBleScan: scanned,
        ),
      ),
    );
    if (edited == null) return;
    await widget.repository.upsert(edited);
    if (!mounted) return;
    setState(() {});
  }

  Future<void> _deleteCard(EspCard card) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        title: const Text('Supprimer la carte'),
        content: Text('Supprimer ${card.name} (${card.cardId}) ?'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('Annuler'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('Supprimer'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    await widget.repository.delete(card.cardId);
    if (!mounted) return;
    setState(() {});
  }

  Widget _policyChip(TransportPolicy p) {
    final (bg, fg, label) = switch (p) {
      TransportPolicy.auto => (
          const Color(0xFFEAF4FF),
          const Color(0xFF0D47A1),
          'AUTO'
        ),
      TransportPolicy.local => (
          const Color(0xFFE3F2FD),
          const Color(0xFF0B3D91),
          'LOCAL'
        ),
      TransportPolicy.gsm => (
          const Color(0xFFE1F5FE),
          const Color(0xFF01579B),
          'GSM'
        ),
      TransportPolicy.ble => (
          const Color(0xFFE8EAF6),
          const Color(0xFF1A237E),
          'BLE'
        ),
    };

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      decoration: BoxDecoration(
        color: bg,
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: bg.withValues(alpha: 0.7)),
      ),
      child: Text(label,
          style: TextStyle(fontWeight: FontWeight.w700, color: fg, fontSize: 12)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final cards = widget.repository.cards;

    return Scaffold(
      appBar: AppBar(
        title: const Text('ESP R4 Manager'),
        actions: [
          IconButton(
            tooltip: 'Ajouter une carte (manuel)',
            onPressed: () => _openCardEditor(),
            icon: const Icon(Icons.add),
          ),
          IconButton(
            tooltip: 'Ajouter via BLE',
            onPressed: _busyBle ? null : _addCardFromBle,
            icon: const Icon(Icons.bluetooth_searching),
          ),
        ],
      ),
      body: cards.isEmpty
          ? Center(
              child: Padding(
                padding: const EdgeInsets.all(24),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Icon(Icons.router_outlined, size: 56),
                    const SizedBox(height: 12),
                    const Text(
                      'Aucune carte enregistrée',
                      style: TextStyle(fontWeight: FontWeight.w700, fontSize: 18),
                    ),
                    const SizedBox(height: 8),
                    const Text(
                      'Ajoute une carte manuellement, par QR code, ou via BLE.',
                      textAlign: TextAlign.center,
                    ),
                    const SizedBox(height: 14),
                    Wrap(
                      spacing: 8,
                      runSpacing: 8,
                      alignment: WrapAlignment.center,
                      children: [
                        FilledButton.icon(
                          onPressed: () => _openCardEditor(),
                          icon: const Icon(Icons.add),
                          label: const Text('Ajouter carte'),
                        ),
                        OutlinedButton.icon(
                          onPressed: _busyBle ? null : _addCardFromBle,
                          icon: const Icon(Icons.bluetooth_searching),
                          label: const Text('Scanner BLE'),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            )
          : ListView.separated(
              padding: const EdgeInsets.all(14),
              itemCount: cards.length,
              separatorBuilder: (_, _) => const SizedBox(height: 10),
              itemBuilder: (_, i) {
                final c = cards[i];
                return Card(
                  elevation: 0,
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12),
                    side: const BorderSide(color: Color(0xFFDADADA)),
                  ),
                  child: ListTile(
                    contentPadding:
                        const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
                    title: Text(c.name,
                        style: const TextStyle(fontWeight: FontWeight.w700)),
                    subtitle: Padding(
                      padding: const EdgeInsets.only(top: 8),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          SelectableText(
                            c.cardId,
                            style: const TextStyle(
                              fontFamily: 'monospace',
                              fontWeight: FontWeight.w600,
                              fontSize: 12,
                            ),
                          ),
                          const SizedBox(height: 6),
                          Wrap(
                            spacing: 8,
                            runSpacing: 8,
                            children: [
                              _policyChip(c.policy),
                              if (c.localHost.trim().isNotEmpty)
                                _tinyChip('Local ${c.localHost}:${c.localPort}'),
                              if (c.gsmHost.trim().isNotEmpty)
                                _tinyChip('GSM ${c.gsmHost}:${c.gsmPort}'),
                            ],
                          ),
                        ],
                      ),
                    ),
                    trailing: PopupMenuButton<String>(
                      onSelected: (value) {
                        if (value == 'edit') {
                          _openCardEditor(existing: c);
                        } else if (value == 'delete') {
                          _deleteCard(c);
                        }
                      },
                      itemBuilder: (_) => const [
                        PopupMenuItem(value: 'edit', child: Text('Modifier')),
                        PopupMenuItem(value: 'delete', child: Text('Supprimer')),
                      ],
                    ),
                    onTap: () => _openCardDetail(c),
                  ),
                );
              },
            ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: () => _openCardEditor(),
        icon: const Icon(Icons.add),
        label: const Text('Nouvelle carte'),
      ),
    );
  }

  Widget _tinyChip(String text) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        color: const Color(0xFFF3F3F3),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(text,
          style: const TextStyle(fontSize: 11, fontWeight: FontWeight.w500)),
    );
  }
}

class CardEditPage extends StatefulWidget {
  const CardEditPage({super.key, this.initialCard, this.initialBleScan});

  final EspCard? initialCard;
  final BleScanResult? initialBleScan;

  @override
  State<CardEditPage> createState() => _CardEditPageState();
}

class _CardEditPageState extends State<CardEditPage> {
  late final TextEditingController _nameCtrl;
  late final TextEditingController _cardIdCtrl;
  late final TextEditingController _httpHostCtrl;
  late final TextEditingController _httpUserCtrl;
  late final TextEditingController _httpPassCtrl;
  late final TextEditingController _localHostCtrl;
  late final TextEditingController _localPortCtrl;
  late final TextEditingController _localUserCtrl;
  late final TextEditingController _localPassCtrl;
  late final TextEditingController _gsmHostCtrl;
  late final TextEditingController _gsmPortCtrl;
  late final TextEditingController _gsmUserCtrl;
  late final TextEditingController _gsmPassCtrl;

  late TransportPolicy _policy;
  late bool _retain;
  bool _importBusy = false;
  bool _saveBusy = false;
  bool _bleFetchBusy = false;
  double _bleFetchProgress = 0;
  String _bleFetchStatus = '';
  TransportPolicy? _lastBlePolicyHint;

  @override
  void initState() {
    super.initState();
    final c = widget.initialCard?.copy() ?? EspCard.defaults();
    _nameCtrl = TextEditingController(text: c.name);
    _cardIdCtrl = TextEditingController(text: c.cardId);
    _httpHostCtrl = TextEditingController(text: c.httpHost);
    _httpUserCtrl = TextEditingController(text: c.httpUser);
    _httpPassCtrl = TextEditingController(text: c.httpPass);
    _localHostCtrl = TextEditingController(text: c.localHost);
    _localPortCtrl = TextEditingController(text: c.localPort.toString());
    _localUserCtrl = TextEditingController(text: c.localUser);
    _localPassCtrl = TextEditingController(text: c.localPass);
    _gsmHostCtrl = TextEditingController(text: c.gsmHost);
    _gsmPortCtrl = TextEditingController(text: c.gsmPort.toString());
    _gsmUserCtrl = TextEditingController(text: c.gsmUser);
    _gsmPassCtrl = TextEditingController(text: c.gsmPass);
    _policy = c.policy;
    _retain = c.retain;
    if (widget.initialBleScan != null) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted) return;
        _handleBleSelection(widget.initialBleScan!);
      });
    }
  }

  @override
  void dispose() {
    _nameCtrl.dispose();
    _cardIdCtrl.dispose();
    _httpHostCtrl.dispose();
    _httpUserCtrl.dispose();
    _httpPassCtrl.dispose();
    _localHostCtrl.dispose();
    _localPortCtrl.dispose();
    _localUserCtrl.dispose();
    _localPassCtrl.dispose();
    _gsmHostCtrl.dispose();
    _gsmPortCtrl.dispose();
    _gsmUserCtrl.dispose();
    _gsmPassCtrl.dispose();
    super.dispose();
  }

  Future<void> _scanQr() async {
    final value = await Navigator.of(context).push<String>(
      MaterialPageRoute(builder: (_) => const QrScanPage()),
    );
    if (!mounted || value == null) return;
    final extracted = extractCardIdFromAny(value);
    if (extracted == null) {
      _snack('QR invalide: Card ID non trouvé.');
      return;
    }
    setState(() => _cardIdCtrl.text = extracted);
    _snack('Card ID rempli depuis QR.');
  }

  Future<void> _scanBle() async {
    if (_importBusy || _bleFetchBusy) return;
    final result = await Navigator.of(context).push<BleScanResult>(
      MaterialPageRoute(builder: (_) => const BleScannerPage()),
    );
    if (!mounted || result == null) return;
    await _handleBleSelection(result);
  }

  void _setBleProgress(double progress, String status) {
    if (!mounted) return;
    setState(() {
      _bleFetchBusy = true;
      _bleFetchProgress = progress.clamp(0, 1);
      _bleFetchStatus = status;
    });
  }

  void _clearBleProgress() {
    if (!mounted) return;
    setState(() {
      _bleFetchBusy = false;
      _bleFetchProgress = 0;
      _bleFetchStatus = '';
    });
  }

  Future<void> _handleBleSelection(BleScanResult result) async {
    if (!mounted || _importBusy) return;
    final messenger = ScaffoldMessenger.of(context);
    _cardIdCtrl.text = result.cardId;
    if (_nameCtrl.text.trim().isEmpty || _nameCtrl.text.trim() == 'Nouvelle carte') {
      _nameCtrl.text = result.suggestedName;
    }
    messenger.showSnackBar(
      const SnackBar(
        content: Text('Connexion BLE puis récupération de la configuration...'),
        duration: Duration(seconds: 2),
      ),
    );

    _setBleProgress(0.05, 'Connexion BLE...');
    try {
      final bleHost = await _readHttpHostFromBle(
        result,
        onProgress: (p, s) => _setBleProgress(p, s),
      );
      if (!mounted) return;
      final blePolicyHint = _lastBlePolicyHint;
      if (blePolicyHint != null && blePolicyHint != TransportPolicy.ble) {
        setState(() => _policy = blePolicyHint);
      }
      if (bleHost == null || bleHost.isEmpty) {
        _clearBleProgress();
        if (blePolicyHint != null) {
          _snack(
            'Mode MQTT actif détecté via BLE: ${transportPolicyCompactLabel(blePolicyHint)}',
          );
        }
        _snack('IP non récupérée via BLE. Refaire le scan BLE.');
        return;
      }
      _httpHostCtrl.text = bleHost;
      final modeSuffix = (blePolicyHint == null)
          ? ''
          : ' | mode ${transportPolicyCompactLabel(blePolicyHint)}';
      _setBleProgress(1.0, 'IP trouvée: $bleHost$modeSuffix');
      await Future<void>.delayed(const Duration(milliseconds: 250));
      _clearBleProgress();

      final ok = await _askWebAccessDialog(bleHost);
      if (!mounted || !ok) return;
      final allowImport = await _warnIfVpnLikelyEnabled();
      if (!mounted || !allowImport) return;
      FocusManager.instance.primaryFocus?.unfocus();
      await Future<void>.delayed(const Duration(milliseconds: 150));
      if (!mounted) return;
      await _importFromCardHttp();
    } finally {
      if (mounted && _bleFetchBusy) _clearBleProgress();
    }
  }

  Future<bool> _askWebAccessDialog(String host) async {
    final result = await showDialog<_WebAuthDialogResult>(
      context: context,
      barrierDismissible: false,
      builder: (_) => _WebAuthDialog(
        host: host,
        initialUser: _httpUserCtrl.text,
        initialPass: _httpPassCtrl.text,
      ),
    );

    if (result == null) return false;
    _httpHostCtrl.text = host;
    _httpUserCtrl.text = result.user.trim();
    _httpPassCtrl.text = result.pass;
    return true;
  }

  Map<String, dynamic>? _parseBleStateJson(String raw) {
    try {
      final obj = jsonDecode(raw);
      if (obj is Map<String, dynamic>) return obj;
    } catch (_) {
      // try framed payload below
    }
    final start = raw.indexOf('\u001E\u001E');
    if (start < 0) return null;
    final end = raw.indexOf('\u001F', start + 2);
    if (end <= start + 2) return null;
    final payload = raw.substring(start + 2, end);
    try {
      final obj = jsonDecode(payload);
      if (obj is Map<String, dynamic>) return obj;
    } catch (_) {}
    return null;
  }

  bool _isUsableHost(String raw) {
    final v = raw.trim();
    if (v.isEmpty) return false;
    if (v == '--' || v == '0.0.0.0') return false;
    if (v.toLowerCase() == 'null') return false;
    return true;
  }

  bool _bleFieldToBool(dynamic value) {
    final lower = value?.toString().trim().toLowerCase() ?? '';
    return lower == '1' || lower == 'true' || lower == 'on' || lower == 'yes';
  }

  TransportPolicy? _policyFromBleStateJson(Map<String, dynamic> parsed) {
    final eth = parsed['eth'];
    final gsm = parsed['gsm'];
    final ethLink = (eth is Map) ? _bleFieldToBool(eth['link']) : false;
    final gsmReady = (gsm is Map)
        ? (_bleFieldToBool(gsm['ready']) ||
            (_bleFieldToBool(gsm['network']) && _bleFieldToBool(gsm['data'])))
        : false;
    final mqtt = parsed['mqtt'];
    if (mqtt is! Map) {
      if (!ethLink && gsmReady) return TransportPolicy.gsm;
      if (ethLink && !gsmReady) return TransportPolicy.local;
      return null;
    }
    final active = (mqtt['active_transport'] ?? '').toString().trim().toLowerCase();
    final activeHasEth = active.contains('ethernet') || active.contains('eth');
    final activeHasGsm = active.contains('gsm');
    final onGsm = _bleFieldToBool(mqtt['on_gsm']);
    final ethConnected = _bleFieldToBool(mqtt['eth_connected']);
    final gsmConnected =
        _bleFieldToBool(mqtt['gsm_connected']) || onGsm;
    // Ethernet link physically down: prioritize GSM, even if stale ETH MQTT flags remain.
    if (!ethLink && (gsmReady || gsmConnected || activeHasGsm)) {
      return TransportPolicy.gsm;
    }
    if (activeHasEth && activeHasGsm) {
      return ethLink ? TransportPolicy.local : TransportPolicy.gsm;
    }
    if (active == 'ethernet' || active == 'eth' || active == 'local' || activeHasEth) {
      return TransportPolicy.local;
    }
    if (active == 'gsm' || activeHasGsm) return TransportPolicy.gsm;
    if (ethConnected && !gsmConnected) return TransportPolicy.local;
    if (gsmConnected) return TransportPolicy.gsm;
    if (!ethLink && gsmReady) return TransportPolicy.gsm;
    if (ethLink && !gsmReady) return TransportPolicy.local;
    return null;
  }

  String? _extractHttpHostFromBleJson(Map<String, dynamic> parsed) {
    final candidates = <String>[];

    void addCandidate(dynamic value) {
      if (value == null) return;
      candidates.add(value.toString().trim());
    }

    final eth = parsed['eth'];
    if (eth is Map) addCandidate(eth['ip']);
    final wifi = parsed['wifi'];
    if (wifi is Map) addCandidate(wifi['ip']);
    final mqtt = parsed['mqtt'];
    if (mqtt is Map) addCandidate(mqtt['ip']);
    addCandidate(parsed['ip']);

    for (final c in candidates) {
      if (_isUsableHost(c)) return c;
    }
    return null;
  }

  String? _extractHttpHostFromBleBytes(List<int> value) {
    if (value.isEmpty) return null;
    final txt = utf8.decode(value, allowMalformed: true);
    final parsed = _parseBleStateJson(txt);
    if (parsed != null) {
      _lastBlePolicyHint = _policyFromBleStateJson(parsed) ?? _lastBlePolicyHint;
      final fromJson = _extractHttpHostFromBleJson(parsed);
      if (fromJson != null) return fromJson;
    }
    return _extractIpFromStateText(txt);
  }

  String? _extractIpFromStateText(String raw) {
    // Keep fallback strict to avoid picking random GSM/operator fields.
    final candidates = <String?>[
      RegExp(
        r'"eth"\s*:\s*\{[^{}]*"ip"\s*:\s*"(\d{1,3}(?:\.\d{1,3}){3})"',
      ).firstMatch(raw)?.group(1),
      RegExp(
        r'"wifi"\s*:\s*\{[^{}]*"ip"\s*:\s*"(\d{1,3}(?:\.\d{1,3}){3})"',
      ).firstMatch(raw)?.group(1),
      RegExp(
        r'"mqtt"\s*:\s*\{[^{}]*"ip"\s*:\s*"(\d{1,3}(?:\.\d{1,3}){3})"',
      ).firstMatch(raw)?.group(1),
      RegExp(r'"ip"\s*:\s*"(\d{1,3}(?:\.\d{1,3}){3})"')
          .firstMatch(raw)
          ?.group(1),
    ];
    for (final c in candidates) {
      final ip = _validateIpv4(c);
      if (ip != null) return ip;
    }
    return null;
  }

  String? _validateIpv4(String? raw) {
    if (raw == null) return null;
    final ip = raw.trim();
    if (!_isUsableHost(ip)) return null;
    final parts = ip.split('.');
    if (parts.length != 4) return null;
    for (final p in parts) {
      final n = int.tryParse(p);
      if (n == null || n < 0 || n > 255) return null;
    }
    return ip;
  }

  String? _extractHttpHostFromNotifyBuffer(List<int> buffer) {
    if (buffer.isEmpty) return null;

    // Framed payload: 0x1E 0x1E + json + 0x1F
    const int fs = 0x1E;
    const int fe = 0x1F;
    int start = -1;
    for (int i = 0; i + 1 < buffer.length; i++) {
      if (buffer[i] == fs && buffer[i + 1] == fs) {
        start = i + 2;
        break;
      }
    }
    if (start < 0) return null;
    int end = -1;
    for (int i = start; i < buffer.length; i++) {
      if (buffer[i] == fe) {
        end = i;
        break;
      }
    }
    if (end <= start) return null;
    final framed = buffer.sublist(start, end);
    return _extractHttpHostFromBleBytes(framed);
  }

  Future<String?> _readHostFromNotify(
    BluetoothCharacteristic notifyChar,
    void Function(double progress, String status)? onProgress,
  ) async {
    final buffer = <int>[];
    StreamSubscription<List<int>>? sub;
    try {
      final completer = Completer<String?>();
      sub = notifyChar.onValueReceived.listen((chunk) {
        if (chunk.isEmpty || completer.isCompleted) return;
        buffer.addAll(chunk);
        // Keep only data after the latest frame-start marker.
        const int fs = 0x1E;
        int latestStart = -1;
        for (int i = 0; i + 1 < buffer.length; i++) {
          if (buffer[i] == fs && buffer[i + 1] == fs) {
            latestStart = i;
          }
        }
        if (latestStart > 0) {
          buffer.removeRange(0, latestStart);
        }
        // keep bounded buffer
        if (buffer.length > 4096) {
          buffer.removeRange(0, buffer.length - 4096);
        }
        final host = _extractHttpHostFromNotifyBuffer(buffer);
        if (host != null && !completer.isCompleted) {
          completer.complete(host);
        }
      });

      onProgress?.call(0.85, 'Lecture notifications BLE...');
      await notifyChar.setNotifyValue(true);
      final host = await completer.future.timeout(
        const Duration(seconds: 4),
        onTimeout: () => null,
      );
      return host;
    } catch (_) {
      return null;
    } finally {
      try {
        await notifyChar.setNotifyValue(false);
      } catch (_) {}
      await sub?.cancel();
    }
  }

  Future<String?> _readHttpHostFromBle(
    BleScanResult result, {
    void Function(double progress, String status)? onProgress,
  }) async {
    _lastBlePolicyHint = null;
    final device = result.device;
    final suffix = result.cardId;
    final serviceExpected = result.serviceUuid ?? bleUuidFor('01', suffix);
    final readUuid = bleUuidFor('04', suffix);
    final notifyUuid = bleUuidFor('03', suffix);
    bool connectedHere = false;

    try {
      onProgress?.call(0.10, 'Connexion à la carte...');
      try {
        await device.connect(
          timeout: const Duration(seconds: 8),
          autoConnect: false,
        );
        connectedHere = true;
      } catch (_) {
        // already connected or temporary connect race
      }

      try {
        await device.requestMtu(185);
      } catch (_) {
        // optional
      }

      onProgress?.call(0.25, 'Découverte des services BLE...');
      final services = await device.discoverServices();
      BluetoothService? service;
      for (final s in services) {
        if (s.uuid == serviceExpected) {
          service = s;
          break;
        }
      }
      service ??= services.isNotEmpty ? services.first : null;
      if (service == null) return null;

      BluetoothCharacteristic? readChar;
      BluetoothCharacteristic? notifyChar;
      for (final c in service.characteristics) {
        if (c.uuid == readUuid) readChar = c;
        if (c.uuid == notifyUuid) notifyChar = c;
      }
      if (readChar == null && notifyChar == null) return null;

      if (readChar != null) {
        // Firmware updates this characteristic every second after BLE connect.
        // Retry a few times to avoid getting "{}" too early.
        for (int attempt = 0; attempt < 6; attempt++) {
          onProgress?.call(
            0.40 + (attempt * 0.06),
            'Lecture état BLE (${attempt + 1}/6)...',
          );
          if (attempt > 0) {
            await Future<void>.delayed(const Duration(milliseconds: 650));
          }
          final value = await readChar.read();
          final host = _extractHttpHostFromBleBytes(value);
          if (host != null) return host;
        }
      }
      if (notifyChar != null) {
        onProgress?.call(0.80, 'Attente des données BLE...');
        final host = await _readHostFromNotify(
          notifyChar,
          onProgress,
        );
        if (host != null) return host;
      }
      return null;
    } catch (_) {
      return null;
    } finally {
      if (connectedHere) {
        try {
          await device.disconnect();
        } catch (_) {}
      }
    }
  }

  Uri _buildCardApiUri(String hostInput, String path) {
    final host = hostInput.trim();
    final normalizedPath = path.startsWith('/') ? path : '/$path';
    if (host.startsWith('http://') || host.startsWith('https://')) {
      final base = Uri.parse(host);
      return base.replace(path: normalizedPath);
    }
    return Uri.parse('http://$host$normalizedPath');
  }

  Future<Map<String, dynamic>> _getJsonFromCard({
    required String host,
    required String path,
    required String user,
    required String pass,
    bool useAuth = true,
  }) async {
    final uri = _buildCardApiUri(host, path);
    final headers = <String, String>{'Accept': 'application/json'};
    if (useAuth) {
      final token = base64Encode(utf8.encode('$user:$pass'));
      headers['Authorization'] = 'Basic $token';
    }

    final res = await http
        .get(uri, headers: headers)
        .timeout(const Duration(seconds: 4));
    if (res.statusCode != 200) {
      throw Exception('HTTP ${res.statusCode} sur $path');
    }
    final decoded = jsonDecode(res.body);
    if (decoded is! Map<String, dynamic>) {
      throw Exception('Réponse JSON invalide sur $path');
    }
    return decoded;
  }

  Future<Map<String, dynamic>> _putJsonToCard({
    required String host,
    required String path,
    required Map<String, dynamic> body,
    required String user,
    required String pass,
    bool useAuth = true,
  }) async {
    final uri = _buildCardApiUri(host, path);
    final headers = <String, String>{
      'Accept': 'application/json',
      'Content-Type': 'application/json',
    };
    if (useAuth) {
      final token = base64Encode(utf8.encode('$user:$pass'));
      headers['Authorization'] = 'Basic $token';
    }

    final res = await http
        .put(uri, headers: headers, body: jsonEncode(body))
        .timeout(const Duration(seconds: 5));
    if (res.statusCode != 200) {
      throw Exception('HTTP ${res.statusCode} sur $path');
    }
    final decoded = jsonDecode(res.body);
    if (decoded is! Map<String, dynamic>) {
      throw Exception('Réponse JSON invalide sur $path');
    }
    final ok = decoded['ok'];
    if (ok is bool && !ok) {
      throw Exception('Erreur carte sur $path: ${decoded['error'] ?? 'unknown'}');
    }
    return decoded;
  }

  bool _isNoRouteLikeError(Object e) {
    final s = e.toString().toLowerCase();
    return s.contains('no route to host') ||
        s.contains('network is unreachable') ||
        s.contains('failed host lookup');
  }

  bool _isUnauthorizedError(Object e) {
    final s = e.toString().toLowerCase();
    return s.contains('http 401');
  }

  String _hostOnly(String hostInput) {
    final host = hostInput.trim();
    if (host.isEmpty) return '';
    if (host.startsWith('http://') || host.startsWith('https://')) {
      final uri = Uri.tryParse(host);
      if (uri != null && uri.host.isNotEmpty) return uri.host;
    }
    final first = host.split('/').first;
    final idx = first.indexOf(':');
    if (idx > 0) return first.substring(0, idx);
    return first;
  }

  Future<({String host, Map<String, dynamic> state})> _loadStateWithFallback(
    String preferredHost,
  ) async {
    try {
      final state = await _getJsonFromCard(
        host: preferredHost,
        path: '/api/state',
        user: '',
        pass: '',
        useAuth: false,
      );
      return (host: preferredHost, state: state);
    } catch (e) {
      final hostOnly = _hostOnly(preferredHost);
      if (!_isNoRouteLikeError(e) || hostOnly == '192.168.4.1') rethrow;

      const fallbackHost = '192.168.4.1';
      final state = await _getJsonFromCard(
        host: fallbackHost,
        path: '/api/state',
        user: '',
        pass: '',
        useAuth: false,
      );
      _snack('IP LAN non joignable, bascule sur $fallbackHost');
      return (host: fallbackHost, state: state);
    }
  }

  String _friendlyImportError(Object e, String host) {
    final detail = e.toString();
    if (_isUnauthorizedError(e)) {
      return 'Import échoué: accès refusé (401). Vérifie User WEB / Password WEB (défaut: admin/admin). Détail: $detail';
    }
    if (_isNoRouteLikeError(e)) {
      return 'Import échoué: hôte $host non joignable. Vérifie VPN/réseau. Détail: $detail';
    }
    return 'Import échoué: $detail';
  }

  Future<bool> _warnIfVpnLikelyEnabled() async {
    final vpnLikely = await _isVpnLikelyEnabled();
    if (!vpnLikely || !mounted) return true;
    final proceed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('VPN détecté'),
        content: const Text(
          'Un VPN semble actif. Cela peut bloquer l’accès local à la carte (LAN). '
          'Désactive le VPN puis réessaie.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('Annuler'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('Continuer'),
          ),
        ],
      ),
    );
    return proceed == true;
  }

  Future<bool> _isVpnLikelyEnabled() async {
    if (kIsWeb) return false;
    try {
      final interfaces = await NetworkInterface.list(
        includeLoopback: false,
        type: InternetAddressType.any,
      );
      for (final itf in interfaces) {
        final n = itf.name.toLowerCase();
        if (n.startsWith('tun') ||
            n.startsWith('ppp') ||
            n.startsWith('wg') ||
            n.startsWith('utun') ||
            n.startsWith('ipsec') ||
            n.contains('vpn')) {
          return true;
        }
      }
    } catch (_) {
      // best-effort only
    }
    return false;
  }

  Future<String?> _askHostRetryDialog(String currentHost) async {
    final hostCtrl = TextEditingController(text: currentHost);
    final value = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('IP carte non joignable'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Align(
              alignment: Alignment.centerLeft,
              child: Text(
                'Saisir une autre IP/Host puis réessayer.',
              ),
            ),
            const SizedBox(height: 8),
            TextField(
              controller: hostCtrl,
              decoration: const InputDecoration(labelText: 'IP / Host carte'),
            ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(),
            child: const Text('Annuler'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(ctx).pop(hostCtrl.text.trim()),
            child: const Text('Réessayer'),
          ),
        ],
      ),
    );
    hostCtrl.dispose();
    if (value == null || value.trim().isEmpty) return null;
    return value.trim();
  }

  TransportPolicy _policyFromDeviceTransport(String raw) {
    switch (raw.trim().toLowerCase()) {
      case 'ethernet':
      case 'eth':
        return TransportPolicy.local;
      case 'gsm':
        return TransportPolicy.gsm;
      case 'auto':
        return TransportPolicy.auto;
      default:
        return _policy;
    }
  }

  Future<void> _importFromCardHttp() async {
    var host = _httpHostCtrl.text.trim();
    if (host.isEmpty) {
      _snack('Renseigne l’IP ou l’hôte HTTP de la carte.');
      return;
    }

    FocusScope.of(context).unfocus();
    setState(() => _importBusy = true);
    int authRetryCount = 0;
    try {
      while (true) {
        try {
          final stateFetch = await _loadStateWithFallback(host);
          host = stateFetch.host;
          final state = stateFetch.state;

          final stateDeviceId = EspCard.normalizeCardId(
            (state['device_id'] ?? '').toString(),
          );
          if (stateDeviceId.isEmpty || !EspCard.isValidCardId(stateDeviceId)) {
            throw Exception('Impossible de lire un device_id valide depuis /api/state');
          }

          final currentCard = EspCard.normalizeCardId(_cardIdCtrl.text);
          if (currentCard.isNotEmpty && currentCard != stateDeviceId) {
            throw Exception(
              'La carte trouvée ($stateDeviceId) ne correspond pas au Card ID saisi ($currentCard)',
            );
          }

          final authUser = _httpUserCtrl.text.trim();
          final authPass = _httpPassCtrl.text;
          await _getJsonFromCard(
            host: host,
            path: '/api/auth',
            user: authUser,
            pass: authPass,
            useAuth: true,
          );
          final mqttCfg = await _getJsonFromCard(
            host: host,
            path: '/api/mqtt',
            user: authUser,
            pass: authPass,
            useAuth: true,
          );

          setState(() {
            _httpHostCtrl.text = host;
            _cardIdCtrl.text = stateDeviceId;
            _policy =
                _policyFromDeviceTransport((mqttCfg['transport'] ?? 'auto').toString());
            _localHostCtrl.text = (mqttCfg['host'] ?? '').toString();
            _localPortCtrl.text = (mqttCfg['port'] ?? 1883).toString();
            _localUserCtrl.text = (mqttCfg['user'] ?? '').toString();
            _localPassCtrl.text = (mqttCfg['pass'] ?? '').toString();
            _gsmHostCtrl.text = (mqttCfg['gsm_mqtt_host'] ?? '').toString();
            _gsmPortCtrl.text = (mqttCfg['gsm_mqtt_port'] ?? 1883).toString();
            _gsmUserCtrl.text = (mqttCfg['gsm_mqtt_user'] ?? '').toString();
            _gsmPassCtrl.text = (mqttCfg['gsm_mqtt_pass'] ?? '').toString();
            _retain = ((mqttCfg['retain'] ?? 1).toString() == '1');
          });
          _snack('Paramétrage carte importé depuis HTTP.');
          break;
        } catch (e) {
          if (_isUnauthorizedError(e) && mounted) {
            authRetryCount++;
            if (authRetryCount > 3) {
              _snack(
                'Import échoué: 401 persistant. Vérifie les identifiants WEB de la carte.',
              );
              break;
            }
            _snack('401: identifiants WEB invalides ou non appliqués.');
            final retryAuth = await _askWebAccessDialog(host);
            if (retryAuth) {
              continue;
            }
          }
          if (_isNoRouteLikeError(e) && mounted) {
            final retryHost = await _askHostRetryDialog(host);
            if (retryHost != null &&
                retryHost.isNotEmpty &&
                retryHost.trim() != host.trim()) {
              host = retryHost.trim();
              _httpHostCtrl.text = host;
              continue;
            }
          }
          _snack(_friendlyImportError(e, host));
          break;
        }
      }
    } finally {
      if (mounted) setState(() => _importBusy = false);
    }
  }

  void _snack(String text) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text), duration: const Duration(seconds: 2)),
    );
  }

  Future<void> _save() async {
    if (_saveBusy) return;
    FocusScope.of(context).unfocus();
    final cardId = EspCard.normalizeCardId(_cardIdCtrl.text);
    final name = _nameCtrl.text.trim();
    final localPort = int.tryParse(_localPortCtrl.text.trim());
    final gsmPort = int.tryParse(_gsmPortCtrl.text.trim());

    if (!EspCard.isValidCardId(cardId)) {
      _snack('Card ID invalide (attendu: 12 hexa).');
      return;
    }
    if (name.isEmpty) {
      _snack('Le nom de la carte est requis.');
      return;
    }
    if (localPort == null || localPort <= 0 || localPort > 65535) {
      _snack('Port MQTT local invalide.');
      return;
    }
    if (gsmPort == null || gsmPort <= 0 || gsmPort > 65535) {
      _snack('Port MQTT GSM invalide.');
      return;
    }

    final card = EspCard(
      cardId: cardId,
      name: name,
      policy: _policy,
      httpHost: _httpHostCtrl.text.trim(),
      httpUser: _httpUserCtrl.text.trim(),
      httpPass: _httpPassCtrl.text,
      localHost: _localHostCtrl.text.trim(),
      localPort: localPort,
      localUser: _localUserCtrl.text.trim(),
      localPass: _localPassCtrl.text,
      gsmHost: _gsmHostCtrl.text.trim(),
      gsmPort: gsmPort,
      gsmUser: _gsmUserCtrl.text.trim(),
      gsmPass: _gsmPassCtrl.text,
      retain: _retain,
      updatedAtMs: DateTime.now().millisecondsSinceEpoch,
    );

    final host = _httpHostCtrl.text.trim();
    setState(() => _saveBusy = true);
    try {
      if (host.isNotEmpty) {
        String transportValue;
        switch (_policy) {
          case TransportPolicy.local:
            transportValue = 'ethernet';
            break;
          case TransportPolicy.gsm:
            transportValue = 'gsm';
            break;
          case TransportPolicy.auto:
          case TransportPolicy.ble:
            transportValue = 'auto';
            break;
        }

        await _putJsonToCard(
          host: host,
          path: '/api/mqtt',
          user: _httpUserCtrl.text.trim(),
          pass: _httpPassCtrl.text,
          useAuth: true,
          body: <String, dynamic>{
            'enabled': 1,
            'transport': transportValue,
            'host': _localHostCtrl.text.trim(),
            'port': localPort,
            'user': _localUserCtrl.text.trim(),
            'pass': _localPassCtrl.text,
            'gsm_mqtt_host': _gsmHostCtrl.text.trim(),
            'gsm_mqtt_port': gsmPort,
            'gsm_mqtt_user': _gsmUserCtrl.text.trim(),
            'gsm_mqtt_pass': _gsmPassCtrl.text,
            'retain': _retain ? 1 : 0,
          },
        );
      }

      if (!mounted) return;
      Navigator.of(context).pop(card);
    } catch (e) {
      _snack('Échec enregistrement carte: $e');
    } finally {
      if (mounted) setState(() => _saveBusy = false);
    }
  }

  Widget _field(
    String label,
    TextEditingController ctrl, {
    bool obscure = false,
    TextInputType? keyboardType,
  }) {
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

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(widget.initialCard == null ? 'Ajouter carte' : 'Modifier carte'),
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Card(
            elevation: 0,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(12),
              side: const BorderSide(color: Color(0xFFDADADA)),
            ),
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Identité carte',
                    style: TextStyle(fontSize: 16, fontWeight: FontWeight.w700),
                  ),
                  const SizedBox(height: 10),
                  _field('Nom carte', _nameCtrl),
                  _field('Card ID (MAC12 sans :)', _cardIdCtrl),
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    children: [
                      OutlinedButton.icon(
                        onPressed: _scanQr,
                        icon: const Icon(Icons.qr_code_scanner),
                        label: const Text('Scanner QR'),
                      ),
                      OutlinedButton.icon(
                        onPressed: _scanBle,
                        icon: const Icon(Icons.bluetooth_searching),
                        label: const Text('Scanner BLE'),
                      ),
                    ],
                  ),
                  if (_bleFetchBusy) ...[
                    const SizedBox(height: 10),
                    LinearProgressIndicator(
                      value: _bleFetchProgress <= 0 ? null : _bleFetchProgress,
                    ),
                    const SizedBox(height: 6),
                    Text(
                      _bleFetchStatus.isEmpty
                          ? 'Récupération des infos BLE...'
                          : _bleFetchStatus,
                      style: const TextStyle(fontSize: 12),
                    ),
                  ],
                ],
              ),
            ),
          ),
          const SizedBox(height: 10),
          Card(
            elevation: 0,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(12),
              side: const BorderSide(color: Color(0xFFDADADA)),
            ),
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Transport',
                    style: TextStyle(fontSize: 16, fontWeight: FontWeight.w700),
                  ),
                  const SizedBox(height: 8),
                  DropdownButtonFormField<TransportPolicy>(
                    initialValue: _policy,
                    isExpanded: true,
                    items: TransportPolicy.values
                        .map((p) => DropdownMenuItem(
                              value: p,
                              child: Text(
                                transportPolicyLabel(p),
                                overflow: TextOverflow.ellipsis,
                              ),
                            ))
                        .toList(),
                    selectedItemBuilder: (context) => TransportPolicy.values
                        .map(
                          (p) => Text(
                            transportPolicyCompactLabel(p),
                            overflow: TextOverflow.ellipsis,
                          ),
                        )
                        .toList(),
                    onChanged: (p) {
                      if (p == null) return;
                      setState(() => _policy = p);
                    },
                    decoration: const InputDecoration(
                      labelText: 'Politique transport',
                      filled: true,
                      fillColor: Colors.white,
                      border: OutlineInputBorder(),
                    ),
                  ),
                  const SizedBox(height: 10),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('MQTT retain'),
                    value: _retain,
                    onChanged: (v) => setState(() => _retain = v),
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 10),
          Card(
            elevation: 0,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(12),
              side: const BorderSide(color: Color(0xFFDADADA)),
            ),
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('MQTT Local',
                      style: TextStyle(fontSize: 16, fontWeight: FontWeight.w700)),
                  const SizedBox(height: 10),
                  _field('Broker local', _localHostCtrl),
                  _field('Port local', _localPortCtrl,
                      keyboardType: TextInputType.number),
                  _field('User local', _localUserCtrl),
                  _field('Pass local', _localPassCtrl, obscure: true),
                ],
              ),
            ),
          ),
          const SizedBox(height: 10),
          Card(
            elevation: 0,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(12),
              side: const BorderSide(color: Color(0xFFDADADA)),
            ),
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('MQTT GSM',
                      style: TextStyle(fontSize: 16, fontWeight: FontWeight.w700)),
                  const SizedBox(height: 10),
                  _field('Broker GSM', _gsmHostCtrl),
                  _field('Port GSM', _gsmPortCtrl,
                      keyboardType: TextInputType.number),
                  _field('User GSM', _gsmUserCtrl),
                  _field('Pass GSM', _gsmPassCtrl, obscure: true),
                ],
              ),
            ),
          ),
        ],
      ),
      bottomNavigationBar: SafeArea(
        minimum: const EdgeInsets.fromLTRB(12, 0, 12, 12),
        child: FilledButton.icon(
          onPressed: _saveBusy ? null : _save,
          icon: _saveBusy
              ? const SizedBox(
                  width: 16,
                  height: 16,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : const Icon(Icons.save),
          label: Text(_saveBusy ? 'Enregistrement...' : 'Enregistrer carte'),
        ),
      ),
    );
  }
}

class _WebAuthDialogResult {
  const _WebAuthDialogResult({required this.user, required this.pass});

  final String user;
  final String pass;
}

class _WebAuthDialog extends StatefulWidget {
  const _WebAuthDialog({
    required this.host,
    required this.initialUser,
    required this.initialPass,
  });

  final String host;
  final String initialUser;
  final String initialPass;

  @override
  State<_WebAuthDialog> createState() => _WebAuthDialogState();
}

class _WebAuthDialogState extends State<_WebAuthDialog> {
  late final TextEditingController _userCtrl;
  late final TextEditingController _passCtrl;
  bool _obscure = true;

  @override
  void initState() {
    super.initState();
    _userCtrl = TextEditingController(
      text: widget.initialUser.trim().isEmpty ? 'admin' : widget.initialUser,
    );
    _passCtrl = TextEditingController(
      text: widget.initialPass.isEmpty ? 'admin' : widget.initialPass,
    );
  }

  @override
  void dispose() {
    _userCtrl.dispose();
    _passCtrl.dispose();
    super.dispose();
  }

  void _submit() {
    Navigator.of(context).pop(
      _WebAuthDialogResult(
        user: _userCtrl.text,
        pass: _passCtrl.text,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('Accès Web carte'),
      content: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Align(
              alignment: Alignment.centerLeft,
              child: Text(
                'IP / Host carte: ${widget.host}',
                style: const TextStyle(
                  fontFamily: 'monospace',
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
            const SizedBox(height: 8),
            TextField(
              controller: _userCtrl,
              textInputAction: TextInputAction.next,
              textCapitalization: TextCapitalization.none,
              autocorrect: false,
              enableSuggestions: false,
              decoration: const InputDecoration(labelText: 'User WEB'),
            ),
            const SizedBox(height: 8),
            TextField(
              controller: _passCtrl,
              textInputAction: TextInputAction.done,
              onSubmitted: (_) => _submit(),
              textCapitalization: TextCapitalization.none,
              autocorrect: false,
              enableSuggestions: false,
              obscureText: _obscure,
              decoration: InputDecoration(
                labelText: 'Password WEB',
                suffixIcon: IconButton(
                  onPressed: () => setState(() => _obscure = !_obscure),
                  icon: Icon(
                    _obscure ? Icons.visibility : Icons.visibility_off,
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('Annuler'),
        ),
        FilledButton(
          onPressed: _submit,
          child: const Text('Importer'),
        ),
      ],
    );
  }
}

class CardDetailPage extends StatefulWidget {
  const CardDetailPage({
    super.key,
    required this.card,
    required this.repository,
  });

  final EspCard card;
  final CardRepository repository;

  @override
  State<CardDetailPage> createState() => _CardDetailPageState();
}

class _CardDetailPageState extends State<CardDetailPage> {
  late EspCard _card;
  late CardSession _session;
  late final PageController _detailsPageCtrl;
  int _detailsPage = 0;
  bool _bleConnectSyncBusy = false;
  String _bleDetectedMode = '--';

  @override
  void initState() {
    super.initState();
    _card = widget.card.copy();
    _session = CardSession(_card)..addListener(_onSession);
    _detailsPageCtrl = PageController();
  }

  @override
  void dispose() {
    _session.removeListener(_onSession);
    _session.dispose();
    _detailsPageCtrl.dispose();
    super.dispose();
  }

  void _onSession() {
    if (!mounted) return;
    setState(() {});
  }

  void _snack(String text) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text), duration: const Duration(seconds: 2)),
    );
  }

  Map<String, dynamic>? _parseBleStateJson(String raw) {
    try {
      final obj = jsonDecode(raw);
      if (obj is Map<String, dynamic>) return obj;
    } catch (_) {}
    final start = raw.indexOf('\u001E\u001E');
    if (start < 0) return null;
    final end = raw.indexOf('\u001F', start + 2);
    if (end <= start + 2) return null;
    final payload = raw.substring(start + 2, end);
    try {
      final obj = jsonDecode(payload);
      if (obj is Map<String, dynamic>) return obj;
    } catch (_) {}
    return null;
  }

  bool _bleFieldToBool(dynamic value) {
    final lower = value?.toString().trim().toLowerCase() ?? '';
    return lower == '1' || lower == 'true' || lower == 'on' || lower == 'yes';
  }

  TransportPolicy? _policyFromBleStateJson(Map<String, dynamic> parsed) {
    final eth = parsed['eth'];
    final gsm = parsed['gsm'];
    final ethLink = (eth is Map) ? _bleFieldToBool(eth['link']) : false;
    final gsmReady = (gsm is Map)
        ? (_bleFieldToBool(gsm['ready']) ||
            (_bleFieldToBool(gsm['network']) && _bleFieldToBool(gsm['data'])))
        : false;
    final mqtt = parsed['mqtt'];
    if (mqtt is! Map) {
      if (!ethLink && gsmReady) return TransportPolicy.gsm;
      if (ethLink && !gsmReady) return TransportPolicy.local;
      return null;
    }
    final active = (mqtt['active_transport'] ?? '').toString().trim().toLowerCase();
    final activeHasEth = active.contains('ethernet') || active.contains('eth');
    final activeHasGsm = active.contains('gsm');
    final onGsm = _bleFieldToBool(mqtt['on_gsm']);
    final ethConnected = _bleFieldToBool(mqtt['eth_connected']);
    final gsmConnected =
        _bleFieldToBool(mqtt['gsm_connected']) || onGsm;
    // Ethernet link physically down: prioritize GSM, even if stale ETH MQTT flags remain.
    if (!ethLink && (gsmReady || gsmConnected || activeHasGsm)) {
      return TransportPolicy.gsm;
    }
    if (activeHasEth && activeHasGsm) {
      return ethLink ? TransportPolicy.local : TransportPolicy.gsm;
    }
    if (active == 'ethernet' || active == 'eth' || active == 'local' || activeHasEth) {
      return TransportPolicy.local;
    }
    if (active == 'gsm' || activeHasGsm) return TransportPolicy.gsm;
    if (ethConnected && !gsmConnected) return TransportPolicy.local;
    if (gsmConnected) return TransportPolicy.gsm;
    if (!ethLink && gsmReady) return TransportPolicy.gsm;
    if (ethLink && !gsmReady) return TransportPolicy.local;
    return null;
  }

  Map<String, dynamic>? _extractBleStateFromBytes(List<int> value) {
    if (value.isEmpty) return null;
    final txt = utf8.decode(value, allowMalformed: true);
    return _parseBleStateJson(txt);
  }

  Map<String, dynamic>? _extractBleStateFromNotifyBuffer(List<int> buffer) {
    if (buffer.isEmpty) return null;
    const int fs = 0x1E;
    const int fe = 0x1F;
    int start = -1;
    for (int i = 0; i + 1 < buffer.length; i++) {
      if (buffer[i] == fs && buffer[i + 1] == fs) {
        start = i + 2;
        break;
      }
    }
    if (start < 0) return null;
    int end = -1;
    for (int i = start; i < buffer.length; i++) {
      if (buffer[i] == fe) {
        end = i;
        break;
      }
    }
    if (end <= start) return null;
    return _extractBleStateFromBytes(buffer.sublist(start, end));
  }

  Future<Map<String, dynamic>?> _readStateFromNotify(
    BluetoothCharacteristic notifyChar,
  ) async {
    final buffer = <int>[];
    StreamSubscription<List<int>>? sub;
    try {
      final completer = Completer<Map<String, dynamic>?>();
      sub = notifyChar.onValueReceived.listen((chunk) {
        if (chunk.isEmpty || completer.isCompleted) return;
        buffer.addAll(chunk);
        const int fs = 0x1E;
        int latestStart = -1;
        for (int i = 0; i + 1 < buffer.length; i++) {
          if (buffer[i] == fs && buffer[i + 1] == fs) {
            latestStart = i;
          }
        }
        if (latestStart > 0) {
          buffer.removeRange(0, latestStart);
        }
        if (buffer.length > 4096) {
          buffer.removeRange(0, buffer.length - 4096);
        }
        final state = _extractBleStateFromNotifyBuffer(buffer);
        if (state != null && !completer.isCompleted) {
          completer.complete(state);
        }
      });
      await notifyChar.setNotifyValue(true);
      return await completer.future.timeout(
        const Duration(seconds: 4),
        onTimeout: () => null,
      );
    } catch (_) {
      return null;
    } finally {
      try {
        await notifyChar.setNotifyValue(false);
      } catch (_) {}
      await sub?.cancel();
    }
  }

  Future<Map<String, dynamic>?> _readBleStateFromBle(BleScanResult result) async {
    final device = result.device;
    final suffix = result.cardId;
    final serviceExpected = result.serviceUuid ?? bleUuidFor('01', suffix);
    final readUuid = bleUuidFor('04', suffix);
    final notifyUuid = bleUuidFor('03', suffix);
    bool connectedHere = false;

    try {
      try {
        await device.connect(
          timeout: const Duration(seconds: 8),
          autoConnect: false,
        );
        connectedHere = true;
      } catch (_) {}

      try {
        await device.requestMtu(185);
      } catch (_) {}

      final services = await device.discoverServices();
      BluetoothService? service;
      for (final s in services) {
        if (s.uuid == serviceExpected) {
          service = s;
          break;
        }
      }
      service ??= services.isNotEmpty ? services.first : null;
      if (service == null) return null;

      BluetoothCharacteristic? readChar;
      BluetoothCharacteristic? notifyChar;
      for (final c in service.characteristics) {
        if (c.uuid == readUuid) {
          readChar = c;
        }
        if (c.uuid == notifyUuid) notifyChar = c;
      }
      if (readChar != null) {
        for (int attempt = 0; attempt < 6; attempt++) {
          if (attempt > 0) {
            await Future<void>.delayed(const Duration(milliseconds: 650));
          }
          final value = await readChar.read();
          final state = _extractBleStateFromBytes(value);
          if (state != null) return state;
        }
      }
      if (notifyChar != null) {
        final state = await _readStateFromNotify(notifyChar);
        if (state != null) return state;
      }
      return null;
    } catch (_) {
      return null;
    } finally {
      if (connectedHere) {
        try {
          await device.disconnect();
        } catch (_) {}
      }
    }
  }

  Future<void> _connectWithBleSync() async {
    if (_bleConnectSyncBusy || _session.connecting) return;
    setState(() => _bleConnectSyncBusy = true);
    try {
      bool preferGsmFirst = false;
      final scan = await Navigator.of(context).push<BleScanResult>(
        MaterialPageRoute(
          builder: (_) => BleScannerPage(expectedCardId: _card.cardId),
        ),
      );
      if (!mounted) return;
      if (scan != null) {
        _session.setBleAvailable(true);
        final bleState = await _readBleStateFromBle(scan);
        final blePolicy =
            (bleState == null) ? null : _policyFromBleStateJson(bleState);
        if (!mounted) return;
        setState(() {
          _bleDetectedMode = blePolicy == null
              ? 'indéterminé'
              : transportPolicyCompactLabel(blePolicy);
        });
        if (blePolicy != null) {
          if (blePolicy == TransportPolicy.gsm &&
              (_card.policy == TransportPolicy.local ||
                  _card.policy == TransportPolicy.auto)) {
            preferGsmFirst = true;
          }
          _snack('Mode MQTT BLE: ${transportPolicyCompactLabel(blePolicy)}');
        }
      }
      if (!mounted) return;
      await _session.connect(
        preferGsmFirst: preferGsmFirst,
        allowLocalRecovery: true,
      );
    } finally {
      if (mounted) setState(() => _bleConnectSyncBusy = false);
    }
  }

  Future<void> _editCard() async {
    final updated = await Navigator.of(context).push<EspCard>(
      MaterialPageRoute(builder: (_) => CardEditPage(initialCard: _card)),
    );
    if (updated == null) return;
    await widget.repository.upsert(updated);
    _session.removeListener(_onSession);
    _session.dispose();
    setState(() {
      _card = updated.copy();
      _session = CardSession(_card)..addListener(_onSession);
    });
  }

  Widget _statusChip({
    required String text,
    required bool on,
  }) {
    final bg = on ? const Color(0xFFE3F2FD) : const Color(0xFFF3F3F3);
    final fg = on ? const Color(0xFF0D47A1) : Colors.black87;
    final border = on ? const Color(0xFF64B5F6) : const Color(0xFFDADADA);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: bg,
        border: Border.all(color: border),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(text,
          style: TextStyle(color: fg, fontWeight: FontWeight.w600, fontSize: 12)),
    );
  }

  Widget _buildRelayRows() {
    final rows = <Widget>[];
    for (int i = 1; i <= 4; i++) {
      final on = _session.relayStates[i] ?? false;
      final mode = _session.relayModes[i] ?? 'AUTO';
      rows.add(
        Container(
          margin: const EdgeInsets.only(bottom: 8),
          padding: const EdgeInsets.all(8),
          decoration: BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.circular(10),
            border: Border.all(color: const Color(0xFFDADADA)),
          ),
          child: Column(
            children: [
              Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    crossAxisAlignment: WrapCrossAlignment.center,
                    children: [
                      Text('Relais $i',
                          style: const TextStyle(fontWeight: FontWeight.w700)),
                      _statusChip(text: on ? 'ON' : 'OFF', on: on),
                    ],
                  ),
                  const SizedBox(height: 4),
                  Align(
                    alignment: Alignment.centerRight,
                    child: Text(
                      mode,
                      style: const TextStyle(
                          fontSize: 12,
                          fontWeight: FontWeight.w600,
                          color: Colors.black54),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              Row(
                children: [
                  Expanded(
                    child: FilledButton.tonal(
                      onPressed: _session.isConnected
                          ? () => _session.publishRelaySet(i, 'ON')
                          : null,
                      child: const Text('ON'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: FilledButton.tonal(
                      onPressed: _session.isConnected
                          ? () => _session.publishRelaySet(i, 'OFF')
                          : null,
                      child: const Text('OFF'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: OutlinedButton(
                      onPressed: _session.isConnected
                          ? () => _session.publishRelaySet(i, 'AUTO')
                          : null,
                      child: const Text('AUTO'),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      );
    }
    return Column(children: rows);
  }

  Widget _buildInputRows() {
    final rows = <Widget>[];
    for (int i = 1; i <= 4; i++) {
      final on = _session.inputStates[i] ?? false;
      final vin = _session.vinStates[i] ?? false;
      rows.add(
        Container(
          margin: const EdgeInsets.only(bottom: 8),
          padding: const EdgeInsets.all(8),
          decoration: BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.circular(10),
            border: Border.all(color: const Color(0xFFDADADA)),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text('Entrée $i', style: const TextStyle(fontWeight: FontWeight.w700)),
              const SizedBox(height: 6),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  _statusChip(text: on ? 'IN ON' : 'IN OFF', on: on),
                  _statusChip(text: vin ? 'VIN ON' : 'VIN OFF', on: vin),
                ],
              ),
              const SizedBox(height: 8),
              Row(
                children: [
                  Expanded(
                    child: FilledButton.tonal(
                      onPressed: _session.isConnected
                          ? () => _session.publishVinSet(i, 'ON')
                          : null,
                      child: const Text('VIN ON'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: FilledButton.tonal(
                      onPressed: _session.isConnected
                          ? () => _session.publishVinSet(i, 'OFF')
                          : null,
                      child: const Text('VIN OFF'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: OutlinedButton(
                      onPressed: _session.isConnected
                          ? () => _session.publishVinSet(i, 'TOGGLE')
                          : null,
                      child: const Text('TOGGLE'),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      );
    }
    return Column(children: rows);
  }

  Widget _section({required String title, required Widget child}) {
    return Card(
      elevation: 0,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: const BorderSide(color: Color(0xFFDADADA)),
      ),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(title,
                style: const TextStyle(fontWeight: FontWeight.w700, fontSize: 16)),
            const SizedBox(height: 10),
            child,
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final root = _card.baseTopic;
    return Scaffold(
      appBar: AppBar(
        title: Text(_card.name),
        actions: [
          IconButton(
            tooltip: 'Modifier carte',
            onPressed: _editCard,
            icon: const Icon(Icons.edit_outlined),
          ),
        ],
      ),
      body: Column(
        children: [
          Expanded(
            child: PageView(
              controller: _detailsPageCtrl,
              onPageChanged: (idx) => setState(() => _detailsPage = idx),
              children: [
                ListView(
                  padding: const EdgeInsets.all(14),
                  children: [
                    _section(
                      title: 'Connectivité',
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          SelectableText(
                            _card.cardId,
                            style: const TextStyle(
                              fontFamily: 'monospace',
                              fontWeight: FontWeight.w700,
                            ),
                          ),
                          const SizedBox(height: 8),
                          Wrap(
                            spacing: 8,
                            runSpacing: 8,
                            children: [
                              _statusChip(text: 'Policy ${_card.policy.name}', on: true),
                              _statusChip(
                                text: 'Active ${activeTransportLabel(_session.activeTransport)}',
                                on: _session.isConnected,
                              ),
                              _statusChip(text: 'MQTT Local', on: _session.localConnected),
                              _statusChip(text: 'MQTT GSM', on: _session.gsmConnected),
                              _statusChip(text: 'BLE', on: _session.bleAvailable),
                            ],
                          ),
                          const SizedBox(height: 8),
                          Text(_session.statusText,
                              style: const TextStyle(fontWeight: FontWeight.w600)),
                          const SizedBox(height: 10),
                          Wrap(
                            spacing: 8,
                            runSpacing: 8,
                            children: [
                              FilledButton.icon(
                                onPressed: (_session.connecting || _bleConnectSyncBusy)
                                    ? null
                                    : _connectWithBleSync,
                                icon: _bleConnectSyncBusy
                                    ? const SizedBox(
                                        width: 16,
                                        height: 16,
                                        child: CircularProgressIndicator(strokeWidth: 2),
                                      )
                                    : const Icon(Icons.play_arrow),
                                label: Text(_bleConnectSyncBusy ? 'Sync BLE...' : 'Connecter'),
                              ),
                              OutlinedButton.icon(
                                onPressed: _session.disconnect,
                                icon: const Icon(Icons.stop),
                                label: const Text('Déconnecter'),
                              ),
                              OutlinedButton.icon(
                                onPressed: () async {
                                  final messenger = ScaffoldMessenger.of(context);
                                  final result = await Navigator.of(context)
                                      .push<BleScanResult>(
                                    MaterialPageRoute(
                                      builder: (_) => BleScannerPage(
                                        expectedCardId: _card.cardId,
                                      ),
                                    ),
                                  );
                                  if (!mounted || result == null) return;
                                  _session.setBleAvailable(true);
                                  messenger.showSnackBar(
                                    const SnackBar(
                                      content: Text('Carte détectée via BLE.'),
                                    ),
                                  );
                                },
                                icon: const Icon(Icons.bluetooth_searching),
                                label: const Text('Tester BLE'),
                              ),
                            ],
                          ),
                          const SizedBox(height: 10),
                          Text('IP: ${_session.ethIp}    ICCID: ${_session.iccid}'),
                          const SizedBox(height: 6),
                          Text(
                            'Mode détecté via BLE: $_bleDetectedMode',
                            style: const TextStyle(fontSize: 12, color: Colors.black54),
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: 10),
                    _section(title: 'Relais', child: _buildRelayRows()),
                    const SizedBox(height: 10),
                    _section(title: 'Entrées', child: _buildInputRows()),
                  ],
                ),
                ListView(
                  padding: const EdgeInsets.all(14),
                  children: [
                    _section(
                      title: 'Topics MQTT',
                      child: SelectableText(
                        'Base: $root\n\n'
                        'Commandes:\n'
                        '  $root/relay/1/set  payload ON|OFF|AUTO\n'
                        '  $root/vin/1/set    payload ON|OFF|TOGGLE\n\n'
                        'Etats:\n'
                        '  $root/status\n'
                        '  $root/net/ip\n'
                        '  $root/gsm/iccid\n'
                        '  $root/relay/1/state\n'
                        '  $root/relay/1/mode\n'
                        '  $root/input/1/state\n'
                        '  $root/vin/1/state',
                        style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          SafeArea(
            top: false,
            minimum: const EdgeInsets.fromLTRB(14, 0, 14, 10),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Container(
                  width: 8,
                  height: 8,
                  decoration: BoxDecoration(
                    color: _detailsPage == 0
                        ? const Color(0xFF1565C0)
                        : const Color(0xFFB0BEC5),
                    shape: BoxShape.circle,
                  ),
                ),
                const SizedBox(width: 8),
                Container(
                  width: 8,
                  height: 8,
                  decoration: BoxDecoration(
                    color: _detailsPage == 1
                        ? const Color(0xFF1565C0)
                        : const Color(0xFFB0BEC5),
                    shape: BoxShape.circle,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class CardSession extends ChangeNotifier {
  CardSession(this.card) {
    for (int i = 1; i <= 4; i++) {
      relayStates[i] = false;
      relayModes[i] = 'AUTO';
      inputStates[i] = false;
      vinStates[i] = false;
    }
  }

  final EspCard card;
  MqttServerClient? _clientLocal;
  MqttServerClient? _clientGsm;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage>>>? _subLocal;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage>>>? _subGsm;
  Timer? _retryTimer;
  bool _disposed = false;
  bool _manualDisconnectInProgress = false;
  bool _fallbackRunning = false;
  bool _allowLocalRecovery = false;
  bool _localCardOnline = false;

  bool connecting = false;
  bool localConnected = false;
  bool gsmConnected = false;
  bool bleAvailable = false;
  ActiveTransport activeTransport = ActiveTransport.none;
  String statusText = 'Disconnected';
  String ethIp = '--';
  String iccid = '--';
  bool boardOnline = false;

  final Map<int, bool> relayStates = <int, bool>{};
  final Map<int, String> relayModes = <int, String>{};
  final Map<int, bool> inputStates = <int, bool>{};
  final Map<int, bool> vinStates = <int, bool>{};

  bool get isConnected => localConnected || gsmConnected;

  void _notifySafely() {
    if (_disposed) return;
    notifyListeners();
  }

  void setBleAvailable(bool value) {
    if (_disposed) return;
    bleAvailable = value;
    if (!isConnected && card.policy == TransportPolicy.ble) {
      activeTransport = ActiveTransport.ble;
    }
    _notifySafely();
  }

  bool _isClientConnected(MqttServerClient? client) {
    return client?.connectionStatus?.state == MqttConnectionState.connected;
  }

  void _refreshConnectedFlags() {
    localConnected = _isClientConnected(_clientLocal);
    gsmConnected = _isClientConnected(_clientGsm);
    if (activeTransport == ActiveTransport.local && !localConnected) {
      if (gsmConnected) {
        activeTransport = ActiveTransport.gsm;
      } else {
        activeTransport = bleAvailable ? ActiveTransport.ble : ActiveTransport.none;
      }
    } else if (activeTransport == ActiveTransport.gsm && !gsmConnected) {
      if (localConnected) {
        activeTransport = ActiveTransport.local;
      } else {
        activeTransport = bleAvailable ? ActiveTransport.ble : ActiveTransport.none;
      }
    }
  }

  Future<void> _tryGsmFallbackNow({bool silent = false}) async {
    if (_disposed || _manualDisconnectInProgress || _fallbackRunning) return;
    if (card.gsmHost.trim().isEmpty) return;
    if (gsmConnected) {
      activeTransport = ActiveTransport.gsm;
      if (!silent) statusText = 'Connected via GSM MQTT';
      _notifySafely();
      return;
    }
    _fallbackRunning = true;
    try {
      final ok = await _connectGsm(silent: true);
      if (ok) {
        activeTransport = ActiveTransport.gsm;
        if (!silent) statusText = 'Connected via GSM MQTT';
      } else if (!localConnected) {
        activeTransport = bleAvailable ? ActiveTransport.ble : ActiveTransport.none;
        if (!silent) statusText = 'No transport connected';
      }
      _notifySafely();
    } finally {
      _fallbackRunning = false;
    }
  }

  Future<void> connect({
    bool preferGsmFirst = false,
    bool allowLocalRecovery = false,
  }) async {
    if (_disposed) return;
    if (connecting) return;
    _allowLocalRecovery = allowLocalRecovery;
    connecting = true;
    statusText = 'Connecting...';
    _notifySafely();

    await disconnect(silent: true);

    bool ok = false;
    switch (card.policy) {
      case TransportPolicy.local:
        if (preferGsmFirst && card.gsmHost.trim().isNotEmpty) {
          ok = await _connectGsm();
          if (!ok) ok = await _connectLocal();
        } else {
          ok = await _connectLocal();
          if (!ok) ok = await _connectGsm();
        }
        break;
      case TransportPolicy.gsm:
        ok = await _connectGsm();
        break;
      case TransportPolicy.ble:
        activeTransport = ActiveTransport.ble;
        statusText = bleAvailable ? 'BLE available' : 'BLE mode (scan required)';
        ok = bleAvailable;
        break;
      case TransportPolicy.auto:
        if (preferGsmFirst && card.gsmHost.trim().isNotEmpty) {
          ok = await _connectGsm();
          if (!ok) ok = await _connectLocal();
        } else {
          ok = await _connectLocal();
          if (!ok) ok = await _connectGsm();
        }
        if (!ok && bleAvailable) {
          activeTransport = ActiveTransport.ble;
          statusText = 'AUTO fallback BLE';
        }
        break;
    }

    if (!ok && card.policy != TransportPolicy.ble && !bleAvailable) {
      statusText = 'No transport connected';
    }

    connecting = false;
    _refreshConnectedFlags();
    _notifySafely();

    _retryTimer?.cancel();
    if (card.policy == TransportPolicy.auto ||
        card.policy == TransportPolicy.local ||
        _allowLocalRecovery) {
      _retryTimer = Timer.periodic(const Duration(seconds: 12), (_) async {
        if (_disposed) return;
        if (connecting) return;
        _refreshConnectedFlags();
        if (localConnected && activeTransport == ActiveTransport.local) return;
        if (gsmConnected && activeTransport == ActiveTransport.gsm) {
          final localBack = await _connectLocal(silent: true);
          if (localBack) {
            // Only switch when local status confirms card online.
            if (_localCardOnline) {
              await _disconnectGsm();
              activeTransport = ActiveTransport.local;
              statusText = card.policy == TransportPolicy.local
                  ? 'Switched back to local MQTT'
                  : 'AUTO switched back to Local';
              _notifySafely();
            }
          }
          return;
        }
        if (!isConnected) {
          final localOk = await _connectLocal(silent: true);
          if (!localOk &&
              (card.policy == TransportPolicy.auto ||
                  card.policy == TransportPolicy.local ||
                  _allowLocalRecovery)) {
            await _tryGsmFallbackNow(silent: true);
          }
          _refreshConnectedFlags();
          _notifySafely();
        }
      });
    }
  }

  Future<bool> _connectLocal({bool silent = false}) async {
    final host = card.localHost.trim();
    if (host.isEmpty) return false;

    final client = MqttServerClient.withPort(
      host,
      '${card.cardId}-app-local',
      card.localPort,
    );
    _configureClient(client, source: ActiveTransport.local);

    try {
      final result = await client.connect(
        card.localUser.isEmpty ? null : card.localUser,
        card.localUser.isEmpty ? null : card.localPass,
      );
      if (result?.state != MqttConnectionState.connected) {
        client.disconnect();
        return false;
      }
      _clientLocal = client;
      localConnected = true;
      activeTransport = ActiveTransport.local;
      if (!silent) statusText = 'Connected via local MQTT';
      _listenClient(client, source: 'local');
      _subscribe(client);
      _notifySafely();
      return true;
    } catch (_) {
      client.disconnect();
      return false;
    }
  }

  Future<bool> _connectGsm({bool silent = false}) async {
    final host = card.gsmHost.trim();
    if (host.isEmpty) return false;

    final client = MqttServerClient.withPort(
      host,
      '${card.cardId}-app-gsm',
      card.gsmPort,
    );
    _configureClient(client, source: ActiveTransport.gsm);

    try {
      final result = await client.connect(
        card.gsmUser.isEmpty ? null : card.gsmUser,
        card.gsmUser.isEmpty ? null : card.gsmPass,
      );
      if (result?.state != MqttConnectionState.connected) {
        client.disconnect();
        return false;
      }
      _clientGsm = client;
      gsmConnected = true;
      activeTransport = ActiveTransport.gsm;
      if (!silent) statusText = 'Connected via GSM MQTT';
      _listenClient(client, source: 'gsm');
      _subscribe(client);
      _notifySafely();
      return true;
    } catch (_) {
      client.disconnect();
      return false;
    }
  }

  void _configureClient(MqttServerClient client, {required ActiveTransport source}) {
    client.logging(on: false);
    client.keepAlivePeriod = 45;
    client.autoReconnect = true;
    client.resubscribeOnAutoReconnect = false;
    client.connectTimeoutPeriod = 3500;
    client.onConnected = () {
      if (_disposed) return;
      if (source == ActiveTransport.local) {
        localConnected = true;
        if (!(_allowLocalRecovery && gsmConnected)) {
          activeTransport = ActiveTransport.local;
        }
      } else if (source == ActiveTransport.gsm) {
        gsmConnected = true;
        if (!localConnected || card.policy == TransportPolicy.gsm) {
          activeTransport = ActiveTransport.gsm;
        }
      }
      if (activeTransport == ActiveTransport.local) {
        statusText = 'Connected via local MQTT';
      } else if (activeTransport == ActiveTransport.gsm) {
        statusText = 'Connected via GSM MQTT';
      }
      _notifySafely();
    };
    client.onDisconnected = () {
      if (_disposed) return;
      if (source == ActiveTransport.local) {
        localConnected = false;
      } else if (source == ActiveTransport.gsm) {
        gsmConnected = false;
      }
      _refreshConnectedFlags();
      if (!_manualDisconnectInProgress &&
          source == ActiveTransport.local &&
          (card.policy == TransportPolicy.auto ||
              card.policy == TransportPolicy.local ||
              _allowLocalRecovery)) {
        unawaited(_tryGsmFallbackNow(silent: true));
      }
      if (!isConnected &&
          card.policy != TransportPolicy.ble &&
          activeTransport != ActiveTransport.ble) {
        statusText = 'No transport connected';
      }
      _notifySafely();
    };
    client.connectionMessage = MqttConnectMessage()
        .withClientIdentifier(client.clientIdentifier)
        .startClean()
        .withWillQos(MqttQos.atMostOnce)
        .withWillTopic('${card.baseTopic}/status')
        .withWillMessage('offline');
  }

  void _subscribe(MqttServerClient client) {
    final b = card.baseTopic;
    client.subscribe('$b/status', MqttQos.atLeastOnce);
    client.subscribe('$b/net/ip', MqttQos.atLeastOnce);
    client.subscribe('$b/gsm/iccid', MqttQos.atLeastOnce);
    for (int i = 1; i <= 4; i++) {
      client.subscribe('$b/relay/$i/state', MqttQos.atLeastOnce);
      client.subscribe('$b/relay/$i/mode', MqttQos.atLeastOnce);
      client.subscribe('$b/input/$i/state', MqttQos.atLeastOnce);
      client.subscribe('$b/vin/$i/state', MqttQos.atLeastOnce);
    }
  }

  void _listenClient(MqttServerClient client, {required String source}) {
    final sub = client.updates?.listen((events) {
      for (final e in events) {
        final rec = e.payload as MqttPublishMessage;
        final payload =
            MqttPublishPayload.bytesToStringAsString(rec.payload.message).trim();
        _handleMessage(e.topic, payload, source: source);
      }
    });
    if (source == 'local') {
      _subLocal?.cancel();
      _subLocal = sub;
    } else {
      _subGsm?.cancel();
      _subGsm = sub;
    }
  }

  void _handleMessage(String fullTopic, String payload, {required String source}) {
    if (_disposed) return;
    final prefix = '${card.baseTopic}/';
    if (!fullTopic.startsWith(prefix)) return;
    final sub = fullTopic.substring(prefix.length);

    if (sub == 'status') {
      final online = payload.toLowerCase() == 'online' || payload == '1';
      boardOnline = online;
      if (source == 'local') {
        _localCardOnline = online;
        if (online) {
          activeTransport = ActiveTransport.local;
          statusText = 'Connected via local MQTT';
        } else {
          if (!_manualDisconnectInProgress && localConnected) {
            unawaited(_disconnectLocal());
          }
          if (gsmConnected) {
            activeTransport = ActiveTransport.gsm;
            statusText = 'Connected via GSM MQTT';
          } else if (card.policy != TransportPolicy.ble) {
            statusText = 'Card offline on local MQTT';
          }
          if (!_manualDisconnectInProgress &&
              (card.policy == TransportPolicy.auto ||
                  card.policy == TransportPolicy.local ||
                  _allowLocalRecovery)) {
            unawaited(_tryGsmFallbackNow(silent: true));
          }
        }
      } else {
        if (online) {
          activeTransport = ActiveTransport.gsm;
          statusText = 'Connected via GSM MQTT';
        } else {
          if (!_manualDisconnectInProgress && gsmConnected) {
            unawaited(_disconnectGsm());
          }
          if (localConnected) {
            activeTransport = ActiveTransport.local;
            statusText = 'Connected via local MQTT';
          } else if (card.policy != TransportPolicy.ble) {
            statusText = 'Card offline on GSM MQTT';
          }
        }
      }
      _notifySafely();
      return;
    }
    if (sub == 'net/ip') {
      ethIp = payload.isEmpty ? '--' : payload;
      _notifySafely();
      return;
    }
    if (sub == 'gsm/iccid') {
      iccid = payload.isEmpty ? '--' : payload;
      _notifySafely();
      return;
    }

    final relayState = RegExp(r'^relay/(\d+)/state$').firstMatch(sub);
    if (relayState != null) {
      final idx = int.tryParse(relayState.group(1)!) ?? 0;
      if (idx >= 1 && idx <= 4) {
        relayStates[idx] = _toBoolPayload(payload);
        _notifySafely();
      }
      return;
    }

    final relayMode = RegExp(r'^relay/(\d+)/mode$').firstMatch(sub);
    if (relayMode != null) {
      final idx = int.tryParse(relayMode.group(1)!) ?? 0;
      if (idx >= 1 && idx <= 4) {
        relayModes[idx] = _toRelayMode(payload);
        _notifySafely();
      }
      return;
    }

    final inputState = RegExp(r'^input/(\d+)/state$').firstMatch(sub);
    if (inputState != null) {
      final idx = int.tryParse(inputState.group(1)!) ?? 0;
      if (idx >= 1 && idx <= 4) {
        inputStates[idx] = _toBoolPayload(payload);
        _notifySafely();
      }
      return;
    }

    final vinState = RegExp(r'^vin/(\d+)/state$').firstMatch(sub);
    if (vinState != null) {
      final idx = int.tryParse(vinState.group(1)!) ?? 0;
      if (idx >= 1 && idx <= 4) {
        vinStates[idx] = _toBoolPayload(payload);
        _notifySafely();
      }
    }
  }

  bool _toBoolPayload(String s) {
    final lower = s.toLowerCase();
    return lower == '1' || lower == 'on' || lower == 'online' || lower == 'true';
  }

  String _toRelayMode(String s) {
    final up = s.toUpperCase();
    if (up == 'FORCE_ON' || up == 'ON') return 'FORCE_ON';
    if (up == 'FORCE_OFF' || up == 'OFF') return 'FORCE_OFF';
    return 'AUTO';
  }

  MqttServerClient? get _activeClient {
    switch (activeTransport) {
      case ActiveTransport.local:
        return _clientLocal;
      case ActiveTransport.gsm:
        return _clientGsm;
      case ActiveTransport.none:
      case ActiveTransport.ble:
        return null;
    }
  }

  Future<void> publishRelaySet(int relay, String action) async {
    if (_disposed) return;
    _refreshConnectedFlags();
    MqttServerClient? client = _activeClient;
    if (client == null || !_isClientConnected(client)) {
      if (card.policy == TransportPolicy.auto ||
          card.policy == TransportPolicy.local ||
          _allowLocalRecovery) {
        await _tryGsmFallbackNow(silent: true);
      }
      _refreshConnectedFlags();
      client = _activeClient;
    }
    if (client == null || !_isClientConnected(client)) {
      statusText = 'Not connected over MQTT';
      _notifySafely();
      return;
    }

    final payload = MqttClientPayloadBuilder()..addString(action);
    client.publishMessage(
      '${card.baseTopic}/relay/$relay/set',
      MqttQos.atLeastOnce,
      payload.payload!,
      retain: card.retain,
    );
  }

  Future<void> publishVinSet(int vin, String action) async {
    if (_disposed) return;
    _refreshConnectedFlags();
    MqttServerClient? client = _activeClient;
    if (client == null || !_isClientConnected(client)) {
      if (card.policy == TransportPolicy.auto ||
          card.policy == TransportPolicy.local ||
          _allowLocalRecovery) {
        await _tryGsmFallbackNow(silent: true);
      }
      _refreshConnectedFlags();
      client = _activeClient;
    }
    if (client == null || !_isClientConnected(client)) {
      statusText = 'Not connected over MQTT';
      _notifySafely();
      return;
    }

    final payload = MqttClientPayloadBuilder()..addString(action);
    client.publishMessage(
      '${card.baseTopic}/vin/$vin/set',
      MqttQos.atLeastOnce,
      payload.payload!,
      retain: card.retain,
    );
  }

  Future<void> disconnect({bool silent = false}) async {
    if (_disposed) return;
    _manualDisconnectInProgress = true;
    _retryTimer?.cancel();
    _retryTimer = null;
    await _disconnectLocal();
    await _disconnectGsm();
    _allowLocalRecovery = false;
    _manualDisconnectInProgress = false;

    if (!silent) statusText = 'Disconnected';
    activeTransport = bleAvailable ? ActiveTransport.ble : ActiveTransport.none;
    if (activeTransport == ActiveTransport.ble && card.policy == TransportPolicy.ble) {
      statusText = 'BLE mode';
    }
    _notifySafely();
  }

  Future<void> _disconnectLocal() async {
    await _subLocal?.cancel();
    _subLocal = null;
    _clientLocal?.disconnect();
    _clientLocal = null;
    localConnected = false;
    _localCardOnline = false;
  }

  Future<void> _disconnectGsm() async {
    await _subGsm?.cancel();
    _subGsm = null;
    _clientGsm?.disconnect();
    _clientGsm = null;
    gsmConnected = false;
  }

  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _manualDisconnectInProgress = true;
    _retryTimer?.cancel();
    _retryTimer = null;
    unawaited(_subLocal?.cancel());
    _subLocal = null;
    unawaited(_subGsm?.cancel());
    _subGsm = null;
    _clientLocal?.disconnect();
    _clientLocal = null;
    _clientGsm?.disconnect();
    _clientGsm = null;
    localConnected = false;
    gsmConnected = false;
    _localCardOnline = false;
    connecting = false;
    _allowLocalRecovery = false;
    super.dispose();
  }
}

class BleScanResult {
  BleScanResult({
    required this.cardId,
    required this.suggestedName,
    required this.device,
    this.serviceUuid,
  });

  final String cardId;
  final String suggestedName;
  final BluetoothDevice device;
  final Guid? serviceUuid;
}

class BleScannerPage extends StatefulWidget {
  const BleScannerPage({super.key, this.expectedCardId});

  final String? expectedCardId;

  @override
  State<BleScannerPage> createState() => _BleScannerPageState();
}

class _BleScannerPageState extends State<BleScannerPage> {
  final Map<String, BleScanResult> _found = <String, BleScanResult>{};
  final Set<String> _resolving = <String>{};
  StreamSubscription<List<ScanResult>>? _scanSub;
  Timer? _scanTimer;
  bool _scanning = false;
  String _status = 'Scan BLE...';
  bool _autoReturned = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _startScan();
    });
  }

  @override
  void dispose() {
    _scanTimer?.cancel();
    _scanSub?.cancel();
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  Future<bool> _ensurePermissions() async {
    if (kIsWeb || defaultTargetPlatform != TargetPlatform.android) return true;
    final scan = await Permission.bluetoothScan.request();
    final connect = await Permission.bluetoothConnect.request();
    final loc = await Permission.locationWhenInUse.request();
    return scan.isGranted && connect.isGranted && loc.isGranted;
  }

  Future<void> _startScan() async {
    final ok = await _ensurePermissions();
    if (!ok) {
      setState(() => _status = 'Permissions BLE requises');
      return;
    }
    _found.clear();
    setState(() {
      _scanning = true;
      _status = 'Scanning BLE...';
    });

    _scanSub?.cancel();
    _scanSub = FlutterBluePlus.scanResults.listen((results) {
      for (final r in results) {
        final name = _deviceName(r);
        final serviceUuid = _extractEspServiceUuidFromScan(r);
        if (serviceUuid != null) {
          final cardId = _extractCardIdFromServiceUuid(serviceUuid);
          if (cardId != null && EspCard.isValidCardId(cardId)) {
            _found[cardId] = BleScanResult(
              cardId: cardId,
              suggestedName: name.isEmpty ? 'ESPRelay4-$cardId' : name,
              device: r.device,
              serviceUuid: serviceUuid,
            );
            continue;
          }
        }

        // Some Android scans do not expose service UUIDs in advertisement.
        // Fallback: for ESPRelay-looking names, resolve card ID from services.
        if (_looksLikeEspRelayName(name)) {
          unawaited(_resolveCardIdFromServices(r.device, suggestedName: name));
        }
      }
      final expected = widget.expectedCardId;
      if (!_autoReturned &&
          expected != null &&
          expected.isNotEmpty &&
          _found.containsKey(expected)) {
        _autoReturned = true;
        FlutterBluePlus.stopScan();
        if (mounted) Navigator.of(context).pop(_found[expected]);
        return;
      }
      if (mounted) setState(() {});
    });

    await FlutterBluePlus.startScan(timeout: const Duration(seconds: 6));
    _scanTimer?.cancel();
    _scanTimer = Timer(const Duration(seconds: 6), () async {
      await FlutterBluePlus.stopScan();
      if (!mounted) return;
      setState(() {
        _scanning = false;
        _status = _found.isEmpty ? 'Aucune carte BLE trouvée' : 'Scan terminé';
      });
    });
  }

  String _deviceName(ScanResult r) {
    final n1 = r.device.platformName.trim();
    if (n1.isNotEmpty) return n1;
    final n2 = r.advertisementData.advName.trim();
    return n2;
  }

  Guid? _extractEspServiceUuidFromScan(ScanResult r) {
    // Only trust ESPRelay4 custom service UUID:
    // 6E400001-B5A3-F393-E0A9-<CARD_ID_12_HEX>
    for (final uuid in r.advertisementData.serviceUuids) {
      final id = _extractCardIdFromServiceUuid(uuid);
      if (id != null) return uuid;
    }
    for (final uuid in r.advertisementData.serviceData.keys) {
      final id = _extractCardIdFromServiceUuid(uuid);
      if (id != null) return uuid;
    }
    return null;
  }

  bool _looksLikeEspRelayName(String name) {
    final n = name.trim().toUpperCase();
    return n.startsWith('ESPRELAY4-');
  }

  Future<void> _resolveCardIdFromServices(
    BluetoothDevice device, {
    required String suggestedName,
  }) async {
    final deviceKey = device.remoteId.toString();
    if (_resolving.contains(deviceKey)) return;
    _resolving.add(deviceKey);
    bool connectedHere = false;
    try {
      try {
        await device.connect(
          timeout: const Duration(seconds: 5),
          autoConnect: false,
        );
        connectedHere = true;
      } catch (_) {
        // already connected or temporary race
      }

      final services = await device.discoverServices();
      Guid? matched;
      for (final s in services) {
        final id = _extractCardIdFromServiceUuid(s.uuid);
        if (id != null) {
          matched = s.uuid;
          break;
        }
      }
      if (matched == null) return;
      final cardId = _extractCardIdFromServiceUuid(matched);
      if (cardId == null || !EspCard.isValidCardId(cardId)) return;

      _found[cardId] = BleScanResult(
        cardId: cardId,
        suggestedName:
            suggestedName.isEmpty ? 'ESPRelay4-$cardId' : suggestedName,
        device: device,
        serviceUuid: matched,
      );

      final expected = widget.expectedCardId;
      if (!_autoReturned &&
          expected != null &&
          expected.isNotEmpty &&
          expected == cardId) {
        _autoReturned = true;
        await FlutterBluePlus.stopScan();
        if (mounted) Navigator.of(context).pop(_found[expected]);
        return;
      }

      if (mounted) setState(() {});
    } catch (_) {
      // ignore fallback errors
    } finally {
      if (connectedHere) {
        try {
          await device.disconnect();
        } catch (_) {}
      }
      _resolving.remove(deviceKey);
    }
  }

  String? _extractCardIdFromServiceUuid(Guid serviceUuid) {
    final s = serviceUuid.toString().toUpperCase();
    const prefix = '6E400001-B5A3-F393-E0A9-';
    if (!s.startsWith(prefix)) return null;
    final suffix = s.substring(prefix.length);
    if (!RegExp(r'^[0-9A-F]{12}$').hasMatch(suffix)) return null;
    return suffix;
  }

  @override
  Widget build(BuildContext context) {
    final list = _found.values.toList()
      ..sort((a, b) => a.cardId.compareTo(b.cardId));
    final expected = widget.expectedCardId;
    final visible = (expected != null && expected.isNotEmpty)
        ? list.where((e) => e.cardId == expected).toList()
        : list;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Scan BLE'),
        actions: [
          IconButton(
            onPressed: _scanning ? null : _startScan,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text(_status, style: const TextStyle(fontWeight: FontWeight.w700)),
          const SizedBox(height: 10),
          if (visible.isEmpty)
            const Card(
              child: Padding(
                padding: EdgeInsets.all(12),
                child: Text('Aucun ID carte BLE détecté.'),
              ),
            ),
          ...visible.map((e) {
            return Card(
              elevation: 0,
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(12),
                side: const BorderSide(color: Color(0xFFDADADA)),
              ),
              child: ListTile(
                title: Text(
                  e.cardId,
                  style: const TextStyle(
                    fontFamily: 'monospace',
                    fontSize: 14,
                    fontWeight: FontWeight.w700,
                  ),
                ),
                trailing: const Icon(Icons.chevron_right),
                onTap: () => Navigator.of(context).pop(e),
              ),
            );
          }),
        ],
      ),
    );
  }
}

class QrScanPage extends StatefulWidget {
  const QrScanPage({super.key});

  @override
  State<QrScanPage> createState() => _QrScanPageState();
}

class _QrScanPageState extends State<QrScanPage> {
  final MobileScannerController _controller = MobileScannerController(
    detectionSpeed: DetectionSpeed.noDuplicates,
    facing: CameraFacing.back,
    torchEnabled: false,
  );

  bool _done = false;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _onDetect(BarcodeCapture capture) {
    if (_done) return;
    for (final b in capture.barcodes) {
      final raw = b.rawValue?.trim();
      if (raw == null || raw.isEmpty) continue;
      _done = true;
      Navigator.of(context).pop(raw);
      return;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Scanner QR Card ID')),
      body: Stack(
        children: [
          MobileScanner(controller: _controller, onDetect: _onDetect),
          Align(
            alignment: Alignment.bottomCenter,
            child: Container(
              width: double.infinity,
              padding: const EdgeInsets.all(16),
              color: Colors.black.withValues(alpha: 0.45),
              child: const Text(
                'Cadre le QR code de la carte pour récupérer le Card ID.',
                textAlign: TextAlign.center,
                style: TextStyle(color: Colors.white),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

String? extractCardIdFromAny(String raw) {
  final text = raw.trim();
  if (text.isEmpty) return null;

  final macWithSep = RegExp(r'([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}').firstMatch(text);
  if (macWithSep != null) {
    final c = EspCard.normalizeCardId(macWithSep.group(0)!);
    if (EspCard.isValidCardId(c)) return c;
  }

  final directHex = RegExp(r'[0-9A-Fa-f]{12}').firstMatch(text);
  if (directHex != null) {
    final c = directHex.group(0)!.toUpperCase();
    if (EspCard.isValidCardId(c)) return c;
  }

  final normalized = EspCard.normalizeCardId(text);
  if (EspCard.isValidCardId(normalized)) return normalized;
  if (normalized.length > 12) {
    final tail = normalized.substring(normalized.length - 12);
    if (EspCard.isValidCardId(tail)) return tail;
  }

  return null;
}
